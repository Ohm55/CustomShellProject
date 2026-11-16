#define _POSIX_C_SOURCE 200809L
#include "hamani_loganalyzer.h"

#include <ctype.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* The signal handler can safely update this value. */
static volatile sig_atomic_t interrupted = 0;

static void handle_signal(int signal_number) {
    interrupted = signal_number;
}

static void usage(FILE *stream) {
    fprintf(stream, "Usage: hamani_loganalyzer -f LOGFILE\n"
                    "  -f FILE   Log file to analyze\n"
                    "  -h        Show this help\n");
}

static int word_char(unsigned char c) {
    return isalnum(c) || c == '_';
}

/* Look for a whole word. ERROR and error both work. */
static int contains_word(const char *line, size_t length, const char *word) {
    size_t word_length = strlen(word);
    if (length < word_length) return 0;

    for (size_t i = 0; i <= length - word_length; ++i) {
        if (interrupted) return 0;
        if (i > 0 && word_char((unsigned char)line[i - 1])) continue;
        if (i + word_length < length &&
            word_char((unsigned char)line[i + word_length])) continue;

        size_t j = 0;
        while (j < word_length &&
               toupper((unsigned char)line[i + j]) == (unsigned char)word[j]) {
            ++j;
        }
        if (j == word_length) return 1;
    }
    return 0;
}

static void classify_line(const char *line, size_t length,
                          uint64_t *errors, uint64_t *warnings,
                          uint64_t *infos, uint64_t *others) {
    /* Count each line in just one group. */
    if (contains_word(line, length, "ERROR")) {
        ++*errors;
    } else if (contains_word(line, length, "WARNING") ||
               contains_word(line, length, "WARN")) {
        ++*warnings;
    } else if (contains_word(line, length, "INFO")) {
        ++*infos;
    } else {
        ++*others;
    }
}

int loganalyzer(int argc, char *argv[]) {
    const char *path = NULL;
    int option;

    /* Reset the options so the shell can run this command again. */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    extern int optreset; /* BSD needs this reset; the POSIX macro hides its declaration. */
    optreset = 1;
    optind = 1;
#elif defined(__GLIBC__)
    optind = 0;
#else
    optind = 1;
#endif
    int saved_opterr = opterr;
    opterr = 0;
    while ((option = getopt(argc, argv, ":f:h")) != -1) {
        switch (option) {
            case 'f': path = optarg; break;
            case 'h':
                opterr = saved_opterr;
                usage(stdout);
                return 0;
            case ':':
                opterr = saved_opterr;
                fprintf(stderr, "loganalyzer: option -%c requires a value\n", optopt);
                usage(stderr);
                return 2;
            default:
                opterr = saved_opterr;
                fprintf(stderr, "loganalyzer: unknown option -%c\n", optopt);
                usage(stderr);
                return 2;
        }
    }
    opterr = saved_opterr;
    if (path == NULL || optind != argc) {
        fprintf(stderr, "loganalyzer: specify one file using -f\n");
        usage(stderr);
        return 2;
    }

    /* Use nonblocking mode so a FIFO can be rejected without waiting for a writer. */
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        perror("loganalyzer: open");
        return 1;
    }

    struct stat info;
    if (fstat(fd, &info) == -1) {
        perror("loganalyzer: fstat");
        close(fd);
        return 1;
    }
    if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
        (uintmax_t)info.st_size > SIZE_MAX) {
        fprintf(stderr, "loganalyzer: input must be a regular file of supported size\n");
        close(fd);
        return 1;
    }

    size_t size = (size_t)info.st_size;
    const char *data = NULL;
    if (size > 0) {
        void *mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped == MAP_FAILED) {
            perror("loganalyzer: mmap");
            close(fd);
            return 1;
        }
        data = mapped;
    }
    close(fd); /* The mapping still works after the file descriptor is closed. */

    struct sigaction action = {0}, old_int, old_term;
    interrupted = 0;
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &old_int) == -1) {
        perror("loganalyzer: sigaction(SIGINT)");
        if (size > 0) munmap((void *)data, size);
        return 1;
    }
    if (sigaction(SIGTERM, &action, &old_term) == -1) {
        perror("loganalyzer: sigaction(SIGTERM)");
        sigaction(SIGINT, &old_int, NULL);
        if (size > 0) munmap((void *)data, size);
        return 1;
    }

    uint64_t lines = 0, errors = 0, warnings = 0, infos = 0, others = 0;
    size_t start = 0;
    for (size_t i = 0; i < size; ++i) {
        if (interrupted) break;
        if (data[i] == '\n') {
            ++lines;
            classify_line(data + start, i - start,
                          &errors, &warnings, &infos, &others);
            start = i + 1;
        }
    }
    if (!interrupted && start < size) {
        ++lines; /* Count the last line even if it has no newline. */
        classify_line(data + start, size - start,
                      &errors, &warnings, &infos, &others);
    }

    int status = 0;
    if (size > 0 && munmap((void *)data, size) == -1) {
        perror("loganalyzer: munmap");
        status = 1;
    }
    sigaction(SIGTERM, &old_term, NULL);
    sigaction(SIGINT, &old_int, NULL);
    if (interrupted) {
        fprintf(stderr, "loganalyzer: interrupted; analysis cancelled\n");
        return 128 + interrupted;
    }
    if (status != 0) return status;

    printf("File: %s\n", path);
    printf("Total lines: %" PRIu64 "\n", lines);
    printf("Errors: %" PRIu64 "\n", errors);
    printf("Warnings: %" PRIu64 "\n", warnings);
    printf("Info: %" PRIu64 "\n", infos);
    printf("Other: %" PRIu64 "\n", others);
    return 0;
}

/* Use -DLOGANALYZER_STANDALONE to run this on its own. */
#ifdef LOGANALYZER_STANDALONE
int main(int argc, char *argv[]) {
    return loganalyzer(argc, argv);
}
#endif
