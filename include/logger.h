#ifndef WATCHDOG_LOGGER_H
#define WATCHDOG_LOGGER_H

#include <stddef.h>
#include <sys/types.h>

#define LOGGER_DEFAULT_PATH "watchdog.log"
#define LOGGER_NAME_MAX     64

typedef enum {
    LOG_INFO = 0,
    LOG_WARN,
    LOG_ERROR,
    LOG_STATUS
} LogLevel;

typedef struct {
    char name[LOGGER_NAME_MAX];
    pid_t pid;
    const char *state;
    int restart_count;
    int max_restarts;
    int last_exit_code;
    int last_term_signal;
    long uptime_sec;
} ProcessStatusSnapshot;

#if defined(__GNUC__) || defined(__clang__)
#define WATCHDOG_PRINTF_FMT(fmt_idx, arg_idx) \
    __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define WATCHDOG_PRINTF_FMT(fmt_idx, arg_idx)
#endif

/**
 * Open the log file in append mode with O_CLOEXEC.
 *
 * @param filepath    Path to the log file (uses LOGGER_DEFAULT_PATH if NULL/empty).
 * @param echo_stdout Non-zero to also mirror log entries to stdout.
 * @return 0 on success, -1 on failure (errno set).
 */
int logger_init(const char *filepath, int echo_stdout);

/**
 * Close the log file descriptor cleanly.
 */
void logger_close(void);

/**
 * Write a timestamped log record using POSIX write().
 */
void logger_log(LogLevel level, const char *fmt, ...) WATCHDOG_PRINTF_FMT(2, 3);

/**
 * Write a structured status report of all managed child processes (triggered by SIGUSR1).
 */
void logger_report_status(pid_t watchdog_pid,
                          const ProcessStatusSnapshot *entries,
                          size_t count);

/**
 * Return the active log file path.
 */
const char *logger_get_path(void);

#endif /* WATCHDOG_LOGGER_H */
