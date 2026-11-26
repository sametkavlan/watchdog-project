#include "process_manager.h"
#include "signal_manager.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double monotonic_now_sec(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    }
    return (double)time(NULL);
}

static void sleep_ms(long ms)
{
    struct timespec req;
    req.tv_sec  = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    (void)nanosleep(&req, NULL);
}

const char *pm_state_to_string(ProcessState state)
{
    switch (state) {
        case PROC_STATE_INIT:          return "INIT";
        case PROC_STATE_RUNNING:       return "RUNNING";
        case PROC_STATE_EXITED_NORMAL: return "EXITED_NORMAL";
        case PROC_STATE_STOPPED:       return "STOPPED";
        case PROC_STATE_FAILED:        return "FAILED";
        default:                       return "UNKNOWN";
    }
}

void pm_init(ProcessManager *pm, int max_restarts)
{
    if (pm == NULL) {
        return;
    }
    memset(pm, 0, sizeof(*pm));
    pm->max_restarts = (max_restarts < 0) ? PM_DEFAULT_MAX_RESTARTS : max_restarts;
}

int pm_add_worker(ProcessManager *pm,
                  const char *name,
                  const char *exec_path,
                  const char *mode,
                  int duration_sec)
{
    if (pm == NULL || name == NULL || name[0] == '\0' ||
        exec_path == NULL || exec_path[0] == '\0') {
        return -1;
    }

    if (pm->count >= PM_MAX_WORKERS) {
        return -1;
    }

    ManagedProcess *w = &pm->workers[pm->count];
    memset(w, 0, sizeof(*w));

    snprintf(w->name, sizeof(w->name), "%s", name);
    snprintf(w->exec_path, sizeof(w->exec_path), "%s", exec_path);
    snprintf(w->mode, sizeof(w->mode), "%s",
             (mode != NULL && mode[0] != '\0') ? mode : "normal");

    w->duration_sec     = (duration_sec < 0) ? 0 : duration_sec;
    w->pid              = -1;
    w->state            = PROC_STATE_INIT;
    w->restart_count    = 0;
    w->max_restarts     = pm->max_restarts;
    w->last_exit_code   = -1;
    w->last_term_signal = 0;
    w->start_time       = 0;

    pm->count++;
    return 0;
}

static int start_single_worker(ProcessManager *pm, size_t index, int is_restart)
{
    ManagedProcess *w = &pm->workers[index];

    if (access(w->exec_path, X_OK) != 0) {
        logger_log(LOG_ERROR,
                   "Cannot execute worker '%s' at '%s': %s",
                   w->name, w->exec_path, strerror(errno));
        w->state = PROC_STATE_FAILED;
        return -1;
    }

    char duration_buf[32];
    snprintf(duration_buf, sizeof(duration_buf), "%d", w->duration_sec);

    pid_t pid = fork();
    if (pid < 0) {
        logger_log(LOG_ERROR,
                   "fork() failed for worker '%s': %s",
                   w->name, strerror(errno));
        w->state = PROC_STATE_FAILED;
        return -1;
    }

    if (pid == 0) {
        /* Child process: restore default signal handlers and place in own process group */
        signal_manager_reset_for_child();
        (void)setpgid(0, 0);

        char arg_name_flag[] = "--name";
        char arg_mode_flag[] = "--mode";
        char arg_dur_flag[]  = "--duration";

        char *argv[] = {
            w->exec_path,
            arg_name_flag, w->name,
            arg_mode_flag, w->mode,
            arg_dur_flag,  duration_buf,
            NULL
        };

        execv(w->exec_path, argv);

        /* execv() only returns on error; use _exit() to avoid flushing parent buffers */
        perror("execv");
        _exit(127);
    }

    /* Parent process */
    w->pid        = pid;
    w->state      = PROC_STATE_RUNNING;
    w->start_time = time(NULL);

    if (is_restart) {
        logger_log(LOG_INFO,
                   "Restarted child '%s' with new PID %ld (mode=%s, restart %d/%d).",
                   w->name, (long)w->pid, w->mode,
                   w->restart_count, w->max_restarts);
    } else {
        logger_log(LOG_INFO,
                   "Started child '%s' with PID %ld (mode=%s, duration=%ds).",
                   w->name, (long)w->pid, w->mode, w->duration_sec);
    }

    return 0;
}

int pm_start_all(ProcessManager *pm)
{
    if (pm == NULL || pm->count == 0) {
        return -1;
    }

    int started = 0;
    for (size_t i = 0; i < pm->count; ++i) {
        if (start_single_worker(pm, i, 0) == 0) {
            started++;
        }
    }

    return (started > 0) ? started : -1;
}

