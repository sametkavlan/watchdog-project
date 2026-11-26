#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_DIR"

WATCHDOG="./watchdog"
TMP_DIR="$SCRIPT_DIR/tmp"
mkdir -p "$TMP_DIR"

PASS_COUNT=0
FAIL_COUNT=0
ACTIVE_PIDS=()

cleanup() {
    for pid in "${ACTIVE_PIDS[@]:-}"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill -s TERM "$pid" 2>/dev/null || true
            sleep 0.2
            kill -s KILL "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    printf "[PASS] %s\n" "$1"
}

fail() {
    FAIL_COUNT=$((FAIL_COUNT + 1))
    printf "[FAIL] %s\n" "$1" >&2
}

assert_log_contains() {
    local logfile="$1"
    local pattern="$2"
    local desc="$3"
    if grep -qE "$pattern" "$logfile"; then
        pass "$desc"
    else
        fail "$desc (pattern not found: $pattern)"
        echo "--- Log snippet ($logfile) ---" >&2
        tail -n 30 "$logfile" >&2
        echo "------------------------------" >&2
    fi
}

wait_for_log() {
    local logfile="$1"
    local pattern="$2"
    local timeout_tenths="${3:-50}" # default 5.0s
    local i=0
    while [ "$i" -lt "$timeout_tenths" ]; do
        if [ -f "$logfile" ] && grep -qE "$pattern" "$logfile"; then
            return 0
        fi
        sleep 0.1
        i=$((i + 1))
    done
    return 1
}

