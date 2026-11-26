#ifndef WATCHDOG_SIGNAL_MANAGER_H
#define WATCHDOG_SIGNAL_MANAGER_H

#include <signal.h>

/**
 * Install async-signal-safe handlers for SIGINT, SIGTERM, SIGUSR1,
 * SIGCHLD, and SIGALRM via sigaction(), and ignore SIGPIPE.
 *
 * @return 0 on success, -1 on failure (errno set).
 */
int signal_manager_init(void);

/**
 * Reset all managed signal dispositions to SIG_DFL and unblock all signals.
 * Must be called in the child process immediately after fork() before exec().
 */
void signal_manager_reset_for_child(void);

/**
 * Start a periodic ITIMER_REAL timer using setitimer() that delivers SIGALRM.
 * Passing 0 disables the timer.
 *
 * @return 0 on success, -1 on failure (errno set).
 */
int signal_manager_start_timer(unsigned int interval_sec);

/**
 * Stop the periodic SIGALRM timer.
 */
void signal_manager_stop_timer(void);

/**
 * Sleep without busy-waiting until any managed signal sets a flag.
 * Uses sigprocmask() + sigsuspend() to prevent lost-signal race conditions.
 */
void signal_manager_wait_for_event(void);

/**
 * Query whether a shutdown signal (SIGINT or SIGTERM) has been received.
 */
int signal_manager_shutdown_requested(void);

/**
 * Return the signal number that initiated shutdown (SIGINT or SIGTERM), or 0.
 */
int signal_manager_shutdown_signal(void);

/**
 * Atomically read and clear the SIGUSR1 status-report flag.
 */
int signal_manager_consume_status_flag(void);

/**
 * Atomically read and clear the SIGCHLD child-exit flag.
 */
int signal_manager_consume_child_flag(void);

/**
 * Atomically read and clear the SIGALRM periodic-check flag.
 */
int signal_manager_consume_alarm_flag(void);

/**
 * Return a portable human-readable name for common POSIX signal numbers.
 */
const char *signal_manager_signal_name(int signum);

#endif /* WATCHDOG_SIGNAL_MANAGER_H */
