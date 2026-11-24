#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Forward declarations for built-in commands. */
int filediffadvanced(int argc, char *argv[]);
int filecrypt_main(int argc, char *argv[]);
int loganalyzer(int argc, char *argv[]);
int netstatplus(int argc, char *argv[]);
int processgroup_main(int argc, char *argv[]);

/* ------------------------------------------------------------------ */
/*  Built-in command registry                                         */
/* ------------------------------------------------------------------ */

typedef int (*cmd_fn)(int, char *[]);

typedef struct {
  const char *name;
  cmd_fn fn;
} builtin_t;

static builtin_t builtins[] = {
    {"filecrypt", filecrypt_main},       {"filediffadvanced", filediffadvanced},
    {"loganalyzer", loganalyzer},        {"netstatplus", netstatplus},
    {"processgroup", processgroup_main}, {NULL, NULL}, /* Sentinel */
};

/* ------------------------------------------------------------------ */
/*  Signal handling for the shell itself                              */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t shell_interrupted = 0;

static void handle_shell_signal(int sig) {
  (void)sig;
  shell_interrupted = 1;
}

/* ------------------------------------------------------------------ */
/*  Look up a command in the built-in table and execute it            */
/*  Returns:  the command's exit status, or -1 if not found.          */
/* ------------------------------------------------------------------ */

static int dispatch_command(const char *cmd, int argc, char *argv[]) {
  for (int i = 0; builtins[i].name != NULL; i++) {
    if (strcmp(cmd, builtins[i].name) == 0) {
      return builtins[i].fn(argc, argv);
    }
  }
  return -1; /* Not a built-in command. */
}

/* ------------------------------------------------------------------ */
/*  Main: the shell prompt loop                                       */
/* ------------------------------------------------------------------ */

int main(void) {
  /* Install shell-level signal handler for clean exit. */
  struct sigaction sa = {0};
  sa.sa_handler = handle_shell_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);

  printf("Welcome to Custom Shell v1.0\n");
  printf("Type 'exit' or 'quit' to leave.\n");
  printf("Type 'help' for available commands.\n\n");

  while (!shell_interrupted) {
    /* Prompt and read input. */
    printf("custom-shell> ");
    fflush(stdout);

    char line[4096];
    if (!fgets(line, (int)sizeof(line), stdin))
      break;

    /* Strip trailing newline / carriage return. */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
      --len;
    }

    /* Skip blank / whitespace-only lines. */
    int has_content = 0;
    for (size_t i = 0; i < len; i++) {
      if (!isspace((unsigned char)line[i])) {
        has_content = 1;
        break;
      }
    }
    if (!has_content)
      continue;

    /* Tokenize into argv. */
    int argc_cmd = 0;
    char *argv[64];
    char *p = line;
    while (*p && argc_cmd < (int)(sizeof(argv) / sizeof(argv[0])) - 1) {
      while (*p && isspace((unsigned char)*p))
        p++;
      if (!*p)
        break;
      argv[argc_cmd++] = p; /* pointer into line buffer */
      while (*p && !isspace((unsigned char)*p))
        p++;
      if (*p) {
        *p = '\0';
        p++;
      } /* terminate token */
    }
    argv[argc_cmd] = NULL;

    /* Handle "exit" / "quit". */
    if (argc_cmd >= 1 &&
        (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "quit") == 0)) {
      printf("Goodbye.\n");
      break;
    }

    /* Handle "help". */
    if (argc_cmd >= 1 && strcmp(argv[0], "help") == 0) {
      printf("Available built-in commands:\n");
      for (int i = 0; builtins[i].name != NULL; i++)
        printf("  %-15s Built-in command\n", builtins[i].name);
      printf("  exit / quit    Exit the shell\n");
      continue;
    }

    /* Dispatch to a built-in command. */
    if (argc_cmd == 0)
      continue; /* safety: shouldn't happen */

    int status = dispatch_command(argv[0], argc_cmd, argv);

    if (status == -1) {
      fprintf(stderr, "%s: command not found\n", argv[0]);
    } else {
      printf("  [exit status: %d]\n\n", status);
    }
  }

  return 0;
}
