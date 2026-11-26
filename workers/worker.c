#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_term_signal(int signum)
{
    (void)signum;
    g_stop_requested = 1;
}

static void sleep_step_ms(long ms)
{
    struct timespec req;
    req.tv_sec  = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    (void)nanosleep(&req, NULL);
}

static int install_signal_action(int signum, void (*handler)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    return sigaction(signum, &sa, NULL);
}

static double monotonic_sec(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    }
    return (double)time(NULL);
}

int main(int argc, char *argv[])
{
    const char *name = "worker";
    const char *mode = "normal";
    int duration_sec = 0;
    int duration_set = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_sec = atoi(argv[++i]);
            duration_set = 1;
        }
    }

    (void)name;

    if (!duration_set) {
        if (strcmp(mode, "exit-ok") == 0) {
            duration_sec = 2;
        } else if (strcmp(mode, "crash") == 0 || strcmp(mode, "signal-crash") == 0) {
            duration_sec = 1;
        } else {
            duration_sec = 0;
        }
    }

    if (strcmp(mode, "ignore-term") == 0) {
        /* Ignore SIGTERM to allow verifying watchdog's SIGKILL timeout fallback */
        (void)install_signal_action(SIGTERM, SIG_IGN);
        (void)install_signal_action(SIGINT, SIG_IGN);
    } else {
        if (install_signal_action(SIGTERM, handle_term_signal) < 0 ||
            install_signal_action(SIGINT, handle_term_signal) < 0) {
            perror("worker: sigaction");
            return 1;
        }
    }

    double start = monotonic_sec();

    while (!g_stop_requested) {
        if (duration_sec > 0 && (monotonic_sec() - start) >= (double)duration_sec) {
            if (strcmp(mode, "exit-ok") == 0 || strcmp(mode, "normal") == 0) {
                return 0;
            }
            if (strcmp(mode, "crash") == 0) {
                return 1;
            }
            if (strcmp(mode, "signal-crash") == 0) {
                (void)raise(SIGABRT);
                return 134;
            }
        }
        sleep_step_ms(100);
    }

    return 0;
}