static int find_worker_by_pid(const ProcessManager *pm, pid_t pid)
{
    for (size_t i = 0; i < pm->count; ++i) {
        if (pm->workers[i].pid == pid) {
            return (int)i;
        }
    }
    return -1;
}

static void handle_unexpected_termination(ProcessManager *pm, size_t index)
{
    ManagedProcess *w = &pm->workers[index];

    if (pm->shutting_down) {
        w->state = PROC_STATE_STOPPED;
        return;
    }

    if (w->restart_count < w->max_restarts) {
        w->restart_count++;
        logger_log(LOG_INFO,
                   "Scheduling automatic restart for '%s' (attempt %d/%d).",
                   w->name, w->restart_count, w->max_restarts);
        (void)start_single_worker(pm, index, 1);
    } else {
        w->state = PROC_STATE_FAILED;
        logger_log(LOG_ERROR,
                   "Child '%s' exceeded maximum restart limit (%d/%d); stopping retries.",
                   w->name, w->restart_count, w->max_restarts);
    }
}

static void record_reaped_child(ProcessManager *pm, pid_t pid, int status)
{
    int idx = find_worker_by_pid(pm, pid);
    if (idx < 0) {
        logger_log(LOG_WARN, "Reaped untracked child process with PID %ld.", (long)pid);
        return;
    }

    ManagedProcess *w = &pm->workers[idx];

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        w->last_exit_code   = code;
        w->last_term_signal = 0;

        if (pm->shutting_down) {
            w->state = PROC_STATE_STOPPED;
            logger_log(LOG_INFO,
                       "Child '%s' (PID %ld) terminated cleanly during shutdown (exit=%d).",
                       w->name, (long)pid, code);
        } else if (code == 0) {
            w->state = PROC_STATE_EXITED_NORMAL;
            logger_log(LOG_INFO,
                       "Child '%s' (PID %ld) exited normally with exit code 0; not restarting.",
                       w->name, (long)pid);
        } else {
            logger_log(LOG_WARN,
                       "Child '%s' (PID %ld) terminated unexpectedly with exit code %d.",
                       w->name, (long)pid, code);
            handle_unexpected_termination(pm, (size_t)idx);
        }
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        w->last_exit_code   = -1;
        w->last_term_signal = sig;

        if (pm->shutting_down) {
            w->state = PROC_STATE_STOPPED;
            logger_log(LOG_INFO,
                       "Child '%s' (PID %ld) terminated during shutdown by signal %d (%s).",
                       w->name, (long)pid, sig, signal_manager_signal_name(sig));
        } else {
            logger_log(LOG_WARN,
                       "Child '%s' (PID %ld) terminated unexpectedly by signal %d (%s).",
                       w->name, (long)pid, sig, signal_manager_signal_name(sig));
            handle_unexpected_termination(pm, (size_t)idx);
        }
    }
}

void pm_handle_child_exits(ProcessManager *pm)
{
    if (pm == NULL) {
        return;
    }

    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            record_reaped_child(pm, pid, status);
        } else if (pid == 0) {
            break;
        } else {
            if (errno == EINTR) {
                continue;
            }
            if (errno != ECHILD) {
                logger_log(LOG_ERROR, "waitpid() failed: %s", strerror(errno));
            }
            break;
        }
    }
}

void pm_periodic_check(ProcessManager *pm)
{
    if (pm == NULL || pm->shutting_down) {
        return;
    }

    /* Safety sweep for any pending child terminations */
    pm_handle_child_exits(pm);

    size_t running = 0;
    size_t normal  = 0;
    size_t failed  = 0;

    for (size_t i = 0; i < pm->count; ++i) {
        ManagedProcess *w = &pm->workers[i];

        if (w->state == PROC_STATE_RUNNING) {
            if (kill(w->pid, 0) < 0 && errno == ESRCH) {
                logger_log(LOG_WARN,
                           "Periodic check detected missing child '%s' (PID %ld); reaping.",
                           w->name, (long)w->pid);
                pm_handle_child_exits(pm);
            }
        }

        switch (w->state) {
            case PROC_STATE_RUNNING:       running++; break;
            case PROC_STATE_EXITED_NORMAL: normal++;  break;
            case PROC_STATE_FAILED:        failed++;  break;
            default: break;
        }
    }

    logger_log(LOG_INFO,
               "Periodic health check (SIGALRM): running=%zu, exited_normal=%zu, failed=%zu, total=%zu.",
               running, normal, failed, pm->count);
}

