/* $%BEGINLICENSE%$
 Copyright (C) 2007-2008 MySQL AB, 2008 Sun Microsystems, Inc

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 $%ENDLICENSE%$ */
 
/** @file
 * the user-interface for the MySQL Proxy @see main()
 *
 *  -  command-line handling 
 *  -  config-file parsing
 * 
 *
 * network_mysqld_thread() is the real proxy thread 
 * 
 * @todo move the SQL based help out into a lua script
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_SIGNAL_H
#include <signal.h>
#endif
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#ifdef _WIN32
#include <process.h> /* getpid() */
#include <io.h>      /* open() */
#else
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h> /* for rusage in wait() */
#endif

#include <glib.h>
#include <gmodule.h>

#ifdef HAVE_LUA_H
#include <lua.h>
#include <stdio.h>
#endif

#ifdef HAVE_VALGRIND_VALGRIND_H
#include <valgrind/valgrind.h>
#endif

#ifndef HAVE_VALGRIND_VALGRIND_H
#define RUNNING_ON_VALGRIND 0
#endif


#include "network-mysqld.h"
#include "network-mysqld-proto.h"
#include "sys-pedantic.h"

#include "chassis-log.h"
#include "chassis-keyfile.h"
#include "chassis-mainloop.h"
#include "chassis-path.h"
#include "chassis-limits.h"

#ifdef _WIN32
static char **shell_argv;
static int shell_argc;
static int win32_running_as_service = 0;
static SERVICE_STATUS agent_service_status;
static SERVICE_STATUS_HANDLE agent_service_status_handle = 0;
#endif

#ifdef WIN32
#define CHASSIS_NEWLINE "\r\n"
#else
#define CHASSIS_NEWLINE "\n"
#endif

#ifndef _WIN32
/**
 * start the app in the background 
 * 
 * UNIX-version
 */
static void daemonize(void) {
#ifdef SIGTTOU
	signal(SIGTTOU, SIG_IGN);
#endif
#ifdef SIGTTIN
	signal(SIGTTIN, SIG_IGN);
#endif
#ifdef SIGTSTP
	signal(SIGTSTP, SIG_IGN);
#endif
	if (fork() != 0) exit(0);
	
	if (setsid() == -1) exit(0);

	signal(SIGHUP, SIG_IGN);

	if (fork() != 0) exit(0);
	
	chdir("/");
	
	umask(0);
}


/**
 * forward the signal to the process group, but not us
 */
static void signal_forward(int sig) {
	signal(sig, SIG_IGN); /* we don't want to create a loop here */

	kill(0, sig);
}

/**
 * keep the ourself alive 
 *
 * if we or the child gets a SIGTERM, we quit too
 * on everything else we restart it
 */
static int proc_keepalive(int *child_exit_status) {
	int nprocs = 0;
	pid_t child_pid = -1;

	/* we ignore SIGINT and SIGTERM and just let it be forwarded to the child instead
	 * as we want to collect its PID before we shutdown too 
	 *
	 * the child will have to set its own signal handlers for this
	 */

	for (;;) {
		/* try to start the children */
		while (nprocs < 1) {
			pid_t pid = fork();

			if (pid == 0) {
				/* child */
				
				g_debug("%s: we are the child: %d",
						G_STRLOC,
						getpid());
				return 0;
			} else if (pid < 0) {
				/* fork() failed */

				g_critical("%s: fork() failed: %s (%d)",
					G_STRLOC,
					g_strerror(errno),
					errno);

				return -1;
			} else {
				/* we are the angel, let's see what the child did */
				g_message("%s: [angel] we try to keep PID=%d alive",
						G_STRLOC,
						pid);

				signal(SIGINT, signal_forward);
				signal(SIGTERM, signal_forward);
				signal(SIGHUP, signal_forward);

				child_pid = pid;
				nprocs++;
			}
		}

		if (child_pid != -1) {
			struct rusage rusage;
			int exit_status;
			pid_t exit_pid;

			g_debug("%s: waiting for %d",
					G_STRLOC,
					child_pid);
			exit_pid = wait4(child_pid, &exit_status, 0, &rusage);
			g_debug("%s: %d returned: %d",
					G_STRLOC,
					child_pid,
					exit_pid);

			if (exit_pid == child_pid) {
				/* our child returned, let's see how it went */
				if (WIFEXITED(exit_status)) {
					g_message("%s: [angel] PID=%d exited normally with exit-code = %d (it used %ld kBytes max)",
							G_STRLOC,
							child_pid,
							WEXITSTATUS(exit_status),
							rusage.ru_maxrss / 1024);
					if (child_exit_status) *child_exit_status = WEXITSTATUS(exit_status);
					return 1;
				} else if (WIFSIGNALED(exit_status)) {
					int time_towait = 2;
					/* our child died on a signal
					 *
					 * log it and restart */

					g_message("%s: [angel] PID=%d died on signal=%d (it used %ld kBytes max) ... waiting 3min before restart",
							G_STRLOC,
							child_pid,
							WTERMSIG(exit_status),
							rusage.ru_maxrss / 1024);

					/**
					 * to make sure we don't loop as fast as we can, sleep a bit between 
					 * restarts
					 */
	
					signal(SIGINT, SIG_DFL);
					signal(SIGTERM, SIG_DFL);
					signal(SIGHUP, SIG_DFL);
					while (time_towait > 0) time_towait = sleep(time_towait);

					nprocs--;
					child_pid = -1;
				} else if (WIFSTOPPED(exit_status)) {
				} else {
					g_assert_not_reached();
				}
			} else if (-1 == exit_pid) {
				if (EINTR == errno) {
					/* EINTR is ok, all others bad */
				} else {
					/* how can this happen ? */
					g_critical("%s: wait4(%d, ...) failed: %s (%d)",
						G_STRLOC,
						child_pid,
						g_strerror(errno),
						errno);

					return -1;
				}
			} else {
				g_assert_not_reached();
			}
		}
	}

	return 1;
}
#endif

