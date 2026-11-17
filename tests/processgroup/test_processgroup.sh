#!/bin/bash
# Test cases for processgroup. Run: make test   (or ./test_processgroup.sh)
cd "$(dirname "$0")" || exit 1
PG=${PG:-./processgroup}
pass=0; fail=0

check() {   # check "name" "command-that-returns-0-on-success"
    if eval "$2" >/dev/null 2>&1; then echo "PASS: $1"; pass=$((pass+1))
    else echo "FAIL: $1"; fail=$((fail+1)); fi
}

# ---- Argument handling / error cases ----
check "T1  -h prints usage and exits 0"            "$PG -h | grep -q Usage"
check "T2  no arguments -> usage, exit 1"          "! $PG"
check "T3  unknown option rejected"                "! $PG -z true"
check "T4  invalid count rejected"                 "! $PG -n 0 true"
check "T5  unknown signal name rejected"           "! $PG -s BOGUS -a 1 sleep 1"
check "T6  missing batch file rejected"            "! $PG -f /no/such/file"
check "T7  -p with nonexistent group rejected"     "! $PG -p 999999 -s TERM"
check "T8  -p without -s/-m rejected"              "! $PG -p 2"
check "T9  nonexistent command reported as failure" "! $PG no_such_cmd_xyz"

# ---- Launching ----
check "T10 single command, exit 0"                 "$PG true"
check "T11 failing command -> exit 1"              "! $PG false"
check "T12 -n 3 runs three processes"              "[ \$($PG -n 3 true | grep -c '^\[done\]') -eq 3 ]"
check "T13 all members share one process group"    "$PG -n 3 -- sh -c 'ps -o pgid= -p \$\$' | grep -v '^\[' | sort -u | wc -l | grep -qx 1"
check "T14 batch file runs all commands"          "[ \$($PG -f batch_example.txt | grep -c '^\[done\]') -eq 3 ]"

# ---- Collective signalling ----
check "T15 -s TERM -a 0.5 kills whole group" \
      "$PG -n 4 -s TERM -a 0.5 sleep 30 | grep -c 'Terminated' | grep -qx 4"
check "T16 timed kill finishes quickly" \
      "s=\$(date +%s); $PG -n 2 -s KILL -a 0.3 sleep 30; [ \$((\$(date +%s)-s)) -lt 5 ]"
check "T17 signal given by number (15)" \
      "$PG -n 2 -s 15 -a 0.3 sleep 30 | grep -q 'killed by signal 15'"

# ---- Monitoring ----
check "T18 -m prints monitor table" \
      "$PG -m -i 0.2 -n 2 sleep 1 | grep -q 'CPU(s)'"
check "T19 per-child rusage reported" \
      "$PG true | grep -q 'maxrss'"

# ---- Attach mode ----
setsid sleep 30 & sleep 0.3
PGID=$(ps -o pgid= -p $! | tr -d ' ')
check "T20 attach: monitor existing group" "$PG -p $PGID -m -c 1 | grep -q sleep"
check "T21 attach: SIGSTOP then SIGCONT"   "$PG -p $PGID -s STOP && $PG -p $PGID -s CONT"
check "T22 attach: collective SIGTERM"     "$PG -p $PGID -s TERM"
sleep 0.3
check "T23 attach: group is gone afterwards" "! $PG -p $PGID -s TERM"

# ---- Ctrl-C style interruption forwarded to the group ----
$PG -n 2 sleep 30 >/tmp/pg_int.out 2>&1 &
PGPID=$!; sleep 0.5; kill -INT $PGPID; wait $PGPID 2>/dev/null
check "T24 SIGINT to processgroup stops the batch" "grep -q 'Interrupt' /tmp/pg_int.out"
rm -f /tmp/pg_int.out

echo; echo "Passed: $pass   Failed: $fail"
[ "$fail" -eq 0 ]
