#!/usr/bin/env bash

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
TEMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/filecrypt-test.XXXXXX")
PROGRAM="$TEMP_DIR/filecrypt"
TEST_INPUT="$SCRIPT_DIR/test_input.txt"

PASSED=0
FAILED=0

cleanup()
{
    rm -rf -- "$TEMP_DIR"
}

trap cleanup EXIT HUP INT TERM

CC=${CC:-cc}

if ! command -v pkg-config >/dev/null 2>&1; then
    echo "pkg-config is required."
    exit 1
fi

# Homebrew installs OpenSSL in a separate directory on macOS.
if command -v brew >/dev/null 2>&1; then
    OPENSSL_PC="$(brew --prefix openssl@3)/lib/pkgconfig"
    PKG_CONFIG_PATH="$OPENSSL_PC${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export PKG_CONFIG_PATH
fi

if ! OPENSSL_CFLAGS=$(pkg-config --cflags openssl); then
    echo "OpenSSL development files were not found."
    exit 1
fi

if ! OPENSSL_LIBS=$(pkg-config --libs openssl); then
    echo "OpenSSL development files were not found."
    exit 1
fi

if ! "$CC" \
    -std=c11 \
    -O2 \
    -Wall \
    -Wextra \
    -Wpedantic \
    $OPENSSL_CFLAGS \
    "$PROJECT_ROOT/source/filecrypt/filecrypt.c" \
    "$PROJECT_ROOT/source/filecrypt/crypto.c" \
    -o "$PROGRAM" \
    $OPENSSL_LIBS
then
    echo "Failed to compile filecrypt."
    exit 1
fi

pass()
{
    printf 'PASS: %s\n' "$1"
    PASSED=$((PASSED + 1))
}

fail()
{
    printf 'FAIL: %s\n' "$1"
    FAILED=$((FAILED + 1))
}

expect_success()
{
    description=$1
    shift
    if "$@" >/dev/null 2>&1; then pass "$description"; else fail "$description"; fi
}

expect_failure()
{
    description=$1
    shift
    if "$@" >/dev/null 2>&1; then fail "$description"; else pass "$description"; fi
}



printf '%s\n' 'correct horse battery staple' > "$TEMP_DIR/key"
printf '%s\n' 'wrong password' > "$TEMP_DIR/wrong-key"
chmod 600 "$TEMP_DIR/key" "$TEMP_DIR/wrong-key"

# Validate option parsing and required arguments.
expect_success "help option" "$PROGRAM" --help
expect_failure "missing operation" "$PROGRAM" -i "$TEST_INPUT" -o "$TEMP_DIR/x"
expect_failure "both operations" "$PROGRAM" -e -d -i "$TEST_INPUT" -o "$TEMP_DIR/x"
expect_failure "missing input" "$PROGRAM" -e -i "$TEMP_DIR/missing" -o "$TEMP_DIR/x" -k "$TEMP_DIR/key"
expect_failure "same input and output" "$PROGRAM" -e -i "$TEST_INPUT" -o "$TEST_INPUT" -k "$TEMP_DIR/key"

# Check round trips for text, empty, and binary inputs.
expect_success "encrypt text file" \
    "$PROGRAM" -e -i "$TEST_INPUT" -o "$TEMP_DIR/text.enc" -k "$TEMP_DIR/key"
expect_success "decrypt text file" \
    "$PROGRAM" -d -i "$TEMP_DIR/text.enc" -o "$TEMP_DIR/text.out" -k "$TEMP_DIR/key"
if cmp -s "$TEST_INPUT" "$TEMP_DIR/text.out"; then
    pass "text round trip matches"
else
    fail "text round trip matches"
fi

: > "$TEMP_DIR/empty"
expect_success "encrypt empty file" \
    "$PROGRAM" -e -i "$TEMP_DIR/empty" -o "$TEMP_DIR/empty.enc" -k "$TEMP_DIR/key"
expect_success "decrypt empty file" \
    "$PROGRAM" -d -i "$TEMP_DIR/empty.enc" -o "$TEMP_DIR/empty.out" -k "$TEMP_DIR/key"
if cmp -s "$TEMP_DIR/empty" "$TEMP_DIR/empty.out"; then
    pass "empty round trip matches"
else
    fail "empty round trip matches"
fi

dd if=/dev/urandom of="$TEMP_DIR/binary" bs=4096 count=4 2>/dev/null
expect_success "encrypt binary file" \
    "$PROGRAM" -e -i "$TEMP_DIR/binary" -o "$TEMP_DIR/binary.enc" -k "$TEMP_DIR/key"
expect_success "decrypt binary file" \
    "$PROGRAM" -d -i "$TEMP_DIR/binary.enc" -o "$TEMP_DIR/binary.out" -k "$TEMP_DIR/key"
if cmp -s "$TEMP_DIR/binary" "$TEMP_DIR/binary.out"; then
    pass "binary round trip matches"
else
    fail "binary round trip matches"
fi

# Authentication failures must not leave a completed output.
expect_failure "wrong password is rejected" \
    "$PROGRAM" -d -i "$TEMP_DIR/text.enc" -o "$TEMP_DIR/wrong.out" -k "$TEMP_DIR/wrong-key"
if [ ! -e "$TEMP_DIR/wrong.out" ]; then
    pass "wrong-password output is removed"
else
    fail "wrong-password output is removed"
fi

cp "$TEMP_DIR/text.enc" "$TEMP_DIR/damaged.enc"
printf '\001' | dd of="$TEMP_DIR/damaged.enc" bs=1 seek=45 conv=notrunc 2>/dev/null
expect_failure "damaged ciphertext is rejected" \
    "$PROGRAM" -d -i "$TEMP_DIR/damaged.enc" -o "$TEMP_DIR/damaged.out" -k "$TEMP_DIR/key"

printf 'existing\n' > "$TEMP_DIR/existing"
expect_failure "existing output is protected" \
    "$PROGRAM" -e -i "$TEST_INPUT" -o "$TEMP_DIR/existing" -k "$TEMP_DIR/key"
expect_success "force replaces existing output" \
    "$PROGRAM" -f -e -i "$TEST_INPUT" -o "$TEMP_DIR/existing" -k "$TEMP_DIR/key"

printf '\nPassed: %d\nFailed: %d\n' "$PASSED" "$FAILED"
[ "$FAILED" -eq 0 ]
