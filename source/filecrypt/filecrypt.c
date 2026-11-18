#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>

#include <openssl/crypto.h>

#include "crypto.h"
#include "filecrypt.h"

#define PASSWORD_CAPACITY 1024

enum operation {
    OP_NONE,
    OP_ENCRYPT,
    OP_DECRYPT
};

static volatile sig_atomic_t g_cancelled = 0;

/* Defer cleanup to normal control flow; signal handlers must stay minimal. */
static void handle_signal(int signal_number)
{
    (void)signal_number;
    g_cancelled = 1;
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage:\n"
            "  %s -e -i INPUT -o OUTPUT [-k KEYFILE] [-f]\n"
            "  %s -d -i INPUT -o OUTPUT [-k KEYFILE] [-f]\n"
            "\n"
            "Options:\n"
            "  -e, --encrypt       Encrypt INPUT\n"
            "  -d, --decrypt       Decrypt INPUT\n"
            "  -i, --input FILE    Read from FILE\n"
            "  -o, --output FILE   Write to FILE\n"
            "  -k, --key-file FILE Read the password from FILE\n"
            "  -f, --force         Replace an existing output file\n"
            "  -h, --help          Show this help text\n"
            "\n"
            "If -k is omitted, filecrypt securely prompts for a password.\n",
            program, program);
}

struct saved_signal_handlers {
    struct sigaction old_int;
    struct sigaction old_term;
    struct sigaction old_hup;
    int installed;
};

static int install_signal_handlers(struct saved_signal_handlers *saved)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);

    saved->installed = 0;

    if (sigaction(SIGINT, &action, &saved->old_int) == -1) {
        perror("filecrypt: sigaction SIGINT");
        return -1;
    }

    if (sigaction(SIGTERM, &action, &saved->old_term) == -1) {
        perror("filecrypt: sigaction SIGTERM");
        (void)sigaction(SIGINT, &saved->old_int, NULL);
        return -1;
    }

    if (sigaction(SIGHUP, &action, &saved->old_hup) == -1) {
        perror("filecrypt: sigaction SIGHUP");
        (void)sigaction(SIGTERM, &saved->old_term, NULL);
        (void)sigaction(SIGINT, &saved->old_int, NULL);
        return -1;
    }

    saved->installed = 1;
    return 0;
}

static int restore_signal_handlers(struct saved_signal_handlers *saved)
{
    int result = 0;

    if (!saved->installed)
        return 0;

    if (sigaction(SIGHUP, &saved->old_hup, NULL) == -1) {
        perror("filecrypt: restore SIGHUP");
        result = -1;
    }

    if (sigaction(SIGTERM, &saved->old_term, NULL) == -1) {
        perror("filecrypt: restore SIGTERM");
        result = -1;
    }

    if (sigaction(SIGINT, &saved->old_int, NULL) == -1) {
        perror("filecrypt: restore SIGINT");
        result = -1;
    }

    saved->installed = 0;
    return result;
}

static int read_password_from_terminal(unsigned char *password, size_t capacity)
{
    struct termios original;
    struct termios hidden;

    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "filecrypt: standard input is not a terminal; use -k KEYFILE\n");
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &original) == -1) {
        perror("filecrypt: tcgetattr");
        return -1;
    }

    hidden = original;
    hidden.c_lflag &= (tcflag_t)~ECHO;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) == -1) {
        perror("filecrypt: tcsetattr");
        return -1;
    }
    /* Restore the original terminal settings on every read path. */
    fprintf(stderr, "Password: ");
    fflush(stderr);
    if (fgets((char *)password, (int)capacity, stdin) == NULL) {
        fprintf(stderr, "\nfilecrypt: could not read password\n");
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        return -1;
    }
    if (strchr((char *)password, '\n') == NULL && !feof(stdin)) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        fprintf(stderr, "\nfilecrypt: password is too long\n");
        return -1;
    }

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &original) == -1) {
        perror("filecrypt: tcsetattr");
        return -1;
    }
    fprintf(stderr, "\n");

    password[strcspn((char *)password, "\r\n")] = '\0';
    return password[0] == '\0' ? -1 : 0;
}

