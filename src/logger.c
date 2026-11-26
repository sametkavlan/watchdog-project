#include "logger.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_LINE_MAX   1024
#define LOG_PATH_MAX   256
#define TIMESTAMP_MAX  32

static int g_log_fd = -1;
static int g_echo_stdout = 1;
static char g_log_path[LOG_PATH_MAX] = LOGGER_DEFAULT_PATH;

static const char *level_to_string(LogLevel level)
{
    switch (level) {
        case LOG_INFO:   return "INFO";
        case LOG_WARN:   return "WARN";
        case LOG_ERROR:  return "ERROR";
        case LOG_STATUS: return "STATUS";
        default:         return "INFO";
    }
}

static int write_all(int fd, const char *buf, size_t len)
{
    size_t written = 0;

    while (written < len) {
        ssize_t n = write(fd, buf + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        written += (size_t)n;
    }
    return 0;
}

static void format_timestamp(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm tm_now;

    if (now == (time_t)-1 || localtime_r(&now, &tm_now) == NULL) {
        snprintf(buf, size, "0000-00-00 00:00:00");
        return;
    }

    if (strftime(buf, size, "%Y-%m-%d %H:%M:%S", &tm_now) == 0) {
        snprintf(buf, size, "0000-00-00 00:00:00");
    }
}

int logger_init(const char *filepath, int echo_stdout)
{
    const char *target = (filepath != NULL && filepath[0] != '\0')
                             ? filepath
                             : LOGGER_DEFAULT_PATH;

    if (g_log_fd >= 0) {
        logger_close();
    }

    int fd = open(target, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }

    g_log_fd = fd;
    g_echo_stdout = echo_stdout ? 1 : 0;
    snprintf(g_log_path, sizeof(g_log_path), "%s", target);
    return 0;
}

void logger_close(void)
{
    if (g_log_fd >= 0) {
        while (close(g_log_fd) < 0 && errno == EINTR) {
            /* Retry if interrupted by a signal */
        }
        g_log_fd = -1;
    }
}

void logger_log(LogLevel level, const char *fmt, ...)
{
    char timestamp[TIMESTAMP_MAX];
    char msg[LOG_LINE_MAX - 64];
    char line[LOG_LINE_MAX];
    va_list args;

    format_timestamp(timestamp, sizeof(timestamp));

    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    int len = snprintf(line, sizeof(line), "[%s] [%s] %s\n",
                       timestamp, level_to_string(level), msg);
    if (len < 0) {
        return;
    }

    size_t write_len = ((size_t)len >= sizeof(line))
                           ? (sizeof(line) - 1)
                           : (size_t)len;

    if (write_len > 0 && line[write_len - 1] != '\n') {
        line[write_len - 1] = '\n';
    }

    if (g_log_fd >= 0) {
        (void)write_all(g_log_fd, line, write_len);
    }

    if (g_echo_stdout) {
        (void)write_all(STDOUT_FILENO, line, write_len);
    }
}

void logger_report_status(pid_t watchdog_pid,
                          const ProcessStatusSnapshot *entries,
                          size_t count)
{
    size_t running_count = 0;

    for (size_t i = 0; i < count; ++i) {
        if (entries[i].state != NULL && strcmp(entries[i].state, "RUNNING") == 0) {
            running_count++;
        }
    }

    logger_log(LOG_STATUS,
               "=== Status Report (SIGUSR1) | Watchdog PID: %ld | Running: %zu/%zu ===",
               (long)watchdog_pid, running_count, count);

    if (count == 0) {
        logger_log(LOG_STATUS, "  (no managed child processes)");
    }

    for (size_t i = 0; i < count; ++i) {
        const ProcessStatusSnapshot *e = &entries[i];
        const char *state = (e->state != NULL) ? e->state : "UNKNOWN";

        if (e->pid > 0 && strcmp(state, "RUNNING") == 0) {
            logger_log(LOG_STATUS,
                       "  - %-12s | PID: %-7ld | State: %-13s | Restarts: %d/%d | Uptime: %lds",
                       e->name, (long)e->pid, state,
                       e->restart_count, e->max_restarts, e->uptime_sec);
        } else if (e->last_term_signal > 0) {
            logger_log(LOG_STATUS,
                       "  - %-12s | PID: %-7ld | State: %-13s | Restarts: %d/%d | LastSignal: %d",
                       e->name, (long)e->pid, state,
                       e->restart_count, e->max_restarts, e->last_term_signal);
        } else if (e->last_exit_code >= 0) {
            logger_log(LOG_STATUS,
                       "  - %-12s | PID: %-7ld | State: %-13s | Restarts: %d/%d | LastExit: %d",
                       e->name, (long)e->pid, state,
                       e->restart_count, e->max_restarts, e->last_exit_code);
        } else {
            logger_log(LOG_STATUS,
                       "  - %-12s | PID: %-7ld | State: %-13s | Restarts: %d/%d",
                       e->name, (long)e->pid, state,
                       e->restart_count, e->max_restarts);
        }
    }

    logger_log(LOG_STATUS, "=== End of Status Report ===");
}

const char *logger_get_path(void)
{
    return g_log_path;
}
