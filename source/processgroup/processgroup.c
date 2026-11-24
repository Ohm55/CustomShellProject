#define _GNU_SOURCE
#include "processgroup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>

#define MAX_ARGS 64

struct job {
    char **argv;
    char  *buf;     /* owned line buffer (batch mode) */
    pid_t pid;
};

struct config {
    int    count;        /* -n */
    const char *file;    /* -f */
    pid_t  attach_pgid;  /* -p */
    int    sig;          /* -s (0 = none) */
    double after;        /* -a seconds (<0 = unset) */
    double interval;     /* -i seconds */
    int    monitor;      /* -m */
    int    samples;      /* -c (0 = until group is empty) */
};

/* State shared with the signal handler and supervisor thread */
static volatile sig_atomic_t g_interrupts = 0;
static volatile sig_atomic_t g_done = 0;
static volatile pid_t        g_pgid = 0;
static volatile int          g_signal_sent = 0;

// signal handling

static void on_signal(int sig)
{
    /* Forward the signal to the whole group (async-signal-safe calls only).
       A second interrupt escalates to SIGKILL. */
    g_interrupts++;
    if (g_pgid > 0)
        kill(-g_pgid, g_interrupts > 1 ? SIGKILL : sig);
}

/* The caller's handlers, saved so a host shell gets them back afterwards. */
static struct sigaction g_old_int, g_old_term;

static void install_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &g_old_int);
    sigaction(SIGTERM, &sa, &g_old_term);
}

/* Reset per-run state (the shell calls processgroup_main repeatedly in one
   process) and take over SIGINT/SIGTERM for the duration of the run. */
static void begin_run(void)
{
    g_interrupts = 0;
    g_signal_sent = 0;
    g_done = 0;
    g_pgid = 0;
    install_handlers();
}

static void restore_handlers(void)
{
    sigaction(SIGINT, &g_old_int, NULL);
    sigaction(SIGTERM, &g_old_term, NULL);
}

static const struct { const char *name; int num; } sigtab[] = {
    {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"KILL", SIGKILL},
    {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"TERM", SIGTERM},
    {"CONT", SIGCONT}, {"STOP", SIGSTOP}, {"TSTP", SIGTSTP},
};

static int parse_signal(const char *s)
{
    if (strncasecmp(s, "SIG", 3) == 0) s += 3;
    if (isdigit((unsigned char)*s)) {
        char *end;
        long v = strtol(s, &end, 10);
        if (*end == '\0' && v > 0 && v < NSIG) return (int)v;
        return -1;
    }
    for (size_t i = 0; i < sizeof sigtab / sizeof sigtab[0]; i++)
        if (strcasecmp(s, sigtab[i].name) == 0) return sigtab[i].num;
    return -1;
}

//proc monitoring

/* Print a table of every live process in pgid. Returns member count. */
static int snapshot(pid_t pgid, int print)
{
    DIR *d = opendir("/proc");
    if (!d) return 0;
    long hz = sysconf(_SC_CLK_TCK);
    long pagekb = sysconf(_SC_PAGESIZE) / 1024;
    int members = 0;
    struct dirent *e;

    while ((e = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char path[300], buf[1024];
        snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;                       /* process exited meanwhile */
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = '\0';

        char *lp = strrchr(buf, ')'), *fp = strchr(buf, '(');
        if (!lp || !fp) continue;
        char comm[64];
        size_t clen = (size_t)(lp - fp - 1);
        if (clen >= sizeof comm) clen = sizeof comm - 1;
        memcpy(comm, fp + 1, clen);
        comm[clen] = '\0';

        char state;
        int ppid, pgrp;
        unsigned long ut, st;
        long rss;
        if (sscanf(lp + 2,
                   "%c %d %d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu "
                   "%*d %*d %*d %*d %*d %*d %*u %*u %ld",
                   &state, &ppid, &pgrp, &ut, &st, &rss) != 6)
            continue;
        if (pgrp != pgid) continue;

        members++;
        if (print)
            printf("  %-7s %-7d %-5c %8.2f %9ld  %s\n", e->d_name, ppid, state,
                   (double)(ut + st) / hz, rss * pagekb, comm);
    }
    closedir(d);
    return members;
}

static void sleep_sec(double s)
{
    struct timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) };
    nanosleep(&ts, NULL);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void print_header(pid_t pgid, int members)
{
    printf("[monitor] PGID %d, %d process(es)\n", (int)pgid, members);
    printf("  %-7s %-7s %-5s %8s %9s  %s\n", "PID", "PPID", "STATE",
           "CPU(s)", "RSS(KB)", "COMMAND");
}

