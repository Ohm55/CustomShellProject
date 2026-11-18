#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum compare_status {
    FILES_MATCH = 0,
    FILES_DIFFER = 1,
    COMPARE_ERROR = 2,
    COMPARE_INTERRUPTED = 130
};

struct input_file {
    const char *path;
    FILE *stream;
    off_t size;
};

static volatile sig_atomic_t interrupted;

static void handle_interrupt(int signal_number)
{
    (void)signal_number;
    interrupted = 1;
}

static void print_usage(FILE *output)
{
    fputs("Usage: filediffadvanced [-b] LEFT RIGHT\n"
          "       filediffadvanced -h\n"
          "  -b  Compare bytes instead of lines\n"
          "  -h  Show this help\n", output);
}

static enum compare_status file_error(const struct input_file *file)
{
    if (!interrupted) {
        fprintf(stderr, "filediffadvanced: %s: %s\n", file->path, strerror(errno));
    }
    return COMPARE_ERROR;
}

static bool open_input(struct input_file *file)
{
    /* Avoid blocking on a FIFO before we can check the file type. */
    int fd = open(file->path, O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        file_error(file);
        return false;
    }

    struct stat info;
    if (fstat(fd, &info) == -1) {
        file_error(file);
    } else if (!S_ISREG(info.st_mode)) {
        fprintf(stderr, "filediffadvanced: %s: expected a regular file\n", file->path);
    } else {
        file->stream = fdopen(fd, "rb");
        if (file->stream != NULL) {
            file->size = info.st_size;
            return true;
        }
        file_error(file);
    }

    close(fd);
    return false;
}

static enum compare_status compare_lines(const struct input_file *left,
                                         const struct input_file *right,
                                         uintmax_t *difference)
{
    char *left_line = NULL;
    char *right_line = NULL;
    size_t left_capacity = 0;
    size_t right_capacity = 0;
    uintmax_t line_number = 1;
    enum compare_status status = FILES_MATCH;

    while (!interrupted) {
        ssize_t left_length = getline(&left_line, &left_capacity, left->stream);
        if (ferror(left->stream) || (left_length < 0 && !feof(left->stream))) {
            status = file_error(left);
            break;
        }

        ssize_t right_length = getline(&right_line, &right_capacity, right->stream);
        if (ferror(right->stream) || (right_length < 0 && !feof(right->stream))) {
            status = file_error(right);
            break;
        }

        if (left_length < 0 && right_length < 0) {
            break;
        }
        if (left_length != right_length ||
            memcmp(left_line, right_line, (size_t)left_length) != 0) {
            *difference = line_number;
            status = FILES_DIFFER;
            break;
        }
        ++line_number;
    }

    free(left_line);
    free(right_line);
    return status;
}

static enum compare_status compare_bytes(const struct input_file *left,
                                         const struct input_file *right,
                                         uintmax_t *difference)
{
    unsigned char left_block[4096];
    unsigned char right_block[4096];
    uintmax_t bytes_before_block = 0;

    while (!interrupted) {
        size_t left_length = fread(left_block, 1, sizeof left_block, left->stream);
        if (ferror(left->stream)) {
            return file_error(left);
        }

        size_t right_length = fread(right_block, 1, sizeof right_block, right->stream);
        if (ferror(right->stream)) {
            return file_error(right);
        }

        size_t common = left_length < right_length ? left_length : right_length;
        for (size_t index = 0; index < common; ++index) {
            if (left_block[index] != right_block[index]) {
                *difference = bytes_before_block + index + 1;
                return FILES_DIFFER;
            }
        }
        if (left_length != right_length) {
            *difference = bytes_before_block + common + 1;
            return FILES_DIFFER;
        }
        if (common < sizeof left_block) {
            break;
        }
        bytes_before_block += common;
    }

    return FILES_MATCH;
}

static double milliseconds_between(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec) * 1000.0 +
           (double)(end.tv_nsec - start.tv_nsec) / 1000000.0;
}

int filediffadvanced(int argc, char *argv[])
{
    bool binary_mode = false;
    bool help = false;
    bool invalid_option = false;
    int saved_opterr = opterr;

    /* The shell can call this command repeatedly in the same process. */
#ifdef __APPLE__
    optreset = 1;
    optind = 1;
#else
    optind = 0;
#endif
    opterr = 0;

    int option;
    while ((option = getopt(argc, argv, "bh")) != -1) {
        switch (option) {
        case 'b':
            binary_mode = true;
            break;
        case 'h':
            help = true;
            break;
        default:
            invalid_option = true;
            break;
        }
    }
    opterr = saved_opterr;

    if (invalid_option || (!help && argc - optind != 2)) {
        print_usage(stderr);
        return COMPARE_ERROR;
    }
    if (help) {
        print_usage(stdout);
        return FILES_MATCH;
    }

    struct sigaction handler = { .sa_handler = handle_interrupt };
    struct sigaction previous_handler;
    sigemptyset(&handler.sa_mask);
    interrupted = 0;
    if (sigaction(SIGINT, &handler, &previous_handler) == -1) {
        perror("filediffadvanced: sigaction");
        return COMPARE_ERROR;
    }

    struct input_file left = { .path = argv[optind] };
    struct input_file right = { .path = argv[optind + 1] };
    enum compare_status status = COMPARE_ERROR;
    uintmax_t difference = 0;
    struct timespec elapsed_start, elapsed_end, cpu_start, cpu_end;

    if (!open_input(&left) || interrupted || !open_input(&right) || interrupted) {
        goto cleanup;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &elapsed_start) == -1 ||
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_start) == -1) {
        perror("filediffadvanced: clock_gettime");
        goto cleanup;
    }

    if (binary_mode) {
        status = compare_bytes(&left, &right, &difference);
    } else {
        status = compare_lines(&left, &right, &difference);
    }

    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_end) == -1 ||
        clock_gettime(CLOCK_MONOTONIC, &elapsed_end) == -1) {
        perror("filediffadvanced: clock_gettime");
        status = COMPARE_ERROR;
    }

cleanup:
    if (left.stream != NULL && fclose(left.stream) == EOF) {
        status = file_error(&left);
    }
    if (right.stream != NULL && fclose(right.stream) == EOF) {
        status = file_error(&right);
    }
    if (sigaction(SIGINT, &previous_handler, NULL) == -1) {
        perror("filediffadvanced: sigaction");
        status = COMPARE_ERROR;
    }
    if (interrupted) {
        fputs("filediffadvanced: interrupted\n", stderr);
        return COMPARE_INTERRUPTED;
    }
    if (status == COMPARE_ERROR) {
        return status;
    }

    if (status == FILES_MATCH) {
        puts("Files match");
    } else {
        printf("First difference: %s %ju\n", binary_mode ? "byte" : "line", difference);
    }
    printf("File sizes: left=%jd bytes, right=%jd bytes\n",
           (intmax_t)left.size, (intmax_t)right.size);
    printf("Comparison time: %.3f ms elapsed, %.3f ms CPU\n",
           milliseconds_between(elapsed_start, elapsed_end),
           milliseconds_between(cpu_start, cpu_end));
    return status;
}
