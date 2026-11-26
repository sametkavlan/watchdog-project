#ifndef WATCHDOG_PROCESS_MANAGER_H
#define WATCHDOG_PROCESS_MANAGER_H

#include "logger.h"

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#define PM_MAX_WORKERS              16
#define PM_PATH_MAX                 256
#define PM_MODE_MAX                 32
#define PM_DEFAULT_MAX_RESTARTS     3
#define PM_DEFAULT_SHUTDOWN_TIMEOUT 5

typedef enum {
    PROC_STATE_INIT = 0,
    PROC_STATE_RUNNING,
    PROC_STATE_EXITED_NORMAL,
    PROC_STATE_STOPPED,
    PROC_STATE_FAILED
} ProcessState;

typedef struct {
    char name[LOGGER_NAME_MAX];
    char exec_path[PM_PATH_MAX];
    char mode[PM_MODE_MAX];
    int duration_sec;

    pid_t pid;
    ProcessState state;
    int restart_count;
    int max_restarts;
    int last_exit_code;
    int last_term_signal;
    time_t start_time;
} ManagedProcess;

typedef struct {
    ManagedProcess workers[PM_MAX_WORKERS];
    size_t count;
    int max_restarts;
    int shutting_down;
} ProcessManager;

/**
 * Initialize a ProcessManager instance.
 *
 * @param pm           Pointer to ProcessManager.
 * @param max_restarts Maximum automatic restart attempts per child (default 3 if < 0).
 */
void pm_init(ProcessManager *pm, int max_restarts);

/**
 * Register a child worker specification to be launched and monitored.
 *
 * @return 0 on success, -1 if capacity exceeded or invalid arguments.
 */
int pm_add_worker(ProcessManager *pm,
                  const char *name,
                  const char *exec_path,
                  const char *mode,
                  int duration_sec);

/**
 * Start all registered child processes using fork() and execv().
 *
 * @return Number of successfully launched children, or -1 on error.
 */
int pm_start_all(ProcessManager *pm);

/**
 * Reap all terminated children via non-blocking waitpid(), classify
 * normal vs. unexpected terminations, and apply bounded auto-restart.
 */
void pm_handle_child_exits(ProcessManager *pm);

/**
 * Perform a periodic health check (triggered by SIGALRM / setitimer()).
 */
void pm_periodic_check(ProcessManager *pm);

/**
 * Write the current status of all managed child processes to the log file
 * (triggered by SIGUSR1).
 */
void pm_report_status(const ProcessManager *pm);

/**
 * Execute the graceful shutdown sequence:
 * stop restarts, send SIGTERM to running children, wait up to timeout_sec,
 * escalate stubborn children to SIGKILL, and reap every child cleanly.
 */
void pm_graceful_shutdown(ProcessManager *pm, unsigned int timeout_sec);

/**
 * Return the count of child processes currently in PROC_STATE_RUNNING.
 */
size_t pm_count_running(const ProcessManager *pm);

/**
 * Convert a ProcessState enum value to a human-readable string.
 */
const char *pm_state_to_string(ProcessState state);

#endif /* WATCHDOG_PROCESS_MANAGER_H */
