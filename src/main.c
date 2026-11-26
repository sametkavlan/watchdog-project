#include "logger.h"
#include "process_manager.h"
#include "signal_manager.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_CHECK_INTERVAL_SEC 5

typedef struct {
    char name[LOGGER_NAME_MAX];
    char mode[PM_MODE_MAX];
    int duration_sec;
} WorkerSpec;

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "Options:\n"
            "  -l <path>       Log file path (default: %s)\n"
            "  -m <count>      Max automatic restarts per child (default: %d)\n"
            "  -i <seconds>    Periodic SIGALRM check interval, 0=disable (default: %d)\n"
            "  -t <seconds>    Graceful shutdown timeout before SIGKILL (default: %d)\n"
            "  -w <spec>       Worker spec 'name:mode[:duration]' (repeatable)\n"
            "                  Modes: normal, exit-ok, crash, signal-crash, ignore-term\n"
            "  -q              Quiet mode (write logs to file only)\n"
            "  -h              Display this help message\n",
            prog,
            LOGGER_DEFAULT_PATH,
            PM_DEFAULT_MAX_RESTARTS,
            DEFAULT_CHECK_INTERVAL_SEC,
            PM_DEFAULT_SHUTDOWN_TIMEOUT);
}

static int parse_non_negative_int(const char *str, int *out)
{
    if (str == NULL || str[0] == '\0') {
        return -1;
    }

    errno = 0;
    char *end = NULL;
    long val = strtol(str, &end, 10);
    if (errno != 0 || end == str || *end != '\0' || val < 0 || val > INT_MAX) {
        return -1;
    }

    *out = (int)val;
    return 0;
}

static int parse_worker_spec(const char *spec, WorkerSpec *out)
{
    if (spec == NULL || out == NULL) {
        return -1;
    }

    char buf[256];
    if (strlen(spec) >= sizeof(buf)) {
        return -1;
    }
    snprintf(buf, sizeof(buf), "%s", spec);

    char *first_colon = strchr(buf, ':');
    if (first_colon == NULL) {
        return -1;
    }
    *first_colon = '\0';

    const char *name = buf;
    char *mode = first_colon + 1;
    char *second_colon = strchr(mode, ':');
    int duration = 0;

    if (second_colon != NULL) {
        *second_colon = '\0';
        const char *dur_str = second_colon + 1;
        if (dur_str[0] != '\0' && parse_non_negative_int(dur_str, &duration) < 0) {
            return -1;
        }
    }

    if (name[0] == '\0' || mode[0] == '\0') {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->mode, sizeof(out->mode), "%s", mode);
    out->duration_sec = duration;
    return 0;
}

static void resolve_worker_path(const char *argv0, char *out_path, size_t out_size)
{
    if (access("./worker", X_OK) == 0) {
        snprintf(out_path, out_size, "./worker");
        return;
    }

    if (argv0 != NULL) {
        const char *slash = strrchr(argv0, '/');
        if (slash != NULL) {
            size_t dir_len = (size_t)(slash - argv0);
            if (dir_len + strlen("/worker") + 1 < out_size) {
                memcpy(out_path, argv0, dir_len);
                snprintf(out_path + dir_len, out_size - dir_len, "/worker");
                return;
            }
        }
    }

    snprintf(out_path, out_size, "./worker");
}

