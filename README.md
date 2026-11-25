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
the whole group at once and monitor each member's CPU time and memory. The
demo runs inside the Custom Shell on **macOS**.

### Setup

Install OpenSSL for filecrypt (`brew install openssl@3`), then build the shell
from the project root:

```sh
cc -std=c11 -pthread -DPG_NO_MAIN -DFILECRYPT_NO_MAIN -DNETSTATPLUS_NO_MAIN \
    -I"$(brew --prefix openssl@3)/include" \
    source/main.c source/processgroup/processgroup.c \
    source/filediffadvanced/filediffadvanced.c \
    source/loganalyzer/hamani_loganalyzer.c source/netstatplus/netstatplus.c \
    source/filecrypt/filecrypt.c source/filecrypt/crypto.c \
    -L"$(brew --prefix openssl@3)/lib" -lcrypto -o custom_shell
./custom_shell
```

### Demo commands

At the `custom-shell>` prompt, run these three commands in order. The whole
demo takes under a minute of run time.

```text
processgroup -h
processgroup -n 3 -m -i 1 -s TERM -a 3 -- sha256sum /dev/zero
processgroup -m -f tests/processgroup/batch_example.txt
```

- The help output shows the three modes: copies of one command (`-n`), a batch
  file (`-f`), or an existing group (`-p`).
- The second command starts three CPU-bound processes in one new group. A
  monitor table prints every second, and CPU time climbs about 1 second per
  sample. After 3 seconds, one signal to the group ends all three. Each
  `[done]` line reports `killed by signal 15` with that process's CPU time and
  peak memory. `3 abnormal exit(s)` and `[exit status: 1]` are expected.
- The third command runs the three commands in the batch file as one group.
  `echo` and `uname` finish right away, the monitor shows only `sleep` left,
  and the summary shows `0 abnormal exit(s)`.

If `sha256sum` is not found (older macOS), use `md5 /dev/zero` instead.

### Tests and manual

The test script uses Linux tools (`/proc`, `setsid`), so run it on Linux with
the standalone build:

```sh
gcc -std=c11 -Wall -Wextra -pthread source/processgroup/processgroup.c \
    -o tests/processgroup/processgroup
bash tests/processgroup/test_processgroup.sh    # expect: Passed: 24   Failed: 0
man ./manpages/processgroup.1
```

`processgroup` monitors through `/proc` on Linux and `libproc` on macOS.
