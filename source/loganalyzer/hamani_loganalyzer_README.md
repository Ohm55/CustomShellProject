# My part: loganalyzer (command 5)

My part of the Custom Shell project is the log analyzer.
The separate program is named `hamani_loganalyzer`. In the group shell,
the command can still be called `loganalyzer`. The idea is simple:
read a log file using `mmap()`, count the lines, and show how many are errors,
warnings, info messages, or other text.

## Running it

You need a C compiler and `make`. The project is meant to run on Linux.
The build file is named `hamani_loganalyzer_Makefile`, so use `-f` as shown below.

```sh
make -f hamani_loganalyzer_Makefile
./hamani_loganalyzer -f hamani_loganalyzer_sample.log
./hamani_loganalyzer -h
make -f hamani_loganalyzer_Makefile test
```

This is what the sample file should give:

```text
File: hamani_loganalyzer_sample.log
Total lines: 6
Errors: 2
Warnings: 1
Info: 2
Other: 1
```

To read the man page, run `man ./man/hamani_loganalyzer.1`.

## What the code does

1. `getopt()` checks the options. `-f` picks the file, and `-h` shows help.
2. `open()` opens the file for reading. `fstat()` checks its type and size.
3. `mmap()` makes the file data available in the program's memory space.
4. A loop goes through the data and counts the lines and message types.
5. `munmap()` removes the mapping when the command is done.

Uppercase and lowercase both work. The code checks whole words, so `ERROR`
counts, but `ERRORS` does not. `WARN` and `WARNING` both count as warnings.
Each line goes into one group. If it has more than one level, ERROR comes
first, then WARNING, then INFO. Everything else goes under Other.
Blank lines also count as Other. The last line still counts if it has no
newline at the end. An empty file gives zero for every count.

The code keeps a few counters rather than copying the whole log. It maps
the whole file, so the file has to fit in the program's address space.
Use a saved log file that stays the same while the command runs.

## How this connects to the lessons

- **Chapter 10, slides 5–10:** virtual address spaces and demand paging.
  This connects to `mmap()`: the program reads file data through memory,
  and the OS loads pages as they are needed.
- **Chapter 4, slide 51:** signal handling. SIGINT (Ctrl+C) and SIGTERM
  tell the command to stop scanning and remove its mapping.
- **Chapter 6, slide 6:** critical sections cover code that shares data
  being changed. This command uses one thread, so its counters do not
  need mutexes or semaphores.

## Checking it

`make -f hamani_loganalyzer_Makefile test` checks the sample counts, empty files, and a last line with no
newline. It also checks uppercase and lowercase, whole words, lines with
more than one message level, blank lines, Windows line endings, long lines,
file names with spaces, help, and bad input.

There is also a test that calls the command several times in one process,
like the group shell would. It checks that the shell gets its signal
handlers and `opterr` setting back. These tests all passed on macOS.

SIGINT and SIGTERM were also checked with a 512 MiB sparse file. Both gave
`analysis cancelled` and did not print unfinished counts. The return codes
are 0 for success, 1 for file or system errors, 2 for bad arguments,
130 for SIGINT, and 143 for SIGTERM. Run `make -f hamani_loganalyzer_Makefile test` again on the group's
Linux machine before the demo.

## What to give the group

- `hamani_loganalyzer.c` and `hamani_loganalyzer.h`: the command code.
- `hamani_loganalyzer_Makefile`, `hamani_loganalyzer_sample.log`, `hamani_loganalyzer_test.sh`, and `tests/hamani_loganalyzer_repeat_calls.c`:
  the build file, sample, and tests.
- `man/hamani_loganalyzer.1`: the man page.
- `hamani_loganalyzer_README.md`: these notes.

All of these files are also in `hamani_loganalyzer_command5.zip`.

To add this to the group shell, include `hamani_loganalyzer.h` and compile
`hamani_loganalyzer.c` with the other shell files. Leave out
`-DLOGANALYZER_STANDALONE` for that build. The shell calls it like this:

```c
int status = loganalyzer(command_argc, command_argv);
```

For `loganalyzer -f hamani_loganalyzer_sample.log`, the argument count is 3. The argument
array contains `"loganalyzer"`, `"-f"`, `"hamani_loganalyzer_sample.log"`, and a final `NULL`.
The function returns a status, so it does not exit the shell.
The build file uses `LOGANALYZER_STANDALONE` to build the separate program
for testing.