static int read_password_from_file(const char *path,
                                   unsigned char *password,
                                   size_t capacity)
{
    int fd;
    ssize_t count;
    size_t used = 0;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        fprintf(stderr, "filecrypt: cannot open key file '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    while (used + 1 < capacity) {
        count = read(fd, password + used, capacity - used - 1);
        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count == 0)
            break;
        if (errno == EINTR && !g_cancelled)
            continue;
        fprintf(stderr, "filecrypt: cannot read key file '%s': %s\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    password[used] = '\0';
    password[strcspn((char *)password, "\r\n")] = '\0';
    if (password[0] == '\0') {
        fprintf(stderr, "filecrypt: password cannot be empty\n");
        return -1;
    }
    if (used + 1 == capacity) {
        fprintf(stderr, "filecrypt: password is too long\n");
        return -1;
    }
    return 0;
}

int filecrypt_main(int argc, char **argv)
{
    enum operation operation = OP_NONE;
    const char *input_path = NULL;
    const char *output_path = NULL;
    const char *key_path = NULL;
    unsigned char password[PASSWORD_CAPACITY] = {0};
    int force = 0;
    int option;
    int status = EXIT_FAILURE;
    int password_locked = 0;
    struct saved_signal_handlers saved_handlers = {0};

    static const struct option options[] = {
        {"encrypt",  no_argument,       NULL, 'e'},
        {"decrypt",  no_argument,       NULL, 'd'},
        {"input",    required_argument, NULL, 'i'},
        {"output",   required_argument, NULL, 'o'},
        {"key-file", required_argument, NULL, 'k'},
        {"force",    no_argument,       NULL, 'f'},
        {"help",     no_argument,       NULL, 'h'},
        {NULL,        0,                 NULL, 0}
    };

    g_cancelled = 0;

    #if defined(__APPLE__) || defined(__FreeBSD__) || \
        defined(__NetBSD__) || defined(__OpenBSD__)
        extern int optreset;
        optreset = 1;
        optind = 1;
    #elif defined(__GLIBC__)
        optind = 0;
    #else
        optind = 1;
    #endif


    while ((option = getopt_long(argc, argv, "edi:o:k:fh", options, NULL)) != -1) {
        switch (option) {
        case 'e':
            if (operation != OP_NONE) {
                fprintf(stderr, "filecrypt: choose exactly one of -e or -d\n");
                return EXIT_FAILURE;
            }
            operation = OP_ENCRYPT;
            break;
        case 'd':
            if (operation != OP_NONE) {
                fprintf(stderr, "filecrypt: choose exactly one of -e or -d\n");
                return EXIT_FAILURE;
            }
            operation = OP_DECRYPT;
            break;
        case 'i': input_path = optarg; break;
        case 'o': output_path = optarg; break;
        case 'k': key_path = optarg; break;
        case 'f': force = 1; break;
        case 'h': usage(stdout, argv[0]); return EXIT_SUCCESS;
        default:  usage(stderr, argv[0]); return EXIT_FAILURE;
        }
    }

    if (optind != argc) {
        fprintf(stderr, "filecrypt: unexpected argument '%s'\n", argv[optind]);
        return EXIT_FAILURE;
    }
    if (operation == OP_NONE || input_path == NULL || output_path == NULL) {
        fprintf(stderr, "filecrypt: -e or -d, -i, and -o are required\n");
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }
    if (strcmp(input_path, output_path) == 0) {
        fprintf(stderr, "filecrypt: input and output must be different files\n");
        return EXIT_FAILURE;
    }
    if (install_signal_handlers(&saved_handlers) == -1)
        return EXIT_FAILURE;

    /* Lock the password buffer when permitted and cleanse it before exit. */
    if (mlock(password, sizeof(password)) == 0) {
        password_locked = 1;
    } else {
        fprintf(stderr, "filecrypt: warning: password memory could not be locked: %s\n",
                strerror(errno));
    }

    if (key_path != NULL) {
        if (read_password_from_file(key_path, password, sizeof(password)) == -1)
            goto cleanup;
    } else if (read_password_from_terminal(password, sizeof(password)) == -1) {
        if (!g_cancelled)
            fprintf(stderr, "filecrypt: password cannot be empty\n");
        goto cleanup;
    }

    if (g_cancelled) {
        fprintf(stderr, "filecrypt: interrupted\n");
        goto cleanup;
    }

    if (operation == OP_ENCRYPT) {
        status = encrypt_file(input_path, output_path, password,
                              force, &g_cancelled);
    } else {
        status = decrypt_file(input_path, output_path, password,
                              force, &g_cancelled);
    }

cleanup:
    OPENSSL_cleanse(password, sizeof(password));

    if (password_locked)
        (void)munlock(password, sizeof(password));

    if (restore_signal_handlers(&saved_handlers) == -1)
        status = EXIT_FAILURE;

    return status;
}

#ifndef FILECRYPT_NO_MAIN

int main(int argc, char *argv[])
{
    return filecrypt_main(argc, argv);
}

#endif