wait_for_exit() {
    local pid="$1"
    local timeout_tenths="${2:-60}"
    local i=0
    while [ "$i" -lt "$timeout_tenths" ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            wait "$pid" 2>/dev/null
            return $?
        fi
        sleep 0.1
        i=$((i + 1))
    done
    return 124
}

extract_latest_pid() {
    local logfile="$1"
    local worker_name="$2"
    grep -E "(Started|Restarted) child '${worker_name}' with (new )?PID [0-9]+" "$logfile" \
        | tail -n 1 \
        | sed -E 's/.*PID ([0-9]+).*/\1/'
}

verify_is_worker_pid() {
    local pid="$1"
    if [ -z "$pid" ]; then
        return 1
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
        return 1
    fi
    ps -p "$pid" -o args= 2>/dev/null | grep -q "worker"
}

echo "============================================================"
echo " Watchdog POSIX Process Supervision - Automated Test Suite"
echo "============================================================"

if [ ! -x "$WATCHDOG" ] || [ ! -x "./worker" ]; then
    echo "Building binaries with make..."
    make all
fi

# ------------------------------------------------------------------
# Scenario 1 & 5 & 6a: Normal Operation, Periodic Check, SIGUSR1, SIGTERM
# ------------------------------------------------------------------
echo ""
echo "--- Scenario 1, 5 & 6a: Normal Operation, SIGALRM, SIGUSR1, SIGTERM ---"
LOG1="$TMP_DIR/scenario1.log"
rm -f "$LOG1"

"$WATCHDOG" -q -l "$LOG1" -i 1 -t 3 &
WD_PID=$!
ACTIVE_PIDS+=("$WD_PID")

wait_for_log "$LOG1" "Started child 'worker-2' with PID" 40
W1_PID="$(extract_latest_pid "$LOG1" "worker-1")"
W2_PID="$(extract_latest_pid "$LOG1" "worker-2")"

if verify_is_worker_pid "$W1_PID" && verify_is_worker_pid "$W2_PID"; then
    pass "Scenario 1: Watchdog started >=2 live worker processes (PIDs $W1_PID, $W2_PID)"
else
    fail "Scenario 1: Expected two live worker processes (got '$W1_PID' and '$W2_PID')"
fi

# Wait for periodic SIGALRM health check
if wait_for_log "$LOG1" "Periodic health check \(SIGALRM\): running=2" 30; then
    pass "Scenario 1b: Periodic SIGALRM health check detected 2 running children"
else
    fail "Scenario 1b: Periodic SIGALRM health check did not appear in log"
fi

# Scenario 5: SIGUSR1 status reporting
kill -s USR1 "$WD_PID"
if wait_for_log "$LOG1" "=== End of Status Report ===" 30; then
    assert_log_contains "$LOG1" "Status Report \(SIGUSR1\).*Watchdog PID: ${WD_PID}" \
        "Scenario 5a: SIGUSR1 triggered status report header with Watchdog PID"
    assert_log_contains "$LOG1" "worker-1.*PID: ${W1_PID}.*State: RUNNING" \
        "Scenario 5b: SIGUSR1 reported worker-1 PID and RUNNING state"
    assert_log_contains "$LOG1" "worker-2.*PID: ${W2_PID}.*State: RUNNING" \
        "Scenario 5c: SIGUSR1 reported worker-2 PID and RUNNING state"
else
    fail "Scenario 5: SIGUSR1 status report not written to log"
fi

# Scenario 6a: Graceful shutdown on SIGTERM
kill -s TERM "$WD_PID"
wait_for_exit "$WD_PID" 50
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "Scenario 6a: Watchdog exited with code 0 on SIGTERM"
else
    fail "Scenario 6a: Watchdog exit code on SIGTERM was $RC (expected 0)"
fi

if ! kill -0 "$W1_PID" 2>/dev/null && ! kill -0 "$W2_PID" 2>/dev/null; then
    pass "Scenario 6a: All child processes terminated and reaped after SIGTERM"
else
    fail "Scenario 6a: One or more child processes survived SIGTERM shutdown"
fi
assert_log_contains "$LOG1" "Received shutdown signal [0-9]+ \(SIGTERM\)" \
    "Scenario 6a: Log recorded SIGTERM graceful shutdown initiation"
assert_log_contains "$LOG1" "Graceful shutdown complete" \
    "Scenario 6a: Log recorded graceful shutdown completion"

# ------------------------------------------------------------------
# Scenario 2: Normal Termination (exit 0 -> detected, NOT restarted)
# ------------------------------------------------------------------
echo ""
echo "--- Scenario 2: Normal Termination ---"
LOG2="$TMP_DIR/scenario2.log"
rm -f "$LOG2"

"$WATCHDOG" -q -l "$LOG2" -i 0 -t 3 \
    -w "worker-1:normal:0" \
    -w "worker-2:exit-ok:1" &
WD_PID=$!
ACTIVE_PIDS+=("$WD_PID")

if wait_for_log "$LOG2" "Child 'worker-2' \(PID [0-9]+\) exited normally with exit code 0; not restarting" 40; then
    pass "Scenario 2a: Normal termination (exit 0) detected and not restarted"
else
    fail "Scenario 2a: Normal termination was not detected properly"
fi

sleep 0.3
if ! grep -q "Restarted child 'worker-2'" "$LOG2"; then
    pass "Scenario 2b: Verified worker-2 was never restarted after normal exit"
else
    fail "Scenario 2b: worker-2 was erroneously restarted after normal exit"
fi

kill -s USR1 "$WD_PID"
wait_for_log "$LOG2" "=== End of Status Report ===" 30
assert_log_contains "$LOG2" "worker-2.*State: EXITED_NORMAL" \
    "Scenario 2c: Status report reflects EXITED_NORMAL for worker-2"

kill -s TERM "$WD_PID"
wait_for_exit "$WD_PID" 50 >/dev/null

# ------------------------------------------------------------------
# Scenario 3 & 4: Unexpected Termination (SIGKILL) & Restart Limit (3)
# ------------------------------------------------------------------
echo ""
echo "--- Scenario 3 & 4: Unexpected Termination & Restart Limit (max 3) ---"
LOG3="$TMP_DIR/scenario3_4.log"
rm -f "$LOG3"

"$WATCHDOG" -q -l "$LOG3" -i 0 -m 3 -t 3 &
WD_PID=$!
ACTIVE_PIDS+=("$WD_PID")

wait_for_log "$LOG3" "Started child 'worker-2' with PID" 40
ORIG_W1_PID="$(extract_latest_pid "$LOG3" "worker-1")"
CUR_W1_PID="$ORIG_W1_PID"

for attempt in 1 2 3; do
    if ! verify_is_worker_pid "$CUR_W1_PID"; then
        fail "Scenario 3/4: Failed to verify worker-1 PID ($CUR_W1_PID) before SIGKILL #$attempt"
        break
    fi
    kill -s KILL "$CUR_W1_PID"
    if wait_for_log "$LOG3" "Restarted child 'worker-1' with new PID [0-9]+ \(mode=normal, restart ${attempt}/3\)" 40; then
        NEW_W1_PID="$(extract_latest_pid "$LOG3" "worker-1")"
        if [ "$NEW_W1_PID" != "$CUR_W1_PID" ] && verify_is_worker_pid "$NEW_W1_PID"; then
            pass "Scenario 3/4: SIGKILL #$attempt detected; worker-1 restarted ($attempt/3) with new PID $NEW_W1_PID"
            CUR_W1_PID="$NEW_W1_PID"
        else
            fail "Scenario 3/4: Restart #$attempt did not produce a new live worker PID"
        fi
    else
        fail "Scenario 3/4: Watchdog did not restart worker-1 on attempt $attempt/3"
    fi
done

# 4th SIGKILL must exceed limit and NOT restart
if verify_is_worker_pid "$CUR_W1_PID"; then
    kill -s KILL "$CUR_W1_PID"
    if wait_for_log "$LOG3" "Child 'worker-1' exceeded maximum restart limit \(3/3\); stopping retries" 40; then
        pass "Scenario 4a: 4th unexpected termination hit restart limit (3/3) and stopped retrying"
    else
        fail "Scenario 4a: Restart limit exceeded message not logged"
    fi
else
    fail "Scenario 4a: Could not verify worker-1 PID before 4th SIGKILL"
fi

sleep 0.4
RESTART_COUNT="$(grep -c "Restarted child 'worker-1'" "$LOG3" || true)"
if [ "$RESTART_COUNT" -eq 3 ]; then
    pass "Scenario 4b: Verified total restarts for worker-1 is exactly 3"
else
    fail "Scenario 4b: Expected 3 restarts for worker-1, found $RESTART_COUNT"
fi

kill -s USR1 "$WD_PID"
wait_for_log "$LOG3" "=== End of Status Report ===" 30
assert_log_contains "$LOG3" "worker-1.*State: FAILED.*Restarts: 3/3" \
    "Scenario 4c: Status report marks worker-1 as FAILED (3/3)"

kill -s TERM "$WD_PID"
wait_for_exit "$WD_PID" 50 >/dev/null

# ------------------------------------------------------------------
# Scenario 3b: Non-zero Error Exit Code -> Unexpected Termination
# ------------------------------------------------------------------
echo ""
echo "--- Scenario 3b: Non-zero Exit Code Distinguished from Normal Exit ---"
LOG3B="$TMP_DIR/scenario3b.log"
rm -f "$LOG3B"

"$WATCHDOG" -q -l "$LOG3B" -i 0 -m 1 -t 3 \
    -w "worker-1:normal:0" \
    -w "crash-worker:crash:1" &
WD_PID=$!
ACTIVE_PIDS+=("$WD_PID")

if wait_for_log "$LOG3B" "Child 'crash-worker' \(PID [0-9]+\) terminated unexpectedly with exit code 1" 40 && \
   wait_for_log "$LOG3B" "Restarted child 'crash-worker' with new PID [0-9]+ \(mode=crash, restart 1/1\)" 40 && \
   wait_for_log "$LOG3B" "Child 'crash-worker' exceeded maximum restart limit \(1/1\)" 40; then
    pass "Scenario 3b: Non-zero exit code (1) treated as unexpected, restarted once, then halted at limit"
else
    fail "Scenario 3b: Non-zero exit handling failed"
fi

kill -s TERM "$WD_PID"
wait_for_exit "$WD_PID" 50 >/dev/null

# ------------------------------------------------------------------
# Scenario 6b: Graceful Shutdown on SIGINT (Ctrl+C tested separately)
# ------------------------------------------------------------------
echo ""
echo "--- Scenario 6b: Graceful Shutdown on SIGINT (Ctrl+C) ---"
LOG6B="$TMP_DIR/scenario6b.log"
rm -f "$LOG6B"

"$WATCHDOG" -q -l "$LOG6B" -i 0 -t 3 &
WD_PID=$!
ACTIVE_PIDS+=("$WD_PID")

wait_for_log "$LOG6B" "Started child 'worker-2' with PID" 40
W1_PID="$(extract_latest_pid "$LOG6B" "worker-1")"
W2_PID="$(extract_latest_pid "$LOG6B" "worker-2")"

kill -s INT "$WD_PID"
wait_for_exit "$WD_PID" 50
RC=$?
if [ "$RC" -eq 0 ] && ! kill -0 "$W1_PID" 2>/dev/null && ! kill -0 "$W2_PID" 2>/dev/null; then
    pass "Scenario 6b: SIGINT (Ctrl+C) cleanly terminated and reaped all children (exit 0)"
else
    fail "Scenario 6b: SIGINT shutdown failed (rc=$RC)"
fi
assert_log_contains "$LOG6B" "Received shutdown signal [0-9]+ \(SIGINT\)" \
    "Scenario 6b: Log recorded SIGINT shutdown signal"

# ------------------------------------------------------------------
# Scenario 6c: Graceful Shutdown Timeout -> SIGKILL Fallback
# ------------------------------------------------------------------
echo ""
echo "--- Scenario 6c: Unresponsive Child Timeout -> SIGKILL Escalation ---"
LOG6C="$TMP_DIR/scenario6c.log"
rm -f "$LOG6C"

"$WATCHDOG" -q -l "$LOG6C" -i 0 -t 2 \
    -w "worker-1:normal:0" \
    -w "stubborn:ignore-term:0" &
WD_PID=$!
ACTIVE_PIDS+=("$WD_PID")

wait_for_log "$LOG6C" "Started child 'stubborn' with PID" 40
STUB_PID="$(extract_latest_pid "$LOG6C" "stubborn")"
W1_PID="$(extract_latest_pid "$LOG6C" "worker-1")"

kill -s TERM "$WD_PID"
wait_for_exit "$WD_PID" 60
RC=$?
if [ "$RC" -eq 0 ] && ! kill -0 "$STUB_PID" 2>/dev/null && ! kill -0 "$W1_PID" 2>/dev/null; then
    pass "Scenario 6c: Unresponsive child escalated to SIGKILL after timeout and reaped (exit 0)"
else
    fail "Scenario 6c: Timeout SIGKILL escalation failed (rc=$RC)"
fi
assert_log_contains "$LOG6C" "Sent SIGKILL to unresponsive child 'stubborn'" \
    "Scenario 6c: Log recorded SIGKILL escalation for unresponsive child"

# ------------------------------------------------------------------
# Scenario 7: Timestamp & Log Format Verification
# ------------------------------------------------------------------
echo ""
echo "--- Scenario 7: Log Format & Timestamp Verification ---"
BAD_LINES=0
for lf in "$LOG1" "$LOG2" "$LOG3" "$LOG6B" "$LOG6C"; do
    COUNT=$(grep -vcE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\] \[(INFO|WARN|ERROR|STATUS)\] ' "$lf" || true)
    BAD_LINES=$((BAD_LINES + COUNT))
done

if [ "$BAD_LINES" -eq 0 ]; then
    pass "Scenario 7: All log records across all scenarios match timestamped POSIX log format"
else
    fail "Scenario 7: Found $BAD_LINES malformed log line(s)"
fi

echo ""
echo "============================================================"
printf " Summary: %d PASSED, %d FAILED\n" "$PASS_COUNT" "$FAIL_COUNT"
echo "============================================================"

if [ "$FAIL_COUNT" -ne 0 ]; then
    exit 1
fi
exit 0
