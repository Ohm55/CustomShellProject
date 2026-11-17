#!/bin/sh
set -eu

cd "$(dirname "$0")/../.."
export LC_ALL=C

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
trap 'exit 1' HUP INT TERM
logfile="$tmpdir/log file.log"
program="$tmpdir/hamani_loganalyzer"
sample="tests/loganalyzer/hamani_loganalyzer_sample.log"
compiler=${CC:-cc}

"$compiler" -std=c11 -O2 -Wall -Wextra -Wpedantic \
    -DLOGANALYZER_STANDALONE source/loganalyzer/hamani_loganalyzer.c \
    -o "$program"

check_counts() {
    file=$1
    shift
    output=$("$program" -f "$file")
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

before=$(cksum "$sample")
check_counts "$sample" 6 2 1 2 1
[ "$before" = "$(cksum "$sample")" ]

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
expect_failure 2 "$program"
expect_failure 2 "$program" -f
expect_failure 2 "$program" -z
expect_failure 2 "$program" -f "$sample" extra
expect_failure 1 "$program" -f "$tmpdir/missing.log"
expect_failure 1 "$program" -f "$tmpdir"
mkfifo "$tmpdir/pipe"
expect_failure 1 "$program" -f "$tmpdir/pipe"
chmod 000 "$logfile"
if [ ! -r "$logfile" ]; then
    expect_failure 1 "$program" -f "$logfile"
fi
chmod 600 "$logfile"

"$program" -h | grep -q '^Usage:'
"$program" -f"$sample" | grep -q '^Total lines: 6$'
echo 'All loganalyzer command tests passed.'

"$compiler" -std=c11 -O2 -Wall -Wextra -Wpedantic \
    -Isource/loganalyzer source/loganalyzer/hamani_loganalyzer.c \
    tests/loganalyzer/hamani_loganalyzer_repeat_calls.c \
    -o "$tmpdir/hamani_loganalyzer_repeat_calls"
"$tmpdir/hamani_loganalyzer_repeat_calls" "$sample" > /dev/null 2>&1
echo 'Repeated shell calls passed.'
