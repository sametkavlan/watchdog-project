#include "signal_manager.h"

#include <stddef.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

/* Strictly async-signal-safe flags modified only by signal handlers */
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile sig_atomic_t g_shutdown_signal    = 0;
static volatile sig_atomic_t g_status_requested   = 0;
static volatile sig_atomic_t g_child_exited       = 0;
static volatile sig_atomic_t g_alarm_triggered    = 0;

static sigset_t g_managed_mask;

static void handle_shutdown_signal(int signum)
{
    if (!g_shutdown_requested) {
        g_shutdown_signal = (sig_atomic_t)signum;
    }
    g_shutdown_requested = 1;
}

static void handle_status_signal(int signum)
{
    (void)signum;
    g_status_requested = 1;
}

static void handle_child_signal(int signum)
{
    (void)signum;
    g_child_exited = 1;
}

static void handle_alarm_signal(int signum)
{
    (void)signum;
    g_alarm_triggered = 1;
}

static int install_handler(int signum, void (*handler)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sa.sa_mask    = g_managed_mask;
    sa.sa_flags   = flags;

    return sigaction(signum, &sa, NULL);
}

static int consume_flag(volatile sig_atomic_t *flag)
{
    sigset_t old_mask;
    int value = 0;

    if (sigprocmask(SIG_BLOCK, &g_managed_mask, &old_mask) == 0) {
        value = (int)(*flag);
        *flag = 0;
        (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
    } else {
        value = (int)(*flag);
        *flag = 0;
    }

    return value;
}

int signal_manager_init(void)
{
    g_shutdown_requested = 0;
    g_shutdown_signal    = 0;
    g_status_requested   = 0;
    g_child_exited       = 0;
    g_alarm_triggered    = 0;

    if (sigemptyset(&g_managed_mask) < 0 ||
        sigaddset(&g_managed_mask, SIGINT) < 0 ||
        sigaddset(&g_managed_mask, SIGTERM) < 0 ||
        sigaddset(&g_managed_mask, SIGUSR1) < 0 ||
        sigaddset(&g_managed_mask, SIGCHLD) < 0 ||
        sigaddset(&g_managed_mask, SIGALRM) < 0) {
        return -1;
    }

    if (install_handler(SIGINT,  handle_shutdown_signal, 0) < 0 ||
        install_handler(SIGTERM, handle_shutdown_signal, 0) < 0 ||
        install_handler(SIGUSR1, handle_status_signal,   0) < 0 ||
        install_handler(SIGCHLD, handle_child_signal,    SA_NOCLDSTOP) < 0 ||
        install_handler(SIGALRM, handle_alarm_signal,    0) < 0) {
        return -1;
    }

    struct sigaction sa_pipe;
    memset(&sa_pipe, 0, sizeof(sa_pipe));
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sa_pipe.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa_pipe, NULL) < 0) {
        return -1;
    }

    return 0;
}

void signal_manager_reset_for_child(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    (void)sigaction(SIGINT,  &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGUSR1, &sa, NULL);
    (void)sigaction(SIGCHLD, &sa, NULL);
    (void)sigaction(SIGALRM, &sa, NULL);
    (void)sigaction(SIGPIPE, &sa, NULL);

    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    (void)sigprocmask(SIG_SETMASK, &empty_mask, NULL);
}

int signal_manager_start_timer(unsigned int interval_sec)
{
    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));

    timer.it_interval.tv_sec = (time_t)interval_sec;
    timer.it_interval.tv_usec = 0;
    timer.it_value.tv_sec = (time_t)interval_sec;
    timer.it_value.tv_usec = 0;

    return setitimer(ITIMER_REAL, &timer, NULL);
}

void signal_manager_stop_timer(void)
{
    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));
    (void)setitimer(ITIMER_REAL, &timer, NULL);
}

void signal_manager_wait_for_event(void)
{
    sigset_t old_mask;

    if (sigprocmask(SIG_BLOCK, &g_managed_mask, &old_mask) < 0) {
        (void)pause();
        return;
    }

    if (!g_shutdown_requested &&
        !g_child_exited &&
        !g_status_requested &&
        !g_alarm_triggered) {
        sigset_t wait_mask = old_mask;
        sigdelset(&wait_mask, SIGINT);
        sigdelset(&wait_mask, SIGTERM);
        sigdelset(&wait_mask, SIGUSR1);
        sigdelset(&wait_mask, SIGCHLD);
        sigdelset(&wait_mask, SIGALRM);
        (void)sigsuspend(&wait_mask);
    }

    (void)sigprocmask(SIG_SETMASK, &old_mask, NULL);
}

int signal_manager_shutdown_requested(void)
{
    return (int)g_shutdown_requested;
}

int signal_manager_shutdown_signal(void)
{
    return (int)g_shutdown_signal;
}

int signal_manager_consume_status_flag(void)
{
    return consume_flag(&g_status_requested);
}

int signal_manager_consume_child_flag(void)
{
    return consume_flag(&g_child_exited);
}

int signal_manager_consume_alarm_flag(void)
{
    return consume_flag(&g_alarm_triggered);
}

const char *signal_manager_signal_name(int signum)
{
    switch (signum) {
        case SIGHUP:  return "SIGHUP";
        case SIGINT:  return "SIGINT";
        case SIGQUIT: return "SIGQUIT";
        case SIGILL:  return "SIGILL";
        case SIGABRT: return "SIGABRT";
        case SIGFPE:  return "SIGFPE";
        case SIGKILL: return "SIGKILL";
        case SIGSEGV: return "SIGSEGV";
        case SIGPIPE: return "SIGPIPE";
        case SIGALRM: return "SIGALRM";
        case SIGTERM: return "SIGTERM";
        case SIGUSR1: return "SIGUSR1";
        case SIGUSR2: return "SIGUSR2";
        case SIGCHLD: return "SIGCHLD";
        default:      return "UNKNOWN";
    }
}
