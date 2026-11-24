#!/bin/sh
set -eu

cd "$(dirname "$0")/../.."
export LC_ALL=C
demo_dir=$(mktemp -d)
trap 'rm -rf "$demo_dir"' 0
program="$demo_dir/filediffadvanced"

cc -std=c11 -Wall -Wextra -Wpedantic -Isource/filediffadvanced -o "$program" \
    source/filediffadvanced/filediffadvanced.c tests/filediffadvanced/driver.c

run_comparison() {
    status=0
    "$program" "$@" || status=$?
    if [ "$status" -ne 1 ]; then
        printf 'Demo expected different files (exit 1), got exit %s.\n' "$status" >&2
        return 1
    fi
}

printf 'Text comparison\n\nleft.txt\n'
cat left.txt
printf '\nright.txt\n'
cat right.txt
printf '\n'
run_comparison left.txt right.txt

printf '\nBinary comparison (-b)\n\n'
printf 'left.bin (hex):  %s\n' "$(od -An -tx1 left.bin | xargs)"
printf 'right.bin (hex): %s\n' "$(od -An -tx1 right.bin | xargs)"
printf '\n'
run_comparison -b left.bin right.bin