// Supervisor thread: periodic monitoring and the timed group signal. 
static void *supervisor(void *arg)
{
    const struct config *cfg = arg;
    double start = now_sec(), next_print = start;

    while (!g_done) {
        double t = now_sec();
        if (cfg->sig && cfg->after >= 0 && !g_signal_sent &&
            t - start >= cfg->after) {
            if (kill(-g_pgid, cfg->sig) == 0)
                printf("[signal] sent signal %d to process group %d\n",
                       cfg->sig, (int)g_pgid);
            else
                perror("kill");
            g_signal_sent = 1;
        }
        if (cfg->monitor && t >= next_print) {
            int m = snapshot(g_pgid, 0);
            print_header(g_pgid, m);
            snapshot(g_pgid, 1);
            next_print = t + cfg->interval;
        }
        sleep_sec(0.05);
    }
    return NULL;
}

// command parsing

static char **split_line(char *line)
{
    char **argv = calloc(MAX_ARGS + 1, sizeof(char *));
    int n = 0;
    char *save = NULL;
    if (!argv) return NULL;
    for (char *tok = strtok_r(line, " \t\r\n", &save); tok && n < MAX_ARGS;
         tok = strtok_r(NULL, " \t\r\n", &save))
        argv[n++] = tok;
    if (n == 0) { free(argv); return NULL; }
    return argv;
}

/* Read a batch file: one command per line, '#' starts a comment. */
static int load_batch(const char *path, struct job **jobs_out)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "processgroup: %s: %s\n", path, strerror(errno)); return -1; }
    struct job *jobs = NULL;
    int n = 0, cap = 0;
    char *line = NULL;
    size_t len = 0;

    while (getline(&line, &len, f) != -1) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *copy = strdup(line);
        char **argv = copy ? split_line(copy) : NULL;
        if (!argv) { free(copy); continue; }       /* blank line */
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            jobs = realloc(jobs, cap * sizeof *jobs);
            if (!jobs) { fclose(f); free(line); return -1; }
        }
        jobs[n].argv = argv;
        jobs[n].buf = copy;
        jobs[n].pid = 0;
        n++;
    }
    free(line);
    fclose(f);
    *jobs_out = jobs;
    return n;
}

// modes

static void describe_status(int st, char *out, size_t sz)
{
    if (WIFEXITED(st))
        snprintf(out, sz, "exited with status %d", WEXITSTATUS(st));
    else if (WIFSIGNALED(st))
        snprintf(out, sz, "killed by signal %d (%s)", WTERMSIG(st),
                 strsignal(WTERMSIG(st)));
    else
        snprintf(out, sz, "status 0x%x", st);
}

static int run_launch(struct config *cfg, struct job *jobs, int njobs)
{
    pid_t pgid = 0;
    int failures = 0;

    for (int i = 0; i < njobs; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); failures++; continue; }
        if (pid == 0) {
            setpgid(0, pgid);                   /* pgid==0: become leader */
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            execvp(jobs[i].argv[0], jobs[i].argv);
            fprintf(stderr, "processgroup: %s: %s\n", jobs[i].argv[0], strerror(errno));
            _exit(127);
        }
        if (pgid == 0) { pgid = pid; g_pgid = pgid; }
        if (setpgid(pid, pgid) < 0 && errno != EACCES && errno != ESRCH)
            perror("setpgid");                  /* EACCES: child already exec'd */
        jobs[i].pid = pid;
    }
    if (pgid == 0) return 1;

    printf("[launch] process group %d with %d process(es)\n", (int)pgid, njobs);

    pthread_t th;
    int have_thread = 0;
    g_done = 0;
    if ((cfg->monitor || (cfg->sig && cfg->after >= 0)) &&
        pthread_create(&th, NULL, supervisor, cfg) == 0)
        have_thread = 1;

    int remaining = 0;
    for (int i = 0; i < njobs; i++) if (jobs[i].pid > 0) remaining++;

    while (remaining > 0) {
        int st;
        struct rusage ru;
        pid_t p = wait4(-1, &st, 0, &ru);
        if (p < 0) {
            if (errno == EINTR) continue;
            break;                              /* ECHILD */
        }
        const char *name = "?";
        for (int i = 0; i < njobs; i++)
            if (jobs[i].pid == p) { name = jobs[i].argv[0]; break; }
        char desc[96];
        describe_status(st, desc, sizeof desc);
        if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) failures++;
        printf("[done] pid %d (%s) %s | user %.2fs sys %.2fs maxrss %ld KB\n",
               (int)p, name, desc,
               ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6,
               ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6, ru.ru_maxrss);
        remaining--;
    }

    g_done = 1;
    if (have_thread) pthread_join(th, NULL);
    g_pgid = 0;
    printf("[summary] group %d finished, %d abnormal exit(s)\n", (int)pgid, failures);
    return failures ? 1 : 0;
}

