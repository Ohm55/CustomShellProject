#!/bin/sh
set -eu

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
trap 'exit 1' HUP INT TERM
logfile="$tmpdir/log file.log"

check_counts() {
    file=$1
    shift
    output=$(./hamani_loganalyzer -f "$file")
    expected=$(printf 'File: %s\nTotal lines: %s\nErrors: %s\nWarnings: %s\nInfo: %s\nOther: %s\n' "$file" "$@")
    if [ "$output" != "$expected" ]; then
        printf 'FAIL: wrong counts for %s\nExpected:\n%s\nActual:\n%s\n' "$file" "$expected" "$output"
        exit 1
    fi
}

expect_failure() {
    expected_status=$1
    shift
    status=0
    "$@" > "$tmpdir/out" 2> "$tmpdir/err" || status=$?
    if [ "$status" -ne "$expected_status" ] || [ ! -s "$tmpdir/err" ] || [ -s "$tmpdir/out" ]; then
        printf 'FAIL: expected status %s and an error message: %s\n' "$expected_status" "$*"
        exit 1
    fi
}

before=$(cksum hamani_loganalyzer_sample.log)
check_counts hamani_loganalyzer_sample.log 6 2 1 2 1
[ "$before" = "$(cksum hamani_loganalyzer_sample.log)" ]

# Check an empty file and a last line with no newline.
: > "$logfile"
check_counts "$logfile" 0 0 0 0 0
printf 'error happened' > "$logfile"
check_counts "$logfile" 1 1 0 0 0

# Check letter case, mixed levels, and blank lines. Each line counts once.
printf '[eRrOr]\nwarn\nWARNING\ninfo\nDEBUG\n\nINFO WARNING ERROR\nINFO WARN\n' > "$logfile"
check_counts "$logfile" 8 2 3 1 2

# Check whole words next to punctuation, and skip parts of other words.
printf 'ERRORS\npreERROR\nERROR_code\ninformation\n[ERROR]\n(WARN)\nINFO:\n' > "$logfile"
check_counts "$logfile" 7 1 1 1 4

# Check Windows line endings and a line longer than a normal input buffer.
printf 'INFO ready\r\nERROR failed\r\n\r\n' > "$logfile"
check_counts "$logfile" 3 1 0 1 1
awk 'BEGIN { for (i = 0; i < 100000; i++) printf "a"; print " [ERROR]" }' > "$logfile"
check_counts "$logfile" 1 1 0 0 0

# Check the error codes for bad options and files that cannot be used.
expect_failure 2 ./hamani_loganalyzer
expect_failure 2 ./hamani_loganalyzer -f
expect_failure 2 ./hamani_loganalyzer -z
expect_failure 2 ./hamani_loganalyzer -f hamani_loganalyzer_sample.log extra
expect_failure 1 ./hamani_loganalyzer -f "$tmpdir/missing.log"
expect_failure 1 ./hamani_loganalyzer -f "$tmpdir"
mkfifo "$tmpdir/pipe"
expect_failure 1 ./hamani_loganalyzer -f "$tmpdir/pipe"
chmod 000 "$logfile"
if [ ! -r "$logfile" ]; then
    expect_failure 1 ./hamani_loganalyzer -f "$logfile"
fi
chmod 600 "$logfile"

./hamani_loganalyzer -h | grep -q '^Usage:'
./hamani_loganalyzer -fhamani_loganalyzer_sample.log | grep -q '^Total lines: 6$'
echo 'All loganalyzer command tests passed.'
