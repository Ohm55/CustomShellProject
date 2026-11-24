#define _POSIX_C_SOURCE 200809L
#include "netstatplus.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Signal handling                                                   */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t interrupted = 0;

static void handle_signal(int sig) {
    (void)sig;
    interrupted = 1;
}

/* ------------------------------------------------------------------ */
/*  Constants & data structures                                       */
/* ------------------------------------------------------------------ */

#define MAX_CONN 512
#define LINE_BUF 1024

/* TCP states from the kernel /proc/net/tcp "st" column. */
typedef struct {
    uint8_t code;
    const char *name;
} state_map_t;

static const state_map_t state_table[] = {
    { 0x01, "ESTABLISHED" },
    { 0x02, "SYN_SENT"    },
    { 0x03, "SYN_RECV"    },
    { 0x04, "FIN_WAIT1"   },
    { 0x05, "FIN_WAIT2"   },
    { 0x06, "TIME_WAIT"   },
    { 0x07, "CLOSE"       },
    { 0x08, "CLOSING"     },
    { 0x0A, "LISTEN"      },
    { 0x0B, "CLOSE_WAIT"  },
    { 0x0C, "LAST_ACK"    },
};

/* A single parsed socket entry. */
typedef struct {
    char     proto[5];       /* "tcp " or "udp "     */
    uint8_t  local_ip[4];    /* dotted-decimal bytes */
    uint16_t local_port;
    uint8_t  remote_ip[4];
    uint16_t remote_port;
    uint8_t  state;          /* raw kernel state code */
    int64_t  bytes_recv;
    int64_t  bytes_sent;
} conn_entry_t;

/* Snapshot with deltas against a previous read. */
typedef struct {
    conn_entry_t entries[MAX_CONN];
    int count;
} snapshot_t;

/* Configuration parsed from the command line. */
typedef struct {
    int show_tcp;           /* 1 if -t given                          */
    int show_udp;           /* 1 if -u given                          */
    int listening_only;     /* 1 if -l                                */
    int numeric;            /* 1 if -n                                */
    double interval;        /* seconds between updates                */
    int one_shot;           /* 1 if -1                                */
} config_t;

/* ------------------------------------------------------------------ */
/*  Utility: parse a hex pair into a uint8_t                          */
/* ------------------------------------------------------------------ */

static uint8_t hex_pair_to_byte(const char *hex) {
    unsigned int val;
    sscanf(hex, "%02x", &val);
    return (uint8_t)val;
}

/* ------------------------------------------------------------------ */
/*  Parse one line from /proc/net/<proto> into a conn_entry_t         */
/*  Returns:  0 on success, -1 on failure / skip.                    */
/* ------------------------------------------------------------------ */

