#include <pthread.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/buffer.h>

#include "postgres.h"

/* These are always necessary for a bgworker */
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"

/* these headers are used by this particular worker's code */
#include "pgstat.h"
#include "tcop/utility.h"

#include "postgres_stats.h"
#include "disk_stats.h"
#include "system_stats.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(bg_mon_launch);

void _PG_init(void);
void bg_mon_main(Datum);

extern int MaxConnections;

pthread_mutex_t lock;

/* flags set by signal handlers */
static volatile sig_atomic_t got_sighup = false;
static volatile sig_atomic_t got_sigterm = false;

/* GUC variables */
static int bg_mon_naptime = 1;

static pg_stat_list pg_stats_current;
static disk_stat diskspace_stats_current;
static system_stat system_stats_current;

static void report_stats()
{
}

/*
 * Signal handler for SIGTERM
 *		Set a flag to let the main loop to terminate, and set our latch to wake
 *		it up.
 */
static void
bg_mon_sigterm(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sigterm = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

/*
 * Signal handler for SIGHUP
 *		Set a flag to tell the main loop to reread the config file, and set
 *		our latch to wake it up.
 */
static void
bg_mon_sighup(SIGNAL_ARGS)
{
	int			save_errno = errno;

	got_sighup = true;
	if (MyProc)
		SetLatch(&MyProc->procLatch);

	errno = save_errno;
}

/*
 * Initialize workspace for a worker process: create the schema if it doesn't
 * already exist.
 */
static void
initialize_bg_mon()
{
	postgres_stats_init();
	disk_stats_init();
	system_stats_init();
}

static void
send_document_cb(struct evhttp_request *req, void *arg)
{
	bool is_first = true;
	size_t i;
	disk_stat d;
	system_stat s;
	cpu_stat c;
	meminfo m;
	load_avg la;

	struct evbuffer *evb = NULL;

	evhttp_add_header(evhttp_request_get_output_headers(req), "Content-Type", "application/json");

	evb = evbuffer_new();
	pthread_mutex_lock(&lock);
	d = diskspace_stats_current;
	s = system_stats_current;
	c = s.cpu;
	m = s.mem;
	la = s.load_avg;

	evbuffer_add_printf(evb, "{\"hostname\": \"%s\", \"sysname\": \"Linux: %s\", ", s.hostname, s.sysname);
	evbuffer_add_printf(evb, "\"cpu_cores\": %d, \"connections\": {\"max\": %d, ", c.cpu_count, MaxConnections); 
	evbuffer_add_printf(evb, "\"total\": %d, ", pg_stats_current.total_connections);
	evbuffer_add_printf(evb, "\"active\": %d}, ", pg_stats_current.active_connections);
	evbuffer_add_printf(evb, "\"system_stats\": {\"uptime\": %d, \"load_average\": ", s.uptime);
	evbuffer_add_printf(evb, "[%4.6g, %4.6g, %4.6g], \"cpu\": ", la.run_1min, la.run_5min, la.run_15min);
	evbuffer_add_printf(evb, "{\"user\": %2.1f, \"system\": %2.1f, \"idle\": ", c.utime_diff, c.stime_diff);
	evbuffer_add_printf(evb, "%2.1f}, \"iowait\": %2.1f, \"ctxt\": %lu", c.idle_diff, c.iowait_diff, s.ctxt_diff);
	evbuffer_add_printf(evb, ", \"processes\": {\"running\": %lu, \"blocked\": %lu}, ", s.procs_running, s.procs_blocked);
	evbuffer_add_printf(evb, "\"memory\": {\"total\": %lu, \"free\": %lu, \"buffers\": %lu", m.total, m.free, m.buffers);
	evbuffer_add_printf(evb, ", \"cached\": %lu, \"dirty\": %lu, \"commit_limit\": %lu, ", m.cached, m.dirty, m.limit);
	evbuffer_add_printf(evb, "\"committed_as\": %lu}}, \"disk_stats\": {\"data\": {\"device\": {\"name\": ", m.as);
	evbuffer_add_printf(evb, "\"%s\", \"space\": {\"total\": %llu, \"left\": %llu", d.data_dev, d.data_size, d.data_free);
	evbuffer_add_printf(evb, "}, \"io\": {\"read\": %u, \"write\": %u, ", d.data_read_diff, d.data_write_diff);
	evbuffer_add_printf(evb, "\"await\": %u}}, \"directory\": {\"name\": \"%s\"", d.data_time_in_queue_diff, d.data_directory);
	evbuffer_add_printf(evb, ", \"size\": %llu}}, \"xlog\": {\"device\": {\"name\": \"%s\", ", d.du_data, d.xlog_dev);
	evbuffer_add_printf(evb, "\"space\": {\"total\": %llu, \"left\": %llu}, \"io\": ", d.xlog_size, d.xlog_free);
	evbuffer_add_printf(evb, "{\"read\": %u, \"write\": %u, \"await\": ", d.xlog_read_diff, d.xlog_write_diff);
	evbuffer_add_printf(evb, "%u}}, \"directory\": {\"name\": \"%s\", ", d.xlog_time_in_queue_diff, d.xlog_directory);
	evbuffer_add_printf(evb, "\"size\": %llu}}}, \"processes\": [", d.xlog_size);

	for (i = 0; i < pg_stats_current.pos; ++i) {
		pg_stat s = pg_stats_current.values[i];
		if (!s.is_backend || s.query != NULL) {
			proc_stat ps = s.ps;
			proc_io io = ps.io;
			char *type = s.is_backend ? "backend" : ps.cmdline == NULL ? "unknown" : ps.cmdline;
			if (is_first) is_first = false;
			else evbuffer_add_printf(evb, ", ");
			evbuffer_add_printf(evb, "{\"pid\": %d, \"type\": \"%s\", \"state\": \"%c\", ", s.pid, type, ps.state);
			evbuffer_add_printf(evb, "\"cpu\": {\"user\": %2.1f, \"system\": %2.1f, ", ps.utime_diff, ps.stime_diff);
			evbuffer_add_printf(evb, "\"guest\": %2.1f}, \"io\": {\"read\": %lu, ", ps.guest_time_diff, io.read_diff);
			evbuffer_add_printf(evb, "\"write\": %lu}, \"uss\": %llu", io.write_diff, ps.uss);
			if (s.is_backend) {
				if (s.age > -1)
					evbuffer_add_printf(evb, ", \"age\": %d", s.age);

				evbuffer_add_printf(evb, ", \"database\": %s, ", s.datname == NULL ? "null" : s.datname);
				evbuffer_add_printf(evb, "\"username\": %s, ", s.usename == NULL ? "null" : s.usename);
				evbuffer_add_printf(evb, "\"query\": %s", s.query);
				if (s.locked_by != NULL)
					evbuffer_add_printf(evb, ", \"locked_by\": [%s]", s.locked_by);
			}
			evbuffer_add_printf(evb, "}");
		}
	}

	evbuffer_add_printf(evb, "]}");

	pthread_mutex_unlock(&lock);
	evhttp_send_reply(req, 200, "OK", evb);
	evbuffer_free(evb);
}

static void *webapi(void *arg)
{
	struct event_base *base = (struct event_base *)arg;

	event_base_dispatch(base);

	return (void *)0;
}

void
bg_mon_main(Datum main_arg)
{
	unsigned short port = 8080;
	pthread_t thread;
	struct timezone tz;
	struct timeval current_time, next_run;

	struct event_base *base;
	struct evhttp *http;
	struct evhttp_bound_socket *handle;


	/* Establish signal handlers before unblocking signals. */
	pqsignal(SIGHUP, bg_mon_sighup);
	pqsignal(SIGTERM, bg_mon_sigterm);

	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		proc_exit(1);

	if (!(base = event_base_new())) {
		elog(ERROR, "Couldn't create an event_base: exiting");
		return proc_exit(1);
	}

	/* Create a new evhttp object to handle requests. */
	if (!(http = evhttp_new(base))) {
		elog(ERROR, "couldn't create evhttp. Exiting.");
		return proc_exit(1);
	}

	/* We want to accept arbitrary requests, so we need to set a "generic"
	 * cb.  We can also add callbacks for specific paths. */
	evhttp_set_gencb(http, send_document_cb, NULL);

	/* Now we tell the evhttp what port to listen on */
	if (!(handle = evhttp_bind_socket_with_handle(http, "0.0.0.0", port))) {
		elog(ERROR, "couldn't bind to port %d. Exiting.\n", (int)port);
		return proc_exit(1);
	}

	pthread_mutex_init(&lock, NULL);

	pthread_create(&thread, NULL, webapi, base);

	/* We're now ready to receive signals */
	BackgroundWorkerUnblockSignals();

	/* Connect to our database */
	BackgroundWorkerInitializeConnection("postgres", NULL);

	initialize_bg_mon();

	gettimeofday(&next_run, &tz);

	/*
	 * Main loop: do this until the SIGTERM handler tells us to terminate
	 */
	while (!got_sigterm) {
		long double	naptime = bg_mon_naptime * 1000L;

		next_run.tv_sec += bg_mon_naptime;
		gettimeofday(&current_time, &tz);

		naptime = 1000L * (
			(double)next_run.tv_sec + (double)next_run.tv_usec/1000000.0 -
			(double)current_time.tv_sec - (double)current_time.tv_usec/1000000.0
		);

		if (naptime <= 0) { // something is very slow
			next_run = current_time; // reschedule next run
		} else {
			int rc = WaitLatch(&MyProc->procLatch,
						   WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH,
						   naptime);

			ResetLatch(&MyProc->procLatch);

			/* emergency bailout if postmaster has died */
			if (rc & WL_POSTMASTER_DEATH)
				proc_exit(1);
		}

		/*
		 * In case of a SIGHUP, just reload the configuration.
		 */
		if (got_sighup) {
			got_sighup = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		{
			disk_stat d =  get_diskspace_stats();
			pg_stat_list p = get_postgres_stats();
			system_stat s = get_system_stats();

			pthread_mutex_lock(&lock);

			diskspace_stats_current = d;
			pg_stats_current = p;
			system_stats_current = s;

			pthread_mutex_unlock(&lock);
		}

		report_stats();
	}

	proc_exit(1);
}

/*
 * Entrypoint of this module.
 *
 * We register more than one worker process here, to demonstrate how that can
 * be done.
 */
void
_PG_init(void)
{
	BackgroundWorker worker;

	/* get the configuration */
	DefineCustomIntVariable("bg_mon.naptime",
							"Duration between each run (in seconds).",
							NULL,
							&bg_mon_naptime,
							1,
							1,
							10,
							PGC_SIGHUP,
							0,
							NULL,
							NULL,
							NULL);

	if (!process_shared_preload_libraries_in_progress)
		return;

	/* set up common data for all our workers */
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main = bg_mon_main;
	worker.bgw_notify_pid = 0;

	/*
	 * Now fill in worker-specific data, and do the actual registrations.
	 */
	snprintf(worker.bgw_name, BGW_MAXLEN, "bg_mon");
//	worker.bgw_main_arg = Int32GetDatum(i);

	RegisterBackgroundWorker(&worker);
}

/*
 * Dynamically launch an SPI worker.
 */
Datum
bg_mon_launch(PG_FUNCTION_ARGS)
{
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;
	BgwHandleStatus status;
	pid_t		pid;

	worker.bgw_flags = BGWORKER_SHMEM_ACCESS |
		BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main = NULL;		/* new worker might not have library loaded */
	sprintf(worker.bgw_library_name, "bg_mon");
	sprintf(worker.bgw_function_name, "bg_mon_main");
	snprintf(worker.bgw_name, BGW_MAXLEN, "bg_mon");
//	worker.bgw_main_arg = Int32GetDatum(i);
	/* set bgw_notify_pid so that we can use WaitForBackgroundWorkerStartup */
	worker.bgw_notify_pid = MyProcPid;

	if (!RegisterDynamicBackgroundWorker(&worker, &handle))
		PG_RETURN_NULL();

	status = WaitForBackgroundWorkerStartup(handle, &pid);

	if (status == BGWH_STOPPED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
				 errmsg("could not start background process"),
			   errhint("More details may be available in the server log.")));
	if (status == BGWH_POSTMASTER_DIED)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_RESOURCES),
			  errmsg("cannot start background processes without postmaster"),
				 errhint("Kill all remaining database processes and restart the database.")));
	Assert(status == BGWH_STARTED);

	PG_RETURN_INT32(pid);
}
