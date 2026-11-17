# Operating Systems Group Project

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