#define GETTEXT_PACKAGE "mysql-proxy"

#ifdef _WIN32
/* win32 service */

void agent_service_set_state(DWORD new_state, int wait_msec) {
	DWORD status;
	
	/* safeguard against a missing if(win32_running_as_service) in other code */
	if (!win32_running_as_service) return;
	g_assert(agent_service_status_handle);
	
	switch(new_state) {
		case SERVICE_START_PENDING:
		case SERVICE_STOP_PENDING:
			agent_service_status.dwWaitHint = wait_msec;
			
			if (agent_service_status.dwCurrentState == new_state) {
				agent_service_status.dwCheckPoint++;
			} else {
				agent_service_status.dwCheckPoint = 0;
			}
			
			break;
		default:
			agent_service_status.dwWaitHint = 0;
			break;
	}
	
	agent_service_status.dwCurrentState = new_state;
	
	if (!SetServiceStatus (agent_service_status_handle, &agent_service_status)) {
		status = GetLastError();
	}
}
#endif

static void sigsegv_handler(int G_GNUC_UNUSED signum) {
	g_on_error_stack_trace(g_get_prgname());

	abort(); /* trigger a SIGABRT instead of just exiting */
}

/**
 * This is the "real" main which is called both on Windows and UNIX platforms.
 * For the Windows service case, this will also handle the notifications and set
 * up the logging support appropriately.
 */
