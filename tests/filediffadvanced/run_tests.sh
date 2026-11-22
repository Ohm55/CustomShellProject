#!/bin/sh
set -eu

cd "$(dirname "$0")/../.."
export LC_ALL=C
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' 0
program="$test_dir/filediffadvanced"

cc -std=c11 -Wall -Wextra -Wpedantic -Isource/filediffadvanced -o "$program" \
    source/filediffadvanced/filediffadvanced.c tests/filediffadvanced/driver.c

check() {
    expected_status=$1
    expected_output=$2
    shift 2

    status=0
    "$program" "$@" > "$test_dir/output" 2>&1 || status=$?
    if [ "$status" -ne "$expected_status" ] ||
        ! grep -Fq -- "$expected_output" "$test_dir/output"; then
        printf 'Failed: filediffadvanced %s\n' "$*" >&2
        printf 'Expected status %s and text "%s", got status %s\n' \
            "$expected_status" "$expected_output" "$status" >&2
        cat "$test_dir/output" >&2
        exit 1
    fi
}

printf 'one\ntwo\n' > "$test_dir/left.txt"
cp "$test_dir/left.txt" "$test_dir/same.txt"
printf 'one\nnew\n' > "$test_dir/right.txt"
printf 'one\n' > "$test_dir/short.txt"
printf 'one\ntwo' > "$test_dir/no-newline.txt"
: > "$test_dir/empty"

check 0 'Files match' "$test_dir/left.txt" "$test_dir/same.txt"
grep -Fxq 'File sizes: left=8 bytes, right=8 bytes' "$test_dir/output"
grep -Eq '^Comparison time: [0-9]+\.[0-9]+ ms elapsed, [0-9]+\.[0-9]+ ms CPU$' "$test_dir/output"
check 1 'First difference: line 2' "$test_dir/left.txt" "$test_dir/right.txt"
check 1 'First difference: line 2' "$test_dir/left.txt" "$test_dir/short.txt"
check 1 'First difference: line 2' "$test_dir/left.txt" "$test_dir/no-newline.txt"
check 0 'Files match' "$test_dir/empty" "$test_dir/empty"

dd if=/dev/zero of="$test_dir/short.bin" bs=4096 count=1 2>/dev/null
cp "$test_dir/short.bin" "$test_dir/left.bin"
cp "$test_dir/short.bin" "$test_dir/right.bin"
printf '\001\002' >> "$test_dir/left.bin"
printf '\001\003' >> "$test_dir/right.bin"

check 0 'Files match' -b "$test_dir/short.bin" "$test_dir/short.bin"
check 1 'First difference: byte 4098' -b "$test_dir/left.bin" "$test_dir/right.bin"
check 1 'First difference: byte 4097' -b "$test_dir/short.bin" "$test_dir/left.bin"
check 2 "$test_dir/missing.txt:" "$test_dir/left.txt" "$test_dir/missing.txt"
check 2 'expected a regular file' "$test_dir" "$test_dir/left.txt"
check 2 'Usage: filediffadvanced' -x "$test_dir/left.txt" "$test_dir/right.txt"
check 2 'Usage: filediffadvanced' "$test_dir/left.txt"
check 0 'Usage: filediffadvanced' -h

printf 'filediffadvanced: all tests passed\n'