static int run_attach(struct config *cfg)
{
    pid_t pgid = cfg->attach_pgid;

    if (kill(-pgid, 0) < 0) {
        fprintf(stderr, "processgroup: process group %d: %s\n", (int)pgid, strerror(errno));
        return 1;
    }
    /* g_pgid stays 0 here, so Ctrl-C only stops monitoring. */

    if (cfg->sig) {
        if (cfg->after > 0) sleep_sec(cfg->after);
        if (kill(-pgid, cfg->sig) < 0) { perror("kill"); return 1; }
        printf("[signal] sent signal %d to process group %d\n", cfg->sig, (int)pgid);
    }
    if (cfg->monitor) {
        for (int i = 0; !g_interrupts && (cfg->samples == 0 || i < cfg->samples); i++) {
            int m = snapshot(pgid, 0);
            print_header(pgid, m);
            snapshot(pgid, 1);
            if (m == 0) { printf("[monitor] group is empty\n"); break; }
            sleep_sec(cfg->interval);
        }
    }
    return 0;
}

static void usage(FILE *out)
{
    fprintf(out,
"Usage: processgroup [options] [-n COUNT] -- COMMAND [ARGS...]\n"
"       processgroup [options] -f BATCHFILE\n"
"       processgroup [options] -p PGID\n"
"Options:\n"
"  -n COUNT   run COUNT copies of COMMAND in one group (default 1)\n"
"  -f FILE    run every command in FILE (one per line) in one group\n"
"  -p PGID    attach to an existing process group\n"
"  -s SIG     signal to send to the whole group (name or number)\n"
"  -a SECS    delay before sending -s (default: immediately when attached,\n"
"             never when launching unless -a is given)\n"
"  -m         monitor CPU time and resident memory of the group\n"
"  -i SECS    monitor interval (default 1)\n"
"  -c COUNT   number of monitor samples in attach mode (default: until empty)\n"
"  -h         show this help\n");
}

int processgroup_main(int argc, char *argv[])
{
    struct config cfg = { 1, NULL, 0, 0, -1.0, 1.0, 0, 0 };
    int opt;

    optind = 1;                                 /* safe to call repeatedly */
    opterr = 1;
    while ((opt = getopt(argc, argv, "+n:f:p:s:a:i:c:mh")) != -1) {
        char *end;
        switch (opt) {
        case 'n': cfg.count = (int)strtol(optarg, &end, 10);
                  if (*end || cfg.count < 1 || cfg.count > 1000) {
                      fprintf(stderr, "processgroup: invalid count '%s'\n", optarg); return 1; }
                  break;
        case 'f': cfg.file = optarg; break;
        case 'p': cfg.attach_pgid = (pid_t)strtol(optarg, &end, 10);
                  if (*end || cfg.attach_pgid <= 1) {
                      fprintf(stderr, "processgroup: invalid PGID '%s'\n", optarg); return 1; }
                  break;
        case 's': cfg.sig = parse_signal(optarg);
                  if (cfg.sig < 0) {
                      fprintf(stderr, "processgroup: unknown signal '%s'\n", optarg); return 1; }
                  break;
        case 'a': cfg.after = strtod(optarg, &end);
                  if (*end || cfg.after < 0) {
                      fprintf(stderr, "processgroup: invalid delay '%s'\n", optarg); return 1; }
                  break;
        case 'i': cfg.interval = strtod(optarg, &end);
                  if (*end || cfg.interval <= 0) {
                      fprintf(stderr, "processgroup: invalid interval '%s'\n", optarg); return 1; }
                  break;
        case 'c': cfg.samples = (int)strtol(optarg, &end, 10);
                  if (*end || cfg.samples < 0) {
                      fprintf(stderr, "processgroup: invalid sample count '%s'\n", optarg); return 1; }
                  break;
        case 'm': cfg.monitor = 1; break;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 1;
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (cfg.attach_pgid) {
        if (cfg.file || optind < argc) {
            fprintf(stderr, "processgroup: -p cannot be combined with a command or -f\n");
            return 1;
        }
        if (!cfg.sig && !cfg.monitor) {
            fprintf(stderr, "processgroup: -p needs -s and/or -m\n");
            return 1;
        }
        begin_run();
        int rc = run_attach(&cfg);
        restore_handlers();
        return rc;
    }

    struct job *jobs = NULL;
    int njobs = 0, rc;

    if (cfg.file) {
        if (optind < argc) {
            fprintf(stderr, "processgroup: -f cannot be combined with a command\n");
            return 1;
        }
        njobs = load_batch(cfg.file, &jobs);
        if (njobs < 0) return 1;
        if (njobs == 0) { fprintf(stderr, "processgroup: %s has no commands\n", cfg.file); return 1; }
    } else {
        if (optind >= argc) { usage(stderr); return 1; }
        njobs = cfg.count;
        jobs = calloc(njobs, sizeof *jobs);
        if (!jobs) { perror("calloc"); return 1; }
        for (int i = 0; i < njobs; i++) jobs[i].argv = &argv[optind];
    }

    begin_run();
    rc = run_launch(&cfg, jobs, njobs);
    restore_handlers();

    if (cfg.file)
        for (int i = 0; i < njobs; i++) { free(jobs[i].buf); free(jobs[i].argv); }
    free(jobs);
    return rc;
}

#ifndef PG_NO_MAIN
int main(int argc, char *argv[])
{
    return processgroup_main(argc, argv);
}
#endif