int main_cmdline(int argc, char **argv) {
	chassis *srv;
#ifdef HAVE_SIGACTION
	static struct sigaction sigsegv_sa;
#endif
	/* read the command-line options */
	GOptionContext *option_ctx;
	GError *gerr = NULL;
	guint i;
	int exit_code = EXIT_SUCCESS;
	const gchar *exit_location = G_STRLOC;
	int print_version = 0;
	int daemon_mode = 0;
	gchar *user = NULL;
	gchar *base_dir = NULL;
	int auto_base_dir = 0;	/**< distinguish between user supplied basedir and automatically discovered one */
	const gchar *check_str = NULL;
	chassis_plugin *p;
	gchar *pid_file = NULL;
	gchar *plugin_dir = NULL;
	gchar *default_file = NULL;
	GOptionEntry *config_entries;
	gchar **plugin_names = NULL;
	guint invoke_dbg_on_crash = 0;
	guint auto_restart = 0;
	guint max_files_number = 8192;
	gint event_thread_count = 1; /* there is always the main-thread */
#ifdef WIN32
	WSADATA wsaData;
#endif
	gchar *log_level = NULL;

	GKeyFile *keyfile = NULL;
	chassis_log *log;

	/* can't appear in the configfile */
	GOptionEntry base_main_entries[] = 
	{
		{ "version",                 'V', 0, G_OPTION_ARG_NONE, NULL, "Show version", NULL },
		{ "defaults-file",            0, 0, G_OPTION_ARG_STRING, NULL, "configuration file", "<file>" },
		
		{ NULL,                       0, 0, G_OPTION_ARG_NONE,   NULL, NULL, NULL }
	};

	GOptionEntry main_entries[] = 
	{
		{ "daemon",                   0, 0, G_OPTION_ARG_NONE, NULL, "Start in daemon-mode", NULL },
#ifndef _WIN32
		{ "user",                     0, 0, G_OPTION_ARG_STRING, NULL, "Run mysql-proxy as user", "<user>" },
#endif
		{ "basedir",                  0, 0, G_OPTION_ARG_STRING, NULL, "Base directory to prepend to relative paths in the config", "<absolute path>" },
		{ "pid-file",                 0, 0, G_OPTION_ARG_STRING, NULL, "PID file in case we are started as daemon", "<file>" },
		{ "plugin-dir",               0, 0, G_OPTION_ARG_STRING, NULL, "path to the plugins", "<path>" },
		{ "plugins",                  0, 0, G_OPTION_ARG_STRING_ARRAY, NULL, "plugins to load", "<name>" },
		{ "log-level",                0, 0, G_OPTION_ARG_STRING, NULL, "log all messages of level ... or higer", "(error|warning|info|message|debug)" },
		{ "log-config-file",          0, 0, G_OPTION_ARG_FILENAME, NULL, "Use extended logging configuration", "<file>" },
		{ "log-file",                 0, 0, G_OPTION_ARG_STRING, NULL, "log all messages in a file", "<file>" },
		{ "log-use-syslog",           0, 0, G_OPTION_ARG_NONE, NULL, "log all messages to syslog", NULL },
		{ "log-backtrace-on-crash",   0, 0, G_OPTION_ARG_NONE, NULL, "try to invoke debugger on crash", NULL },
		{ "keepalive",                0, 0, G_OPTION_ARG_NONE, NULL, "try to restart the proxy if it crashed", NULL },
		{ "max-open-files",           0, 0, G_OPTION_ARG_INT, NULL, "maximum number of open files (ulimit -n)", NULL},
		{ "event-threads",            0, 0, G_OPTION_ARG_INT, NULL, "number of event-handling threads (default: 1)", NULL},
		
		{ NULL,                       0, 0, G_OPTION_ARG_NONE,   NULL, NULL, NULL }
	};

#if 0
	g_mem_set_vtable(glib_mem_profiler_table);
#endif

	if (!GLIB_CHECK_VERSION(2, 6, 0)) {
		g_error("the glib header are too old, need at least 2.6.0, got: %d.%d.%d", 
				GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
	}

	check_str = glib_check_version(GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);

	if (check_str) {
		g_error("%s, got: lib=%d.%d.%d, headers=%d.%d.%d", 
			check_str,
			glib_major_version, glib_minor_version, glib_micro_version,
			GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
	}

	if (!g_module_supported()) {
		g_error("loading modules is not supported on this platform");
	}

	g_thread_init(NULL);

	log = chassis_log_new();
	log->min_lvl = G_LOG_LEVEL_MESSAGE; /* display messages while parsing or loading plugins */
	g_log_set_default_handler(chassis_log_func, log);

#ifdef _WIN32
	if (0 != WSAStartup(MAKEWORD( 2, 2 ), &wsaData)) {
		g_critical("WSAStartup failed to initialize the socket library.\n");

		exit_code = EXIT_FAILURE; 
		exit_location = G_STRLOC;
		goto exit_nicely;
	}
	if (win32_running_as_service) {
		log->use_windows_applog = TRUE;
		log->event_source_handle = RegisterEventSource(NULL, "mysql-monitor-agent");	/* TODO: get the actual executable name here */
		if (!log->event_source_handle) {
			int err = GetLastError();
			g_critical("unhandled error-code (%d) for RegisterEventSource(), shutting down", err);
			exit_code = EXIT_FAILURE; 
			exit_location = G_STRLOC;
			goto exit_nicely;
		}
	}
#endif

	srv = chassis_new();
	srv->log = log; /* we need the log structure for the log-rotation */

	i = 0;
	base_main_entries[i++].arg_data  = &(print_version);
	base_main_entries[i++].arg_data  = &(default_file);

	i = 0;
	main_entries[i++].arg_data  = &(daemon_mode);
#ifndef _WIN32
	main_entries[i++].arg_data  = &(user);
#endif
	main_entries[i++].arg_data  = &(base_dir);
	main_entries[i++].arg_data  = &(pid_file);
	main_entries[i++].arg_data  = &(plugin_dir);
	main_entries[i++].arg_data  = &(plugin_names);

	main_entries[i++].arg_data  = &(log_level);
	main_entries[i++].arg_data  = &(log->log_config_filename);
	main_entries[i++].arg_data  = &(log->log_filename);
	main_entries[i++].arg_data  = &(log->use_syslog);
	main_entries[i++].arg_data  = &(invoke_dbg_on_crash);
	main_entries[i++].arg_data  = &(auto_restart);
	main_entries[i++].arg_data  = &(max_files_number);
	main_entries[i++].arg_data  = &(event_thread_count);

	option_ctx = g_option_context_new("- MySQL App Shell");
	g_option_context_add_main_entries(option_ctx, base_main_entries, GETTEXT_PACKAGE);
	g_option_context_set_help_enabled(option_ctx, FALSE);
	g_option_context_set_ignore_unknown_options(option_ctx, TRUE);

	/**
	 * parse once to get the basic options like --defaults-file and --version
	 *
	 * leave the unknown options in the list
	 */
	if (FALSE == g_option_context_parse(option_ctx, &argc, &argv, &gerr)) {
		g_critical("%s", gerr->message);
		
		exit_code = EXIT_FAILURE; 
		exit_location = G_STRLOC;
		goto exit_nicely;
	}

	if (default_file) {
		keyfile = g_key_file_new();
		g_key_file_set_list_separator(keyfile, ',');

		if (FALSE == g_key_file_load_from_file(keyfile, default_file, G_KEY_FILE_NONE, &gerr)) {
			g_critical("loading configuration from %s failed: %s", 
					default_file,
					gerr->message);

			exit_code = EXIT_FAILURE; 
			exit_location = G_STRLOC;
			goto exit_nicely;
		}
	}

	/* print the main version number here, but don't exit
	 * we check for print_version again, after loading the plugins (if any)
	 * and print their version numbers, too. then we exit cleanly.
	 */
	if (print_version) {
		printf("%s" CHASSIS_NEWLINE, PACKAGE_STRING); 
		printf("  glib2: %d.%d.%d" CHASSIS_NEWLINE, GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
#ifdef HAVE_EVENT_H
		printf("  libevent: %s" CHASSIS_NEWLINE, event_get_version());
#endif
	}
	
	/* add the other options which can also appear in the configfile */
	g_option_context_add_main_entries(option_ctx, main_entries, GETTEXT_PACKAGE);

	/**
	 * parse once to get the basic options 
	 *
	 * leave the unknown options in the list
	 */
	if (FALSE == g_option_context_parse(option_ctx, &argc, &argv, &gerr)) {
		g_critical("%s", gerr->message);

		exit_code = EXIT_FAILURE; 
		exit_location = G_STRLOC;
		goto exit_nicely;
	}

	if (keyfile) {
		if (chassis_keyfile_to_options(keyfile, "mysql-proxy", main_entries)) {
			exit_code = EXIT_FAILURE; 
			exit_location = G_STRLOC;
			goto exit_nicely;
		}
	}

	/* find our installation directory if no basedir was given
	 * this is necessary for finding files when we daemonize
	 */
	if (!base_dir) {
		base_dir = chassis_get_basedir(argv[0]);
		if (!base_dir) {
			g_critical("%s: Failed to get base directory.", G_STRLOC);
			goto exit_nicely;
		}

		auto_base_dir = 1;
	}
	
	/* --basedir must be an absolute path, doesn't make sense otherwise */
	if (!auto_base_dir && !g_path_is_absolute(base_dir)) {
		/* TODO: here we have a problem, because our logging support is not yet set up.
		   What do we do on Windows when called as a service?
		 */
		g_critical("--basedir option must be an absolute path, but was %s", base_dir);
		exit_code = EXIT_FAILURE; 
		exit_location = G_STRLOC;
		goto exit_nicely;
	}

	/* basic setup is done, base-dir is known, ... */

#if defined(HAVE_LUA_H)
	if (print_version) printf("  lua: %s" CHASSIS_NEWLINE, LUA_RELEASE);
	/**
	 * if the LUA_PATH or LUA_CPATH are not set, set a good default 
	 *
	 * we want to derive it from the basedir ...
	 */
	if (!g_getenv(LUA_PATH)) {
		gchar *path = g_build_filename(base_dir, "lib", "mysql-proxy", "lua", "?.lua", NULL);
#if _WIN32
		/** on Win32 glib uses _wputenv to set the env variable,
		 *  but Lua uses getenv. Those two don't see each other,
		 *  so we use _putenv. Since we only set ASCII chars, this
		 *  is safe.
		 */
		gchar *env_path = g_strdup_printf("%s=%s", LUA_PATH, path);
		_putenv(env_path);
		g_free(env_path);
#else
		g_setenv(LUA_PATH, path, 1);
#endif
		if (print_version) printf("    LUA_PATH: %s" CHASSIS_NEWLINE, path);

		g_free(path);
	} else {
		if (print_version) printf("    LUA_PATH: %s" CHASSIS_NEWLINE, g_getenv(LUA_PATH));
	}

	if (!g_getenv(LUA_CPATH)) {
		/* each OS has its own way of declaring a shared-lib extension
		 *
		 * win32 has .dll
		 * macosx has .so or .dylib
		 * hpux has .sl
		 */ 
#  if _WIN32
		gchar *path = g_build_filename(base_dir, "bin", "lua-?." G_MODULE_SUFFIX, NULL);
		gchar *env_path = g_strdup_printf("%s=%s", LUA_CPATH, path);
		_putenv(env_path);
		g_free(env_path);
#  else
		gchar *path = g_build_filename(base_dir, "lib", "mysql-proxy", "lua", "?." G_MODULE_SUFFIX, NULL);
		g_setenv(LUA_CPATH, path, 1);
#  endif
		if (print_version) printf("    LUA_CPATH: %s" CHASSIS_NEWLINE, path);

		g_free(path);
	} else {
		if (print_version) printf("    LUA_CPATH: %s" CHASSIS_NEWLINE, g_getenv(LUA_CPATH));
	}
# endif

	/* assign the mysqld part to the */
	network_mysqld_init(srv); /* starts the also the lua-scope, LUA_PATH and LUA_CPATH have to be set before this being called */


#ifdef HAVE_SIGACTION
	/* register the sigsegv interceptor */

	g_set_prgname(argv[0]); /* for g_on_error_stack_frame() */

	memset(&sigsegv_sa, 0, sizeof(sigsegv_sa));
	sigsegv_sa.sa_handler = sigsegv_handler;
	sigemptyset(&sigsegv_sa.sa_mask);

	if (invoke_dbg_on_crash && !(RUNNING_ON_VALGRIND)) {
		sigaction(SIGSEGV, &sigsegv_sa, NULL);
	}
#endif


	/*
	 * some plugins cannot see the chassis struct from the point
	 * where they open files, hence we must make it available
	 */
	srv->base_dir = g_strdup(base_dir);
	
	/* Lets find the plugin directory relative the executable path */
	if (!plugin_dir) {
#ifdef WIN32
		plugin_dir = g_build_filename(srv->base_dir, "bin", NULL);
#else
		plugin_dir = g_build_filename(srv->base_dir, "lib", PACKAGE, "plugins", NULL);
#endif
	}
	/* 
	 * these are used before we gathered all the options
	 * from the plugins, thus we need to fix them up before
	 * dealing with all the rest.
	 */
	chassis_resolve_path(srv, &log->log_config_filename);
	chassis_resolve_path(srv, &log->log_filename);
	chassis_resolve_path(srv, &pid_file);
	chassis_resolve_path(srv, &plugin_dir);

	/*
	 * If we have a log config file, it takes precendence before the simple other log-* options.
	 */
	if (log->log_config_filename) {
		chassis_log_extended_t *log_ext;
		log_ext = chassis_log_extended_new();
		log->log_ext = log_ext;
		chassis_log_load_config(log_ext, log->log_config_filename);

		/* reset the default log handler to our hierarchical logger */
		g_log_set_default_handler(chassis_log_extended_log_func, log_ext);

		/* the system should now be set up, let's try to log something */
		g_message("this should go to the root logger on level message");
	} else if (log->log_filename) {
        gboolean turned_off_syslog = FALSE;
        if (log->use_syslog) {
            log->use_syslog = FALSE;
            turned_off_syslog = TRUE;
        }
		if (0 == chassis_log_open(log)) {
			g_critical("can't open log-file '%s': %s", log->log_filename, g_strerror(errno));

			exit_code = EXIT_FAILURE; 
			exit_location = G_STRLOC;
			goto exit_nicely;
		}
        if (turned_off_syslog)
            g_warning("both log-file and log-use-syslog were given. turning off log-use-syslog, logging to %s", log->log_filename);
	}

	/* handle log-level after the config-file is read, just in case it is specified in the file */
	if (log_level) {
		if (0 != chassis_log_set_level(log, log_level)) {
			g_critical("--log-level=... failed, level '%s' is unknown ", log_level);

			exit_code = EXIT_FAILURE; 
			exit_location = G_STRLOC;
			goto exit_nicely;
		}
	} else {
		/* if it is not set, use "critical" as default */
		log->min_lvl = G_LOG_LEVEL_CRITICAL;
	}

	/* if not plugins are specified, load admin and proxy */
	if (!plugin_names) {
		plugin_names = g_new(char *, 3);

		plugin_names[0] = g_strdup("admin");
		plugin_names[1] = g_strdup("proxy");
		plugin_names[2] = NULL;
	}

	/* load the plugins */
	for (i = 0; plugin_names && plugin_names[i]; i++) {
#ifdef WIN32
#define G_MODULE_PREFIX "plugin-" /* we build the plugins with a prefix on win32 to avoid name-clashing in bin/ */
#else
#define G_MODULE_PREFIX "lib"
#endif
/* we have to hack around some glib distributions that
 * don't set the correct G_MODULE_SUFFIX, notably MacPorts
 */
#ifndef SHARED_LIBRARY_SUFFIX
#define SHARED_LIBRARY_SUFFIX G_MODULE_SUFFIX
#endif
		char *plugin_filename;
		/* skip trying to load a plugin when the parameter was --plugins= 
		   that will never work...
		*/
		if (!g_strcmp0("", plugin_names[i])) {
			continue;
		}
		plugin_filename = g_strdup_printf("%s%c%s%s.%s", 
				plugin_dir, 
				G_DIR_SEPARATOR, 
				G_MODULE_PREFIX,
				plugin_names[i],
				SHARED_LIBRARY_SUFFIX);

		p = chassis_plugin_load(plugin_filename);
		g_free(plugin_filename);

		if (print_version && p) {
			if (0 == i) printf("  == plugins ==" CHASSIS_NEWLINE); /* print the == plugins line only once */
			if (0 == strcmp(plugin_names[i], p->name)) {
                printf("  %s: %s" CHASSIS_NEWLINE, p->name, p->version);
			} else {
                printf("  %s(%s): %s" CHASSIS_NEWLINE, p->name, plugin_names[i], p->version);
			}
		}
		
		if (NULL == p) {
			g_critical("setting --plugin-dir=<dir> might help");
			exit_code = EXIT_FAILURE; 
			exit_location = G_STRLOC;
			goto exit_nicely;
		}

		g_ptr_array_add(srv->modules, p);

		if (NULL != (config_entries = chassis_plugin_get_options(p))) {
			gchar *group_desc = g_strdup_printf("%s-module", plugin_names[i]);
			gchar *help_msg = g_strdup_printf("Show options for the %s-module", plugin_names[i]);
			const gchar *group_name = plugin_names[i];

			GOptionGroup *option_grp = g_option_group_new(group_name, group_desc, help_msg, NULL, NULL);
			g_option_group_add_entries(option_grp, config_entries);
			g_option_context_add_group(option_ctx, option_grp);

			g_free(help_msg);
			g_free(group_desc);

			/* parse the new options */
			if (FALSE == g_option_context_parse(option_ctx, &argc, &argv, &gerr)) {
				g_critical("%s", gerr->message);
		
				exit_code = EXIT_FAILURE; 
				exit_location = G_STRLOC;
				goto exit_nicely;
			}
	
			if (keyfile) {
				if (chassis_keyfile_to_options(keyfile, "mysql-proxy", config_entries)) {
					exit_code = EXIT_FAILURE; 
					exit_location = G_STRLOC;
					goto exit_nicely;
				}
			}
			/* check for relative paths among the newly added options
			 * and resolve them to an absolute path if we have --basedir
			 */
			if (srv->base_dir) {
				int entry_idx;
				for (entry_idx = 0; config_entries[entry_idx].long_name; entry_idx++) {
					GOptionEntry entry = config_entries[entry_idx];
					
					switch(entry.arg) {
					case G_OPTION_ARG_FILENAME: {
						gchar **data = entry.arg_data;
						chassis_resolve_path(srv, data);
						break;
					}
					case G_OPTION_ARG_FILENAME_ARRAY: {
						gchar ***data = entry.arg_data;
						gchar **files = *data;
						if (NULL != files) {
							gint j;
							for (j = 0; files[j]; j++) chassis_resolve_path(srv, &files[j]);
						}
						break;
					}
					default:
						/* ignore other option types */
						break;
					}
				}
			}
		}
	}

	/* if we only print the version numbers, exit and don't do any more work */
	if (print_version) {
		exit_code = EXIT_SUCCESS;
		exit_location = G_STRLOC;
		goto exit_nicely;
	}

	/* we know about the options now, lets parse them */
	g_option_context_set_help_enabled(option_ctx, TRUE);
	g_option_context_set_ignore_unknown_options(option_ctx, FALSE);

	/* handle unknown options */
	if (FALSE == g_option_context_parse(option_ctx, &argc, &argv, &gerr)) {
		if (gerr->domain == G_OPTION_ERROR &&
		    gerr->code == G_OPTION_ERROR_UNKNOWN_OPTION) {
			g_critical("%s: %s (use --help to show all options)", 
					G_STRLOC, 
					gerr->message);
		} else {
			g_critical("%s: %s (code = %d, domain = %s)", 
					G_STRLOC, 
					gerr->message,
					gerr->code,
					g_quark_to_string(gerr->domain)
					);
		}
		
		exit_code = EXIT_FAILURE; 
		exit_location = G_STRLOC;
		goto exit_nicely;
	}

	g_option_context_free(option_ctx);
	option_ctx = NULL;

	/* after parsing the options we should only have the program name left */
	if (argc > 1) {
		g_critical("unknown option: %s", argv[1]);

		exit_code = EXIT_FAILURE; 
		exit_location = G_STRLOC;
		goto exit_nicely;
	}

	/* make sure that he max-thread-count isn't negative */
	if (event_thread_count < 1) {
		g_critical("--event-threads has to be >= 1, is %d", event_thread_count);

		exit_code = EXIT_FAILURE; 
		exit_location = G_STRLOC;
		goto exit_nicely;
	}

	srv->event_thread_count = event_thread_count;
	
#ifndef _WIN32	
	signal(SIGPIPE, SIG_IGN);

	if (daemon_mode) {
		daemonize();
	}

	if (auto_restart) {
		int child_exit_status = EXIT_SUCCESS; /* forward the exit-status of the child */
		int ret = proc_keepalive(&child_exit_status);

		if (ret > 0) {
			/* the agent stopped */
		
			exit_code = child_exit_status;
			goto exit_nicely;
		} else if (ret < 0) {
			exit_code = EXIT_FAILURE; 
			exit_location = G_STRLOC;
			goto exit_nicely;
		} else {
			/* we are the child, go on */
		}
	}
#endif
	if (pid_file) {
		int fd;
		gchar *pid_str;

		/**
		 * write the PID file
		 */

		if (-1 == (fd = open(pid_file, O_WRONLY|O_TRUNC|O_CREAT, 0600))) {
			g_critical("%s.%d: open(%s) failed: %s", 
					__FILE__, __LINE__,
					pid_file,
					g_strerror(errno));

			exit_code = EXIT_FAILURE; 
			exit_location = G_STRLOC;
			goto exit_nicely;
		}

		pid_str = g_strdup_printf("%d", getpid());

		write(fd, pid_str, strlen(pid_str));
		g_free(pid_str);

		close(fd);
	}

	/* the message has to be _after_ the g_option_content_parse() to 
	 * hide from the output if the --help is asked for
	 */
	g_message("%s started", PACKAGE_STRING); /* add tag to the logfile (after we opened the logfile) */

#ifdef _WIN32
	if (win32_running_as_service) agent_service_set_state(SERVICE_RUNNING, 0);
#endif

	/*
	 * we have to drop root privileges in chassis_mainloop() after
	 * the plugins opened the ports, so we need the user there
	 */
	srv->user = g_strdup(user);

	if (chassis_set_fdlimit(max_files_number)) {
		/* do we want to exit or just go on */
	}

	if (chassis_mainloop(srv)) {
		/* looks like we failed */
		g_critical("%s: Failure from chassis_mainloop. Shutting down.", G_STRLOC);
		exit_code = EXIT_FAILURE; 
		exit_location = G_STRLOC;
		goto exit_nicely;
	}

exit_nicely:
	/* necessary to set the shutdown flag, because the monitor will continue
	 * to schedule timers otherwise, causing an infinite loop in cleanup
	 */
	if (!exit_code) {
		exit_location = G_STRLOC;
	}
	chassis_set_shutdown_location(exit_location);

	if (!print_version) {
		g_message("shutting down normally, exit code is: %d", exit_code); /* add a tag to the logfile */
	}

#ifdef _WIN32
	if (win32_running_as_service) agent_service_set_state(SERVICE_STOP_PENDING, 0);
#endif
	
	if (keyfile) g_key_file_free(keyfile);
	if (default_file) g_free(default_file);

	if (gerr) g_error_free(gerr);
	if (option_ctx) g_option_context_free(option_ctx);
	if (srv) chassis_free(srv);

	if (base_dir) g_free(base_dir);
	if (user) g_free(user);
	if (pid_file) g_free(pid_file);
	if (log_level) g_free(log_level);
	if (plugin_dir) g_free(plugin_dir);

	if (plugin_names) {
		g_strfreev(plugin_names);
	}

	chassis_log_free(log);
	
#ifdef _WIN32
	if (win32_running_as_service) agent_service_set_state(SERVICE_STOPPED, 0);
#endif

#ifdef HAVE_SIGACTION
	/* reset the handler */
	sigsegv_sa.sa_handler = SIG_DFL;
	if (invoke_dbg_on_crash && !(RUNNING_ON_VALGRIND)) {
		sigaction(SIGSEGV, &sigsegv_sa, NULL);
	}
#endif

	return exit_code;
}

#ifdef _WIN32
/* win32 service */
/**
 the event-handler for the service
 
 the SCM will send us events from time to time which we acknoledge
 */

void WINAPI agent_service_ctrl(DWORD Opcode) {
	
	switch(Opcode) {
		case SERVICE_CONTROL_SHUTDOWN:
		case SERVICE_CONTROL_STOP:
			agent_service_set_state(SERVICE_STOP_PENDING, 0);
			
			chassis_set_shutdown(); /* exit the main-loop */
			
			break;
		default:
			agent_service_set_state(Opcode, 0);
			break;
	}
	
	return;
}

/**
 * trampoline us into the real main_cmdline
 */
void WINAPI agent_service_start(DWORD argc, LPTSTR *argv) {
	
	/* tell the service controller that we are alive */
	agent_service_status.dwCurrentState       = SERVICE_START_PENDING;
	agent_service_status.dwCheckPoint         = 0;
	agent_service_status.dwServiceType        = SERVICE_WIN32_OWN_PROCESS;
	agent_service_status.dwControlsAccepted   = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
	agent_service_status.dwWin32ExitCode      = NO_ERROR;
	agent_service_status.dwServiceSpecificExitCode = 0;
	
	agent_service_status_handle = RegisterServiceCtrlHandler("MerlinAgent", agent_service_ctrl); 
	
	if (agent_service_status_handle == (SERVICE_STATUS_HANDLE)0) {
		g_critical("RegisterServiceCtrlHandler failed");
		return; 
	}
	
	agent_service_set_state(SERVICE_START_PENDING, 1000);
	
	/* jump into the actual main */
	main_cmdline(shell_argc, shell_argv);
}

/**
 * Determine whether we are called as a service and set that up.
 * Then call main_cmdline to do the real work.
 */
int main_win32(int argc, char **argv) {
	WSADATA wsaData;

	SERVICE_TABLE_ENTRY dispatch_tab[] = {
		{ "MerlinAgent", agent_service_start },
		{ NULL, NULL } 
	};

	if (0 != WSAStartup(MAKEWORD( 2, 2 ), &wsaData)) {
		g_critical("WSAStartup failed to initialize the socket library.\n");

		return -1;
	}

	/* save the arguments because the service controller will clobber them */
	shell_argc = argc;
	shell_argv = argv;
	/* speculate that we are running as a service, reset to 0 on error */
	win32_running_as_service = 1;
	
	if (!StartServiceCtrlDispatcher(dispatch_tab)) {
		int err = GetLastError();
		
		switch(err) {
			case ERROR_FAILED_SERVICE_CONTROLLER_CONNECT:
				/* we weren't called as a service, carry on with the cmdline handling */
				win32_running_as_service = 0;
				return main_cmdline(shell_argc, shell_argv);
			case ERROR_SERVICE_ALREADY_RUNNING:
				g_critical("service is already running, shutting down");
				return 0;
			default:
				g_critical("unhandled error-code (%d) for StartServiceCtrlDispatcher(), shutting down", err);
				return -1;
		}
	}
	return 0;
}
#endif


/**
 * On Windows we first look if we are started as a service and 
 * set that up if appropriate.
 * We eventually fall down through to main_cmdline, even on Windows.
 */
int main(int argc, char **argv) {
#ifdef WIN32_AS_SERVICE
	return main_win32(argc, argv);
#else
	return main_cmdline(argc, argv);
#endif
}
