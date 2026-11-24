# Operating Systems Group Project

## Filediffadvanced presentation demo

The four sample files are in the project root.

Start `./custom_shell` from the project root. At the `custom-shell>` prompt,
run these three commands when it is time to demonstrate `filediffadvanced`.

```text
filediffadvanced -h
filediffadvanced left.txt right.txt
filediffadvanced -b left.bin right.bin
```

- The help output shows the command syntax and the `-b` flag for binary mode.
- The text comparison reports `First difference: line 2` because `two` changes
  to `new`. Both files are 8 bytes.
- The binary comparison reports `First difference: byte 2` because `02` changes
  to `04`. Both files are 3 bytes.

Both comparisons show elapsed time and CPU time. `[exit status: 1]` means the
files differ, which is expected for these two examples.

## Log analyzer (command 5)

The log analyzer counts ERROR, WARN/WARNING, INFO, and Other lines in a
saved log file using `mmap()`. It ignores letter case and matches whole
words. Each line counts once, with ERROR taking priority over warnings,
then INFO. Blank lines count as Other, and a final line without a newline
still counts.

The files follow the shared project layout:

- `source/loganalyzer/`: C source and header.
- `tests/loganalyzer/`: tests and sample log.
- `manpages/hamani_loganalyzer.1`: man page.

From the project root, build and try the standalone command:

```sh
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -DLOGANALYZER_STANDALONE \
    source/loganalyzer/hamani_loganalyzer.c -o hamani_loganalyzer
./hamani_loganalyzer -f tests/loganalyzer/hamani_loganalyzer_sample.log
```

Expected counts: 6 total lines, 2 errors, 1 warning, 2 info, and 1 other.

Run the tests and read the manual:

```sh
sh tests/loganalyzer/hamani_loganalyzer_test.sh
man ./manpages/hamani_loganalyzer.1
```

The test script builds its own temporary programs and removes them when
finished. It checks the counts, input errors, and repeated calls from one
process, including restoration of the shell's signal handlers.

For the Custom Shell, include `loganalyzer/hamani_loganalyzer.h` from
`source/main.c` and link `source/loganalyzer/hamani_loganalyzer.c` without
`-DLOGANALYZER_STANDALONE`. The shell calls
`loganalyzer(command_argc, command_argv)` for the `loganalyzer` command.
The function returns a status instead of exiting the shell.



## Filecrypt (command 3)

Filecrypt encrypts and decrypts files using AES-256-GCM authenticated
encryption. It accepts a password from the terminal or a key file.

The files follow the shared project layout:

- `source/filecrypt/`: source code and header files.
- `tests/filecrypt/`: tests and sample input.
- `manpages/filecrypt.1`: man page.

### Requirements

Linux:

```sh
sudo apt install build-essential libssl-dev pkg-config
```

macOS:

```sh
brew install openssl@3 pkg-config
```

### Testing

From the project root, run:

```sh
./tests/filecrypt/test_filecrypt.sh
```

The test script compiles filecrypt in a temporary directory, runs the
tests, and removes the temporary executable.

Read the manual with:

```sh
man ./manpages/filecrypt.1
```

### Custom shell integration

The custom shell includes:

```c
#include "filecrypt/filecrypt.h"
```

The shell runs the command with:

```c
filecrypt_main(command_argc, command_argv);
```

When building the custom shell, compile filecrypt with
`FILECRYPT_NO_MAIN` and link OpenSSL using `-lcrypto`.

## Processgroup presentation demo

`processgroup` runs a batch of processes as one process group. It can signal
the whole group at once and monitor each member's CPU time and memory. It
reads `/proc`, so run the demo on **Linux**. The demo uses the standalone
build because `netstatplus` does not compile on Linux yet.

### Setup

From the project root:

```sh
sudo apt install build-essential procps    # if gcc or ps is missing
gcc -std=c11 -Wall -Wextra -pthread source/processgroup/processgroup.c -o processgroup
```

### Demo commands

Run these in order. The whole demo takes about two minutes.

**1. Help**

```sh
./processgroup -h
```

Shows the three modes: launch copies of a command (`-n`), launch a batch file
(`-f`), or attach to an existing group (`-p`).

**2. Launch, monitor, and signal the whole group**

```sh
./processgroup -n 3 -m -i 1 -s TERM -a 3 -- sha256sum /dev/zero
```

Starts three CPU-bound processes in one new group. The monitor table prints
every second and CPU time climbs about 1 second per sample. After 3 seconds,
one `kill(-pgid, SIGTERM)` stops all three. Each `[done]` line reports
`killed by signal 15 (Terminated)` with that process's user/sys time and peak
memory. The summary shows `3 abnormal exit(s)` and exit status 1, which is
expected here.

**3. Batch file**

```sh
cat tests/processgroup/batch_example.txt
./processgroup -m -f tests/processgroup/batch_example.txt
```

Runs three different commands from the file in one group. `echo` and `uname`
finish right away. The monitor then shows only `sleep` left in the group
until it exits after 2 seconds. The summary shows `0 abnormal exit(s)`.

**4. Attach to a group that is already running**

```sh
sh -c 'sleep 300 & sleep 300 & wait' &
PG=$!
sleep 1                           # let sh start both sleeps
./processgroup -p $PG -m -c 1     # 3 members, STATE S (sleeping)
./processgroup -p $PG -s STOP
./processgroup -p $PG -m -c 1     # same 3 members, STATE T (stopped)
./processgroup -p $PG -s CONT
./processgroup -p $PG -s TERM
./processgroup -p $PG -m -c 1     # "No such process": the group is gone
```

An interactive shell puts each background job in its own process group, so
`$!` is both the job's PID and its PGID. Attach mode pauses, resumes, and
terminates the shell and both of its `sleep` children with one signal each.

**5. Ctrl-C is forwarded to the group (optional)**

```sh
./processgroup -n 3 -- sleep 60
```

Press Ctrl-C after a second. The children are in their own group, so the
terminal does not signal them directly. `processgroup` catches SIGINT and
forwards it to the group, and all three report `killed by signal 2
(Interrupt)`. A second Ctrl-C would escalate to SIGKILL.

### Tests and manual

```sh
cp processgroup tests/processgroup/
bash tests/processgroup/test_processgroup.sh    # expect: Passed: 24   Failed: 0
rm tests/processgroup/processgroup
man ./manpages/processgroup.1
```

In the Custom Shell, `processgroup` is registered in `source/main.c` and
calls `processgroup_main(argc, argv)`. Compile it with `-DPG_NO_MAIN -pthread`.