int main(int argc, char *argv[])
{
    const char *log_path   = LOGGER_DEFAULT_PATH;
    int max_restarts       = PM_DEFAULT_MAX_RESTARTS;
    int check_interval_sec = DEFAULT_CHECK_INTERVAL_SEC;
    int shutdown_timeout   = PM_DEFAULT_SHUTDOWN_TIMEOUT;
    int echo_stdout        = 1;

    WorkerSpec custom_specs[PM_MAX_WORKERS];
    size_t custom_count = 0;

    int opt;
    while ((opt = getopt(argc, argv, "l:m:i:t:w:qh")) != -1) {
        switch (opt) {
            case 'l':
                log_path = optarg;
                break;
            case 'm':
                if (parse_non_negative_int(optarg, &max_restarts) < 0) {
                    fprintf(stderr, "Invalid value for -m: %s\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'i':
                if (parse_non_negative_int(optarg, &check_interval_sec) < 0) {
                    fprintf(stderr, "Invalid value for -i: %s\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 't':
                if (parse_non_negative_int(optarg, &shutdown_timeout) < 0) {
                    fprintf(stderr, "Invalid value for -t: %s\n", optarg);
                    return EXIT_FAILURE;
                }
                break;
            case 'w':
                if (custom_count >= PM_MAX_WORKERS) {
                    fprintf(stderr, "Maximum worker limit (%d) exceeded.\n", PM_MAX_WORKERS);
                    return EXIT_FAILURE;
                }
                if (parse_worker_spec(optarg, &custom_specs[custom_count]) < 0) {
                    fprintf(stderr, "Invalid worker spec '%s' (expected name:mode[:duration]).\n",
                            optarg);
                    return EXIT_FAILURE;
                }
                custom_count++;
                break;
            case 'q':
                echo_stdout = 0;
                break;
            case 'h':
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            default:
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    if (logger_init(log_path, echo_stdout) < 0) {
        perror("logger_init");
        return EXIT_FAILURE;
    }

    /* Install POSIX signal handlers before creating any child processes */
    if (signal_manager_init() < 0) {
        logger_log(LOG_ERROR, "Failed to initialize signal handlers: %s", strerror(errno));
        logger_close();
        return EXIT_FAILURE;
    }

    char worker_path[PM_PATH_MAX];
    resolve_worker_path(argv[0], worker_path, sizeof(worker_path));

    ProcessManager pm;
    pm_init(&pm, max_restarts);

    if (custom_count == 0) {
        /* Default configuration: launch two persistent child worker processes */
        if (pm_add_worker(&pm, "worker-1", worker_path, "normal", 0) < 0 ||
            pm_add_worker(&pm, "worker-2", worker_path, "normal", 0) < 0) {
            logger_log(LOG_ERROR, "Failed to register default worker specifications.");
            logger_close();
            return EXIT_FAILURE;
        }
    } else {
        for (size_t i = 0; i < custom_count; ++i) {
            if (pm_add_worker(&pm,
                              custom_specs[i].name,
                              worker_path,
                              custom_specs[i].mode,
                              custom_specs[i].duration_sec) < 0) {
                logger_log(LOG_ERROR, "Failed to register worker '%s'.", custom_specs[i].name);
                logger_close();
                return EXIT_FAILURE;
            }
        }
    }

    logger_log(LOG_INFO,
               "Watchdog started (PID=%ld, log=%s, max_restarts=%d, check_interval=%ds, shutdown_timeout=%ds).",
               (long)getpid(), logger_get_path(),
               max_restarts, check_interval_sec, shutdown_timeout);

    if (pm_start_all(&pm) < 0) {
        logger_log(LOG_ERROR, "Failed to launch initial child processes; exiting.");
        logger_close();
        return EXIT_FAILURE;
    }

    if (check_interval_sec > 0) {
        if (signal_manager_start_timer((unsigned int)check_interval_sec) < 0) {
            logger_log(LOG_WARN,
                       "Failed to start periodic SIGALRM timer: %s",
                       strerror(errno));
        }
    }

    /* Main event-driven control loop (zero busy-waiting via sigsuspend) */
    while (!signal_manager_shutdown_requested()) {
        if (signal_manager_consume_child_flag()) {
            pm_handle_child_exits(&pm);
        }

        if (signal_manager_consume_status_flag()) {
            pm_report_status(&pm);
        }

        if (signal_manager_consume_alarm_flag()) {
            pm_periodic_check(&pm);
        }

        if (signal_manager_shutdown_requested()) {
            break;
        }

        signal_manager_wait_for_event();
    }

    /* Graceful shutdown sequence */
    signal_manager_stop_timer();

    int term_sig = signal_manager_shutdown_signal();
    logger_log(LOG_INFO,
               "Received shutdown signal %d (%s); initiating graceful shutdown.",
               term_sig, signal_manager_signal_name(term_sig));

    pm_graceful_shutdown(&pm, (unsigned int)shutdown_timeout);

    logger_log(LOG_INFO, "Watchdog (PID=%ld) exiting cleanly.", (long)getpid());
    logger_close();
    return EXIT_SUCCESS;
}
