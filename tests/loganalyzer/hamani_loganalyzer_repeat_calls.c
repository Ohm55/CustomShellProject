#define _POSIX_C_SOURCE 200809L
#include "hamani_loganalyzer.h"
#include <stddef.h>
#include <signal.h>
#include <unistd.h>

static volatile sig_atomic_t restored = 0;

static void shell_handler(int signal_number) {
    restored = signal_number;
}

/* Check several calls in one process, like the group shell. */
int main(int argc, char *argv[]) {
    if (argc != 2) return 1;
    char *bad[] = {"loganalyzer", "-zx", NULL};
    char *good[] = {"loganalyzer", "-f", argv[1], NULL};
    char *help[] = {"loganalyzer", "-h", NULL};
    char *missing[] = {"loganalyzer", "-f", NULL};

    if (signal(SIGINT, shell_handler) == SIG_ERR) return 1;
    if (signal(SIGTERM, shell_handler) == SIG_ERR) return 1;
    opterr = 1;
    /* A bad grouped option should not mess up the next call. */
    if (loganalyzer(2, bad) != 2) return 1;
    if (loganalyzer(3, good) != 0) return 1;
    if (opterr != 1) return 1;

    /* Check that the shell gets its original signal handlers back. */
    if (raise(SIGINT) != 0 || restored != SIGINT) return 1;
    if (raise(SIGTERM) != 0 || restored != SIGTERM) return 1;
    if (loganalyzer(2, missing) != 2) return 1;
    if (loganalyzer(2, help) != 0) return 1;
    if (loganalyzer(3, good) != 0) return 1;
    return 0;
}
