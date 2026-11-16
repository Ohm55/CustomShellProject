# My loganalyzer presentation guide

This guide follows the current files in `loganalyzer_project`. The main C
file has 204 lines. Line references below refer to that version.

## What to study first

Spend your time in this order:

1. Explain the whole command in about 30 seconds.
2. Understand the `mmap()` call at line 134 and its six arguments.
3. Understand the counting loop at line 162 and the message rules at line 53.
4. Explain how the signal handler asks the scanner to stop and clean up.
5. Explain the options, error codes, and group-shell entry point.
6. Rehearse the demo and the questions at the end.

You do not need to memorize every include or every line of error handling.
Be able to point to the important blocks and explain their jobs.

## 1. A short explanation you can say

> My part is command 5, the log analyzer. It reads a saved log file and
> counts errors, warnings, info messages, and other lines. It uses mmap
> so the program can scan the file through memory. I use getopt for the
> options and handle Ctrl+C so the command can stop and clean up.
> It also has tests and a man page.

The standalone program is `hamani_loganalyzer`. The C function is still
called `loganalyzer()`, so the group shell can use the assignment's command
name, `loganalyzer`.

## 2. Follow the code from start to finish

Start at [loganalyzer(), line 69](/Users/abeihhamani/Desktop/loganalyzer_project/hamani_loganalyzer.c:69).
The helper functions above it are called later.

1. **Read the options, lines 73–109.** Find the file after `-f`, show help
   for `-h`, and reject bad options or extra arguments.
2. **Open and check the file, lines 111–129.** Get a file descriptor, then
   check that the input is a regular file with a supported size.
3. **Map the file, lines 131–142.** For a nonempty file, call `mmap()`.
   Save the returned address in `data`, then close the file descriptor.
4. **Set up signals, lines 144–158.** Save the previous SIGINT and SIGTERM
   handlers and install this command's handler.
5. **Count the lines, lines 160–175.** Scan for newline characters and pass
   each line to `classify_line()`. Also handle a final line with no newline.
6. **Clean up, lines 177–188.** Unmap the file and restore the previous
   signal handlers. If cancelled, return a cancellation status.
7. **Print the result, lines 190–196.** Print the file name and all counts
   only after a successful scan and cleanup.

The tiny `main()` at lines 200–204 just calls `loganalyzer()`. It is included
only when `LOGANALYZER_STANDALONE` is defined during compilation.

## 3. mmap(): the most important OS part

Your actual call at [line 134](/Users/abeihhamani/Desktop/loganalyzer_project/hamani_loganalyzer.c:134) is:

```c
void *mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
```

Explain the arguments in order:

- `NULL`: let the OS pick the address.
- `size`: map this many bytes, using the file size from `fstat()`.
- `PROT_READ`: allow reads, without allowing writes through the mapping.
- `MAP_PRIVATE`: changes through a writable private mapping would not be
  written back to the file. This mapping is read-only anyway.
- `fd`: the open file being mapped.
- `0`: start at byte zero of the file.