void pm_report_status(const ProcessManager *pm)
{
    if (pm == NULL) {
        return;
    }

    ProcessStatusSnapshot snapshots[PM_MAX_WORKERS];
    time_t now = time(NULL);

    for (size_t i = 0; i < pm->count; ++i) {
        const ManagedProcess *w = &pm->workers[i];
        ProcessStatusSnapshot *s = &snapshots[i];

        memset(s, 0, sizeof(*s));
        snprintf(s->name, sizeof(s->name), "%s", w->name);
        s->pid              = w->pid;
        s->state            = pm_state_to_string(w->state);
        s->restart_count    = w->restart_count;
        s->max_restarts     = w->max_restarts;
        s->last_exit_code   = w->last_exit_code;
        s->last_term_signal = w->last_term_signal;
        s->uptime_sec       = (w->state == PROC_STATE_RUNNING && w->start_time > 0 && now >= w->start_time)
                                  ? (long)(now - w->start_time)
                                  : 0;
    }

    logger_report_status(getpid(), snapshots, pm->count);
}

size_t pm_count_running(const ProcessManager *pm)
{
    if (pm == NULL) {
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i < pm->count; ++i) {
        if (pm->workers[i].state == PROC_STATE_RUNNING) {
            count++;
        }
    }
    return count;
}

void pm_graceful_shutdown(ProcessManager *pm, unsigned int timeout_sec)
{
    if (pm == NULL) {
        return;
    }

    pm->shutting_down = 1;

    /* First reap any children that already exited */
    pm_handle_child_exits(pm);

    size_t remaining = pm_count_running(pm);
    if (remaining == 0) {
        logger_log(LOG_INFO, "Graceful shutdown: no running child processes left to stop.");
        return;
    }

    logger_log(LOG_INFO,
               "Graceful shutdown: sending SIGTERM to %zu running child process(es) (timeout=%us).",
               remaining, timeout_sec);

    for (size_t i = 0; i < pm->count; ++i) {
        ManagedProcess *w = &pm->workers[i];
        if (w->state == PROC_STATE_RUNNING && w->pid > 0) {
            if (kill(w->pid, SIGTERM) == 0) {
                logger_log(LOG_INFO, "Sent SIGTERM to child '%s' (PID %ld).",
                           w->name, (long)w->pid);
            } else if (errno != ESRCH) {
                logger_log(LOG_ERROR,
                           "Failed to send SIGTERM to child '%s' (PID %ld): %s",
                           w->name, (long)w->pid, strerror(errno));
            }
        }
    }

    /* Wait up to timeout_sec for all children to exit cleanly */
    double deadline = monotonic_now_sec() + (double)timeout_sec;
    while (pm_count_running(pm) > 0) {
        pm_handle_child_exits(pm);
        if (pm_count_running(pm) == 0) {
            break;
        }
        if (monotonic_now_sec() >= deadline) {
            break;
        }
        sleep_ms(50);
    }

    /* Escalate unresponsive children to SIGKILL and reap synchronously */
    remaining = pm_count_running(pm);
    if (remaining > 0) {
        logger_log(LOG_WARN,
                   "Shutdown timeout (%us) expired with %zu child(ren) still running; sending SIGKILL.",
                   timeout_sec, remaining);

        for (size_t i = 0; i < pm->count; ++i) {
            ManagedProcess *w = &pm->workers[i];
            if (w->state == PROC_STATE_RUNNING && w->pid > 0) {
                if (kill(w->pid, SIGKILL) == 0) {
                    logger_log(LOG_WARN,
                               "Sent SIGKILL to unresponsive child '%s' (PID %ld).",
                               w->name, (long)w->pid);
                } else if (errno != ESRCH) {
                    logger_log(LOG_ERROR,
                               "Failed to send SIGKILL to child '%s' (PID %ld): %s",
                               w->name, (long)w->pid, strerror(errno));
                }
            }
        }

        /* Synchronously reap all remaining children to prevent zombie leaks */
        for (size_t i = 0; i < pm->count; ++i) {
            ManagedProcess *w = &pm->workers[i];
            if (w->state == PROC_STATE_RUNNING && w->pid > 0) {
                int status = 0;
                pid_t reaped;
                do {
                    reaped = waitpid(w->pid, &status, 0);
                } while (reaped < 0 && errno == EINTR);

                if (reaped > 0) {
                    record_reaped_child(pm, reaped, status);
                } else if (errno == ECHILD) {
                    w->state = PROC_STATE_STOPPED;
                }
            }
        }
    }

    /* Final drain to ensure zero un-reaped children */
    pm_handle_child_exits(pm);
    logger_log(LOG_INFO, "Graceful shutdown complete: all child processes terminated and reaped.");
}
