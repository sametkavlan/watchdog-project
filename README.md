# Watchdog — POSIX Signal-Based Process Monitoring and Management System

**Course:** Sakarya University — Software Engineering Department, SWE-301 System Programming (2026–2027 Fall, Term Project 1)  
**Repository:** https://github.com/sametkavlan/watchdog-project  
**Author:** Samet Kavlan (`sametkavlan`)

## Overview

`watchdog` is a modular process supervisor implemented in standard C (C11) using POSIX system calls. It spawns and monitors child worker processes via `fork()` and `execv()`, detects terminations asynchronously via `SIGCHLD` + `waitpid()`, distinguishes normal exits from unexpected terminations, enforces a bounded automatic restart limit (maximum 3 retries by default), runs periodic health checks via `setitimer()` + `SIGALRM`, reports process status on `SIGUSR1`, and executes a controlled graceful shutdown (`SIGTERM` followed by `SIGKILL` timeout escalation) on `SIGINT` or `SIGTERM`.

## Features

- **POSIX Child Process Management:** Spawns at least two worker processes (`worker-1`, `worker-2`) via `fork()` and `execv()`, isolating each child in its own process group (`setpgid(0, 0)`) and resetting signal masks/dispositions before `execv()`.
- **Async-Signal-Safe Signal Handling:** Uses `sigaction()` exclusively; signal handlers only update `volatile sig_atomic_t` flags. The main control loop uses `sigprocmask()` + `sigsuspend()` for zero busy-waiting and zero lost-signal race conditions.
- **Termination Classification & Bounded Auto-Restart:** Reaps all terminated children in a `waitpid(-1, &status, WNOHANG)` loop. Normal exits (`exit(0)`) are recorded as `EXITED_NORMAL` without restart. Unexpected terminations (non-zero exit codes or terminating signals) are automatically restarted up to 3 times (`MAX_RESTARTS = 3`), after which the child is marked `FAILED`.
- **Periodic Health Checks:** Uses `setitimer(ITIMER_REAL, ...)` to emit `SIGALRM` at regular intervals and audit process health.
- **On-Demand Status Reporting (`SIGUSR1`):** Writes a timestamped snapshot of all managed processes (name, PID, state, restart count, uptime / exit status) to the log file.
- **Graceful Shutdown (`SIGINT` / `SIGTERM`):** Halts further restarts, stops the periodic timer, sends `SIGTERM` to all running children, waits up to the configured timeout, escalates unresponsive children to `SIGKILL`, and synchronously reaps every child so no zombie processes remain.
- **Leak-Free Timestamped Logging:** Uses POSIX `open(..., O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644)` and a `write()` retry loop so child processes never inherit the log file descriptor.

## Project Structure

```text
watchdog-project/
├── Makefile
├── README.md
├── include/
│   ├── logger.h
│   ├── process_manager.h
│   └── signal_manager.h
├── src/
│   ├── logger.c
│   ├── main.c
│   ├── process_manager.c
│   └── signal_manager.c
├── workers/
│   └── worker.c
├── tests/
│   └── test_scenarios.sh
└── docs/
    ├── REPORT.pdf
    ├── sample_watchdog.log
    └── test_results.txt
```

## Build & Installation

### Requirements
- Linux (or POSIX-compliant Unix/macOS) environment
- GCC or Clang C compiler (`-std=c11`, POSIX.1-2008 / `_XOPEN_SOURCE=700`)
- GNU `make` and `bash` (for automated test suite)

### Compile

```bash
make
```

This builds both the `watchdog` supervisor executable and the `worker` child binary with `-std=c11 -Wall -Wextra -Wpedantic -O2 -g` (zero warnings).

To rebuild from scratch or clean build artifacts:

```bash
make re
make clean
```

## Usage

### Default Mode (2 Persistent Workers)

```bash
./watchdog
```

`watchdog` launches `worker-1` and `worker-2`, prints its own PID and child PIDs to stdout, and logs all lifecycle events to `watchdog.log`.

### Interacting via Signals (from Another Terminal)

```bash
# Request a timestamped status report in watchdog.log
kill -SIGUSR1 <WATCHDOG_PID>

# Simulate unexpected termination of a specific worker (verify worker PID first)
kill -SIGKILL <WORKER_PID>

# Trigger graceful shutdown
kill -SIGTERM <WATCHDOG_PID>
# Or press Ctrl+C (SIGINT) in the terminal running ./watchdog
```

### Command-Line Options

```text
Usage: ./watchdog [options]
Options:
  -l <path>       Log file path (default: watchdog.log)
  -m <count>      Max automatic restarts per child (default: 3)
  -i <seconds>    Periodic SIGALRM check interval, 0=disable (default: 5)
  -t <seconds>    Graceful shutdown timeout before SIGKILL (default: 5)
  -w <spec>       Worker spec 'name:mode[:duration]' (repeatable)
                  Modes: normal, exit-ok, crash, signal-crash, ignore-term
  -q              Quiet mode (write logs to file only)
  -h              Display this help message
```

### Example with Custom Workers

```bash
./watchdog -l custom.log -i 2 -m 3 -t 3 \
  -w worker-1:normal:0 \
  -w worker-2:exit-ok:2 \
  -w worker-3:ignore-term:0
```

## Automated Test Suite

Run all 7 required test scenarios (including separate `SIGINT` and `SIGKILL` timeout escalation verification):

```bash
make test
```

Covered scenarios:
1. **Normal operation** — launches >=2 children, verifies live PIDs and `SIGALRM` periodic health checks.
2. **Normal termination** — verifies `exit(0)` child is detected, reaped, marked `EXITED_NORMAL`, and not restarted.
3. **Unexpected termination** — verifies both `SIGKILL` and non-zero exit code (`exit(1)`) trigger automatic restart with a new PID.
4. **Restart limit** — kills a worker 4 consecutive times and verifies exactly 3 restarts followed by `FAILED` state and halt.
5. **Status reporting (`SIGUSR1`)** — verifies timestamped report contains Watchdog PID, child PIDs, states, and restart counts.
6. **Graceful shutdown (`SIGTERM`, `SIGINT` / Ctrl+C, and `SIGKILL` timeout fallback)** — verifies controlled termination and reaping with zero surviving or zombie children.
7. **Log verification** — validates timestamp formatting and event coverage across all logs.

Verification outputs and a full sample log are archived in [`docs/test_results.txt`](docs/test_results.txt) and [`docs/sample_watchdog.log`](docs/sample_watchdog.log).