It returns an address on success and `MAP_FAILED` on failure. Closing `fd`
does not remove the mapping; `munmap()` removes it. Your code skips mapping
empty files because a zero-length mapping is invalid on Linux.
[Linux mmap manual](https://man7.org/linux/man-pages/man2/mmap.2.html).

**What this means in OS terms:** the program uses virtual addresses. The OS
and hardware connect those addresses to physical memory. File pages can be
brought into memory as needed, and file data can already be in the page
cache. Mapping a large file does not mean all of it is immediately loaded
into RAM. This connects directly to Chapter 10, slides 5–10, and Chapter 9's
address translation and paging concepts.
[Linux memory concepts](https://docs.kernel.org/admin-guide/mm/concepts.html).

**Why use it here?** The assignment specifically asks for `mmap()`. It also
lets this scanner read bytes directly without allocating and filling a
separate full-file buffer or a fixed-size line buffer.

Say “it is a suitable way to scan a large file.” Your project does not
benchmark `mmap()` against `fread()`, so you cannot claim it is always faster.
Disk I/O, page faults, caching, and CPU work still matter.

## 4. How the counting actually works

Look at [the scan loop, line 162](/Users/abeihhamani/Desktop/loganalyzer_project/hamani_loganalyzer.c:162):

```c
if (data[i] == '\n') {
    ++lines;
    classify_line(data + start, i - start,
                  &errors, &warnings, &infos, &others);
    start = i + 1;
}
```

- `i` is the current byte position.
- `start` is where the current line begins.
- `data + start` points to that line's first byte.
- `i - start` is its length, without the newline.
- `start = i + 1` moves to the next line.
- Passing `&errors`, for example, lets `classify_line()` update the actual
  counter. Inside that function, `++*errors` means increment that counter.

The check at line 171 counts leftover bytes as one final line. For example,
`INFO ready` counts as one line even without a newline. A file ending in
one newline does not get an extra imaginary line. An empty file has zero
lines; a file containing just `\n` has one blank line.

### The message rules

[classify_line(), line 53](/Users/abeihhamani/Desktop/loganalyzer_project/hamani_loganalyzer.c:53)
uses `if` / `else if`, so each line gets exactly one category:

1. ERROR, if the line contains the whole word `ERROR`.
2. Warning, if it contains `WARNING` or `WARN` and no ERROR.
3. Info, if it contains `INFO` and neither higher-priority level.
4. Other, if none match. Blank lines go here too.

Therefore, Errors + Warnings + Info + Other = Total lines on success.
These are counts of **lines**, not counts of every word occurrence.
`ERROR ERROR` gives one error line. `INFO ERROR` also gives one error line.

[contains_word(), line 33](/Users/abeihhamani/Desktop/loganalyzer_project/hamani_loganalyzer.c:33)
checks both word boundaries and compares letters with `toupper()`.
Letters, digits, and underscores count as word characters in this code.
`[ERROR]` matches; `ERRORS`, `preERROR`, and `ERROR_code` do not.
The `unsigned char` casts make the calls to the character functions valid
even when a plain `char` would have a negative value.

**Important limit:** it searches the whole line. It does not parse a
dedicated log-level field. `INFO no ERROR occurred` is an error line under
these rules. Present this as the chosen rule, rather than claiming support
for every log format.

### Why the lengths matter

Mapped file data is not guaranteed to end with a C string's `\0` byte.
Your scanner uses `size` and line lengths to stay within the file.
It calls `strlen(word)` only for its own keyword strings, never
`strlen(data)` on the mapped file.

The counts use `uint64_t` so they can hold large totals. `PRIu64` supplies
the matching `printf()` format for that type on the platform.

The algorithm is **O(N)** in the file size for these fixed keywords.
The outer loop scans the bytes once, and the keyword checks can revisit
each line several times. The extra counters and scanner variables use
constant space, but the virtual mapping itself grows with file size.

## 5. getopt(): explain the actual option string

At [line 85](/Users/abeihhamani/Desktop/loganalyzer_project/hamani_loganalyzer.c:85):

```c
getopt(argc, argv, ":f:h")
```

- `f:` means `-f` requires a value, the file path.
- `h` means `-h` takes no value.
- The first `:` makes a missing option value return `':'`, which your
  switch handles separately from an unknown option.
- `optarg` points to the value supplied for `-f`.
- `optind` tracks the next argument; the final check rejects leftover ones.
- `opterr = 0` lets your code print its own error messages. The old value is
  saved and restored for the shell.

The reset block supports running this function again in one process.
macOS/BSD uses `optreset`; glibc uses `optind = 0` for reinitialization.
[Linux getopt manual](https://man7.org/linux/man-pages/man3/getopt.3.html).

For the normal command, `argc` is 3 and the argument array is:

```c
{"loganalyzer", "-f", "hamani_loganalyzer_sample.log", NULL}
```

`argv[0]` is the command name and counts toward `argc`. The final `NULL`
does not count. A path with spaces needs quotes when typed at the prompt.

Small details: `-h` returns immediately, and repeating `-f` uses the last
value. The parser does not explicitly reject repeated `-f` options.

## 6. Signals and cleanup

The handler at [line 18](/Users/abeihhamani/Desktop/loganalyzer_project/hamani_loganalyzer.c:18)
does only this:

```c
interrupted = signal_number;
```

`interrupted` is a `volatile sig_atomic_t`: a suitable small flag for
communication between this signal handler and the interrupted code.
`volatile` makes the scanner read the value rather than treating it as
unchanging. It is not a general replacement for thread synchronization.

The scanner checks the flag in its byte loop and inside `contains_word()`.
That second check matters if a single line is extremely long.
The handler returns; the scanner then stops and follows its normal cleanup.
This is cooperative cancellation, not the handler doing all the work.

Why not print in the handler? Standard I/O functions such as `printf()`
are not safe to call from an asynchronous signal handler. This code keeps
the handler tiny and prints the message later from ordinary execution.
[Linux signal-safety manual](https://man7.org/linux/man-pages/man7/signal-safety.7.html).

`sigaction()` installs a handler and can save the previous action.
`sigemptyset()` starts with an empty additional signal mask. The code saves
the shell's old SIGINT and SIGTERM actions, then restores them after the
scan. This relates to Chapter 4, slide 51.
[Linux sigaction manual](https://man7.org/linux/man-pages/man2/sigaction.2.html).

For cancellation during scanning, the command removes its mapping,
restores the old handlers, reports cancellation on standard error, and
does not print partial counts. It returns `128 + signal_number`:
130 for SIGINT and 143 for SIGTERM on the Linux/macOS targets here.

The custom handlers are installed after opening and mapping and restored
before printing results. Explain this as handling cancellation **during
scanning**, rather than protecting every moment of the command's lifetime.
SIGKILL and SIGSTOP cannot be caught.
[Linux sigaction manual](https://man7.org/linux/man-pages/man2/sigaction.2.html).

## 7. The other calls and error paths

- `open()`: gives an integer file descriptor. `O_RDONLY` opens for reading.
  `O_NONBLOCK` prevents opening a FIFO from waiting for a writer, so the
  type check can reject it. It does not make regular-file disk I/O
  nonblocking or remove possible waits for data.
  [Linux open manual](https://man7.org/linux/man-pages/man2/open.2.html).
- `fstat()`: gets information about the already-open file, including
  `st_size`. `S_ISREG()` checks its type. The size checks avoid an invalid
  or unsupported conversion to `size_t`.
- `close()`: releases the descriptor; it is different from removing the
  memory mapping.
- `munmap()`: releases the mapping. This memory is not freed with `free()`.
- `perror()`: prints the operation label and the system's current error
  description, based on `errno`, to standard error.
- `getopt()`, `perror()`, and the character helpers are library functions;
  do not describe every function in the program as a system call.

Return codes: **0** success/help; **1** file or system error; **2** bad
arguments; **130** SIGINT cancellation; **143** SIGTERM cancellation.
Normal results go to standard output; errors go to standard error.

## 8. Connecting it to the Custom Shell

The header declares:

```c
int loganalyzer(int argc, char *argv[]);
```

The group includes `hamani_loganalyzer.h`, compiles
`hamani_loganalyzer.c` with its other source files, and calls the function
when the entered command name is `loganalyzer`. The call looks like:

```c
int status = loganalyzer(command_argc, command_argv);
```

The shell has to supply correctly split arguments and a final `NULL`.
Your function returns a status instead of calling `exit()`, so the shell
can keep running.

**Build difference:** your standalone Makefile defines
`LOGANALYZER_STANDALONE` to include the small `main()`. The group build
leaves that macro undefined because the shell already has its own `main()`.

The repeat-call test links the source without that macro and calls the
function repeatedly. It checks that a bad grouped option does not break
the next call and that the previous handlers and `opterr` are restored.
See [the actual test](/Users/abeihhamani/Desktop/loganalyzer_project/tests/hamani_loganalyzer_repeat_calls.c:14).

There is no shared shell `main.c` in this folder, so the full group-shell
integration has not been verified here. The function is intended for
sequential calls. It uses shared option-parser state and process-wide
signal actions, so do not call it simultaneously from multiple threads.

If another member also has a public function called `loganalyzer`, renamed
filenames alone will not prevent a duplicate-symbol linker error. The group
should link one implementation or agree on separate function names.

## 9. A live demo you can rehearse

Run from `loganalyzer_project`. Use the actual renamed build file:

```sh
make -f hamani_loganalyzer_Makefile
./hamani_loganalyzer -h
./hamani_loganalyzer -f hamani_loganalyzer_sample.log
```

Expected sample output:

```text
File: hamani_loganalyzer_sample.log
Total lines: 6
Errors: 2
Warnings: 1
Info: 2
Other: 1
```

Say: “The six lines add up: two errors, one warning, two info messages,
and one other line.”

Show an error and immediately check its status:

```sh
./hamani_loganalyzer -f hamani_loganalyzer_missing_demo.log
echo $?
```

For a path that does not exist, expect an open error and status **1**.
Running the program without `-f` returns **2**.

Run the automated checks:

```sh
make -f hamani_loganalyzer_Makefile test
```

Look for `All loganalyzer command tests passed.` and
`Repeated shell calls passed.` The shell script checks exact counts,
empty files, a missing final newline, mixed case and levels, boundaries,
Windows line endings, long lines, spaces in paths, invalid input, and an
unchanged sample checksum. The permission-denied check is conditional;
it may be skipped for a user who can still read the file, such as root.

Open the manual:

```sh
man ./man/hamani_loganalyzer.1
```

The `-f` after **make** selects the build file. The `-f` after
**hamani_loganalyzer** selects the input log. They belong to different
programs.

### Optional: show the matching rules

```sh
demo=$(mktemp /tmp/hamani_loganalyzer_demo.XXXXXX)
printf 'INFO ERROR\nERRORS\nwarn\ninfo' > "$demo"
./hamani_loganalyzer -f "$demo"
rm -f "$demo"
```

Expect **4 total, 1 error, 1 warning, 1 info, 1 other**. This shows priority,
whole words, case matching, and a final line without a newline.

### Optional: show Ctrl+C on Linux

The small sample finishes too quickly. Practice this on the demo machine:

```sh
big=$(mktemp /tmp/hamani_loganalyzer_big.XXXXXX)
truncate -s 512M "$big"
./hamani_loganalyzer -f "$big"
```

Press Ctrl+C while it is scanning. Then run:

```sh
echo $?
rm -f "$big"
```

Expect `analysis cancelled`, no count summary, and **130** if interrupted
while the custom handler is active. If it finishes first, retry with a
larger sparse file. This zero-filled file is for cancellation only; it is
not a realistic log or a disk-throughput benchmark. `truncate` here is a
Linux demo utility and is not installed by default on every Mac.

For SIGTERM, send `kill -TERM PID` from another terminal while the scan is
running; replace PID with the actual process ID. Expect cancellation and
status **143** when the command is handling the signal.

## 10. Questions worth practicing aloud

**Why mmap instead of reading one line at a time?**
The assignment asks for it, and it lets me scan file data through memory
without a separate fixed-size line buffer. It is not a promise that it is
faster than every buffered-read implementation.

**Does a 10 GB file require 10 GB of RAM immediately?**
No. It requires enough virtual address space for the mapping. The OS
manages which pages are resident; memory and mapping limits still apply.

**Why can you close the descriptor before reading the data?**
The mapping remains valid independently of the open descriptor. I later
release the mapping with `munmap()`.

**Why not use strlen on the mapped data?**
The file is a bounded byte range, not necessarily a null-terminated string.
I use its known size and each line's length.

**How do you stop without doing unsafe work in the handler?**
The handler only stores the signal number. The scanner sees it and returns
through cleanup. Printing happens afterward in normal code.

**Why save the old signal handlers?**
This function can run inside the group shell's process. Its temporary
handlers should not replace the shell's behavior after the command ends.

**What if one line contains INFO, WARN, and ERROR?**
It counts once as ERROR. My chosen priority is ERROR, warning, info, other.

**Is this multithreaded, and does it need locks?**
No. A single thread updates the counters. Adding worker threads would need
extra design for file sections, split lines, counters, and signals.
The current code does not implement a CPU scheduler, IPC, or deadlock
algorithm just because those topics appear in the course.

**Is every data[i] access a system call?**
No. It is a memory access. An access can need OS involvement to resolve
a page fault, but I do not call `read()` separately for each byte.

**What does a page fault mean here?**
It can be normal: the process accesses a page whose mapping or data needs
to be made available. It does not automatically mean the program crashed
or that a disk read was necessary.

**How do you know it works?**
The tests compare exact counts and expected error behavior, and a separate
C test checks repeated calls and restored signal handlers. Cancellation
was also checked separately. These checks do not prove all possible inputs
or the full group's shell integration.

**What would you improve next?**
Parse an explicit severity field for a known log format. For files that
cannot fit in one mapping, consider mapping chunks. Benchmark before
making speed claims or adding threads.

## 11. Important limits and Linux checks

1. **Rebuild on Linux.** The current executable was identified as a macOS
   ARM64 Mach-O binary. Copy the source and build files, not just that
   executable. On Linux use:

   ```sh
   make -f hamani_loganalyzer_Makefile clean
   make -f hamani_loganalyzer_Makefile test
   ```

2. **Use the renamed Makefile explicitly.** Plain `make` will not
   automatically find `hamani_loganalyzer_Makefile`.
3. **The source uses POSIX interfaces.** `_POSIX_C_SOURCE 200809L` at the top
   requests the needed declarations before the headers. It does not turn
   the code into a Linux binary by itself.
   [Linux feature-test macros](https://man7.org/linux/man-pages/man7/feature_test_macros.7.html).
4. **glibc and other Linux libraries can differ.** The glibc branch fully
   resets `getopt()`; the fallback only sets `optind = 1`. Repeated-call
   behavior on another libc, such as musl, has not been verified here.
   Run the repeat-call test on the actual target instead of assuming all
   Linux installations behave the same.
5. **Use a stable, regular file.** This is not a streaming pipe reader or a
   live `tail` replacement. It scans the size obtained before mapping.
   A private mapping is not a guaranteed snapshot of concurrent file edits.
   Truncating the file can make mapped accesses raise SIGBUS, which your
   code does not handle.
   [Linux mmap manual](https://man7.org/linux/man-pages/man2/mmap.2.html).
6. **Whole-file mapping has limits.** Even a supported size can fail to map
   because of address-space or resource limits. A 64-bit target is more
   suitable for large files; this implementation has no chunking fallback.
7. **Know what was tested.** The current `make ... test` passed on macOS.
   Separate 512 MiB sparse-file checks of SIGINT and SIGTERM also passed,
   with no partial counts and statuses 130 and 143. Those cancellation
   checks are not part of the Makefile's normal test target. No Linux
   execution or complete group-shell run has been verified in this workspace.

## Last practice before presenting

Close this guide and try to answer these five things:

- What does each `mmap()` argument do?
- How do `start`, `i`, and `i - start` identify a line?
- Why does the signal handler only set a flag?
- Why is `main()` behind a compilation macro?
- What are the six counts printed for the sample?

Then run the demo once without looking up the commands. If you can do
that and explain the answers in your own words, you have the core of the
presentation covered.