static int parse_proc_line(const char *line, const char *proto,
                           conn_entry_t *entry) {
    /* /proc/net/tcp format (space-separated):
       sl local_address rem_address   st tx_queue rx-timer ...
       We need:  local_address (IP:PORT), rem_address (IP:PORT), st.
    */

    char *dup = strdup(line);
    if (!dup) return -1;

    /* Skip the "sl: ..." header line. */
    if (strncmp(dup, "  sl", 4) == 0) { free(dup); return -1; }

    char *fields[8];
    int nf = 0;
    char *saveptr = NULL;

    for (char *tok = strtok_r(dup, " \t\n", &saveptr);
         tok && nf < 8;
         tok = strtok_r(NULL, " \t\n", &saveptr)) {
        fields[nf++] = tok;
    }

    free(dup);

    /* We need at least: local_addr rem_addr st */
    if (nf < 3) return -1;

    /* --- Local address (IP:PORT) --- */
    char *colon = strchr(fields[0], ':');
    if (!colon || colon == fields[0]) return -1;

    int n = (int)(colon - fields[0]);
    if (n != 8) return -1;           /* IPv4 always 8 hex chars */

    const char *ip_hex = fields[0];
    entry->local_ip[0] = hex_pair_to_byte(ip_hex + 6);
    entry->local_ip[1] = hex_pair_to_byte(ip_hex + 4);
    entry->local_ip[2] = hex_pair_to_byte(ip_hex + 2);
    entry->local_ip[3] = hex_pair_to_byte(ip_hex + 0);

    char port_buf[6];
    memcpy(port_buf, colon + 1, 4);
    port_buf[4] = '\0';
    sscanf(port_buf, "%04hx", &entry->local_port);

    /* --- Remote address (IP:PORT) --- */
    colon = strchr(fields[1], ':');
    if (!colon || colon == fields[1]) return -1;

    n = (int)(colon - fields[1]);
    ip_hex = fields[1];
    entry->remote_ip[0] = hex_pair_to_byte(ip_hex + 6);
    entry->remote_ip[1] = hex_pair_to_byte(ip_hex + 4);
    entry->remote_ip[2] = hex_pair_to_byte(ip_hex + 2);
    entry->remote_ip[3] = hex_pair_to_byte(ip_hex + 0);

    memset(port_buf, 0, sizeof port_buf);
    memcpy(port_buf, colon + 1, 4);
    sscanf(port_buf, "%04hx", &entry->remote_port);

    /* --- State code --- */
    entry->state = (uint8_t)strtol(fields[2], NULL, 16);

    /* --- Bytes received / sent ---
       In modern kernels the stats blob is separate fields.
       We parse fields[7] onward as hex words (each word = 4 hex chars = 2 bytes).
       Word index 3 -> RX bytes, index 17 -> TX bytes.
    */
    entry->bytes_recv = 0;
    entry->bytes_sent = 0;

    if (nf >= 8) {
        char combined[LINE_BUF];
        memset(combined, 0, sizeof combined);
        for (int i = 7; i < nf && strlen(combined) < (int)sizeof(combined) - 8; i++) {
            if (i > 7) strncat(combined, " ", (size_t)(sizeof(combined) - strlen(combined) - 1));
            strncat(combined, fields[i], (size_t)(sizeof(combined) - strlen(combined) - 1));
        }

        int len = (int)strlen(combined);
        if (len >= 64) {
            unsigned int val3 = 0, val17 = 0;
            sscanf(combined + 24, "%08x", &val3);   /* index 3 * 8 = 24 */
            sscanf(combined + 136, "%08x", &val17);  /* index 17 * 8 = 136 */
            entry->bytes_recv = (int64_t)val3;
            entry->bytes_sent = (int64_t)val17;
        }
    }

    /* --- Protocol name --- */
    strncpy(entry->proto, proto, sizeof(entry->proto) - 1);
    entry->proto[4] = '\0';

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Helper: map a state code to a human-readable string               */
/* ------------------------------------------------------------------ */

static const char *state_name(uint8_t code) {
    for (size_t i = 0; i < sizeof(state_table) / sizeof(state_table[0]); i++) {
        if (state_table[i].code == code) return state_table[i].name;
    }
    return "UNKNOWN";
}

/* ------------------------------------------------------------------ */
/*  Helper: format an IP as a dotted-decimal string                   */
/* ------------------------------------------------------------------ */

static void ip_to_string(const uint8_t *ip, char *buf, size_t sz) {
    snprintf(buf, sz, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

/* ------------------------------------------------------------------ */
/*  Helper: format a port as string                                   */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Format a throughput value for display                             */
/* ------------------------------------------------------------------ */

static void throughput_str(int64_t bytes, char *buf, size_t sz) {
    if (bytes <= 0) {
        snprintf(buf, sz, "0 B");
        return;
    }
    double b = (double)bytes;
    if (b >= 1e9)
        snprintf(buf, sz, "%.1f GB", b / 1e9);
    else if (b >= 1e6)
        snprintf(buf, sz, "%.1f MB", b / 1e6);
    else if (b >= 1e3)
        snprintf(buf, sz, "%.1f KB", b / 1e3);
    else
        snprintf(buf, sz, "%lld B", (long long)b);
}

/* ------------------------------------------------------------------ */
/*  Print a single connection row with throughput delta               */
/* ------------------------------------------------------------------ */

static void print_entry(const config_t *cfg, const conn_entry_t *e,
                        int64_t prev_recv, int64_t prev_sent) {
    char lip[32], rip[32];
    ip_to_string(e->local_ip, lip, sizeof(lip));
    ip_to_string(e->remote_ip, rip, sizeof(rip));

    /* Calculate throughput delta. */
    int64_t delta_recv = 0, delta_sent = 0;
    if (prev_recv >= 0) {
        delta_recv = e->bytes_recv - prev_recv;
        if (delta_recv < 0) delta_recv = 0;
    }
    if (prev_sent >= 0) {
        delta_sent = e->bytes_sent - prev_sent;
        if (delta_sent < 0) delta_sent = 0;
    }

    char rx_str[32], tx_str[32];
    double rx_rate = cfg->interval > 0 ? (double)delta_recv / cfg->interval : 0;
    double tx_rate = cfg->interval > 0 ? (double)delta_sent / cfg->interval : 0;
    throughput_str((int64_t)(rx_rate < 0 ? 0 : rx_rate), rx_str, sizeof(rx_str));
    throughput_str((int64_t)(tx_rate < 0 ? 0 : tx_rate), tx_str, sizeof(tx_str));

    printf("  %-5s %-7s %-7s %-21s %-21s %-13s %8s %8s\n",
           e->proto,
           "0",                /* Recv-Q */
           "0",                /* Send-Q */
           lip,
           rip,
           state_name(e->state),
           rx_str,
           tx_str);
}

/* ------------------------------------------------------------------ */
/*  Print the column header                                         */
/* ------------------------------------------------------------------ */

static void print_header(void) {
    printf("  %-5s %-7s %-7s %-21s %-21s %-13s %8s %8s\n",
           "Proto", "Recv-Q", "Send-Q", "Local Address",
           "Foreign Address", "State", "RX", "TX");
}

/* ------------------------------------------------------------------ */
/*  Take a snapshot: read all connections from /proc/net/<proto>      */
/*  into the snapshot array. Returns entry count or -1 on error.     */
/* ------------------------------------------------------------------ */

static int take_snapshot(const config_t *cfg, snapshot_t *snap) {
    FILE *fp = NULL;
    static const char *tcp_paths[] = { "/proc/net/tcp", "/proc/net/tcp6", NULL };
    static const char *udp_paths[] = { "/proc/net/udp", "/proc/net/udp6", NULL };

    /* TCP connections. */
    if (cfg->show_tcp || (!cfg->show_udp && !cfg->show_tcp)) {
        for (int i = 0; tcp_paths[i] != NULL; i++) {
            fp = fopen(tcp_paths[i], "r");
            if (fp) break;
        }
        if (fp) {
            char line[LINE_BUF];
            int count = 0;
            while (fgets(line, (int)sizeof(line), fp) &&
                   count < MAX_CONN && !interrupted) {
                conn_entry_t *e = &snap->entries[count];
                memset(e, 0, sizeof(*e));
                if (parse_proc_line(line, "tcp", e) == 0) {
                    if (cfg->listening_only &&
                        strcmp(state_name(e->state), "LISTEN") != 0)
                        continue;
                    count++;
                }
            }
            fclose(fp);
        }
    }

    /* UDP connections. */
    if (cfg->show_udp || (!cfg->show_tcp && !cfg->show_udp)) {
        for (int i = 0; udp_paths[i] != NULL; i++) {
            fp = fopen(udp_paths[i], "r");
            if (fp) break;
        }
        if (fp) {
            char line[LINE_BUF];
            while (fgets(line, (int)sizeof(line), fp) &&
                   snap->count < MAX_CONN && !interrupted) {
                conn_entry_t *e = &snap->entries[snap->count];
                memset(e, 0, sizeof(*e));
                if (parse_proc_line(line, "udp", e) == 0) {
                    if (cfg->listening_only &&
                        strcmp(state_name(e->state), "LISTEN") != 0)
                        continue;
                    snap->count++;
                }
            }
            fclose(fp);
        }
    }

    return snap->count;
}

/* ------------------------------------------------------------------ */
/*  Main entry point: netstatplus()                                   */
/*  Parses options, runs snapshot + display loop.                    */
/* ------------------------------------------------------------------ */

int netstatplus(int argc, char *argv[]) {
    config_t cfg = {0};

    /* Defaults: show both TCP and UDP by default. */
    int had_tcp   = 0;
    int had_udp   = 0;

    /* Reset getopt state for repeated calls (BSD-compatible). */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    extern int optreset;
    optreset = 1;
#else
    optind = 0;
#endif
    int saved_opterr = opterr;
    opterr = 0;

    int opt;
    while ((opt = getopt(argc, argv, ":tulni:1h")) != -1) {
        switch (opt) {
        case 't': cfg.show_tcp = 1; had_tcp = 1; break;
        case 'u': cfg.show_udp = 1; had_udp = 1; break;
        case 'l': cfg.listening_only = 1;             break;
        case 'n': cfg.numeric = 1;                    break;
        case 'i': {
            char *end;
            cfg.interval = strtod(optarg, &end);
            if (*end || cfg.interval <= 0) {
                opterr = saved_opterr;
                fprintf(stderr, "netstatplus: invalid interval '%s'\n", optarg);
                fprintf(stderr, "Usage: netstatplus [-t] [-u] [-l] [-n] [-i SECS] [-1] [-h]\n");
                return 2;
            }
            break;
        }
        case '1': cfg.one_shot = 1;                 break;
        case 'h': {
            opterr = saved_opterr;
            printf("Usage: netstatplus [OPTIONS]\n"
                   "Options:\n"
                   "  -t       Show only TCP connections\n"
                   "  -u       Show only UDP connections\n"
                   "  -l       Show only listening sockets\n"
                   "  -n       Numeric addresses (no name resolution)\n"
                   "  -i SEC   Update interval in seconds (default: 1.0)\n"
                   "  -1       One-shot: single display, then exit\n"
                   "  -h       Show this help\n");
            return 0;
        }
        case ':': {
            opterr = saved_opterr;
            fprintf(stderr, "netstatplus: option -%c requires a value\n", optopt);
            fprintf(stderr, "Usage: netstatplus [-t] [-u] [-l] [-n] [-i SECS] [-1] [-h]\n");
            return 2;
        }
        default:
            opterr = saved_opterr;
            fprintf(stderr, "netstatplus: unknown option -%c\n", optopt);
            fprintf(stderr, "Usage: netstatplus [-t] [-u] [-l] [-n] [-i SECS] [-1] [-h]\n");
            return 2;
        }
    }
    opterr = saved_opterr;

    /* Default: if neither -t nor -u given, show both. */
    if (!had_tcp && !had_udp) {
        cfg.show_tcp = 1;
        cfg.show_udp = 1;
    }

    if (cfg.interval <= 0) cfg.interval = 1.0;

    /* Set up signal handler for graceful interrupt. */
    struct sigaction sa, old_int, old_term;
    interrupted = 0;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, &old_int) == -1) {
        perror("netstatplus: sigaction(SIGINT)");
        return 1;
    }
    if (sigaction(SIGTERM, &sa, &old_term) == -1) {
        perror("netstatplus: sigaction(SIGTERM)");
        sigaction(SIGINT, &old_int, NULL);
        return 1;
    }

    /* Main monitoring loop. */
    snapshot_t snap = {{{0}}, 0};
    int64_t prev_recv[MAX_CONN], prev_sent[MAX_CONN];
    memset(prev_recv, -1, (size_t)(sizeof(prev_recv)));
    memset(prev_sent, -1, (size_t)(sizeof(prev_sent)));

    while (!interrupted) {
        /* Take current snapshot. */
        int count = take_snapshot(&cfg, &snap);
        if (count < 0) {
            fprintf(stderr, "netstatplus: could not read /proc/net files\n");
            goto done;
        }

        /* Print header once. */
        print_header();

        /* Print each entry with throughput delta. */
        for (int i = 0; i < count; i++) {
            int64_t pr = prev_recv[i];
            int64_t ps = prev_sent[i];
            print_entry(&cfg, &snap.entries[i], pr, ps);
            prev_recv[i] = snap.entries[i].bytes_recv;
            prev_sent[i] = snap.entries[i].bytes_sent;
        }

        if (cfg.one_shot) break;

        /* Wait for next interval using poll(). */
        int timeout_ms = (int)(cfg.interval * 1000);
        struct pollfd pfd;
        pfd.fd     = STDIN_FILENO;
        pfd.events = POLLIN | POLLHUP;
        pfd.revents = 0;

        poll(&pfd, 0, timeout_ms); /* sleep; check interrupted flag */

        if (interrupted) break;
    }

done:
    sigaction(SIGTERM, &old_term, NULL);
    sigaction(SIGINT,  &old_int,  NULL);

    if (interrupted) {
        printf("\nnetstatplus: interrupted\n");
        return 130;
    }

    return 0;
}

/* Standalone execution. */
#ifndef NETSTATPLUS_NO_MAIN
int main(int argc, char *argv[]) {
    return netstatplus(argc, argv);
}
#endif
