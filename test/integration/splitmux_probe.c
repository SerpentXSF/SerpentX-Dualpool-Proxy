/*
 * splitmux_probe.c — standalone harness that drives splitmux_run() directly
 * (the splitter is not wired to splitmux until M6.2). Listens for ONE miner,
 * connects ONE upstream (M3) or TWO upstreams via --upstream2 (M4), and relays
 * between them via splitmux_run. GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#define _POSIX_C_SOURCE 200112L   /* getaddrinfo/freeaddrinfo under -std=c11 */

#include "splitmux.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Open a persistent listening socket (kept open across reconnects in loop mode). */
static int make_listener(int port)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return -1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0) {
        perror("bind"); close(ls); return -1;
    }
    if (listen(ls, 4) < 0) { perror("listen"); close(ls); return -1; }
    return ls;
}

/* Accept one miner, waiting up to timeout_ms. Returns the connected fd, -1 on
 * error, or -2 if no miner arrived within the timeout (used to end a reconnect
 * loop cleanly once the miner stops coming back). */
static int accept_one(int ls, int timeout_ms)
{
    struct pollfd pf = { .fd = ls, .events = POLLIN, .revents = 0 };
    int pr = poll(&pf, 1, timeout_ms);
    if (pr < 0) { if (errno == EINTR) return -2; perror("poll"); return -1; }
    if (pr == 0) return -2;                 /* timed out: no more reconnects */
    int c = accept(ls, NULL, NULL);
    if (c < 0) perror("accept");
    return c;
}

static int dial(const char *hostport)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", hostport);
    char *colon = strrchr(buf, ':');
    if (!colon) { fprintf(stderr, "bad upstream %s\n", hostport); return -1; }
    *colon = '\0';
    const char *host = buf;
    const char *port = colon + 1;

    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) {
        fprintf(stderr, "getaddrinfo %s:%s failed\n", host, port);
        return -1;
    }
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) fprintf(stderr, "connect %s failed\n", hostport);
    return fd;
}

int main(int argc, char **argv)
{
    /* splitmux_run relies on the process ignoring SIGPIPE (see splitmux.h): a
     * miner that vanishes mid-relay must unwind, not signal-kill this probe. */
    signal(SIGPIPE, SIG_IGN);

    int port = 0;
    const char *upstream = NULL;
    const char *upstream2 = NULL;
    int ratio = 100, target = 10, min_s = 10, max_s = 120;
    int start_pool = -1;      /* default: seed from ratio (M3/M4 unchanged) */
    int recon_loop = 0;       /* >0: M5 reconnect loop, this many iterations max */
    int alt_start = 0;        /* alternate start_pool 0,1,0,1,... per reconnect */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--listen") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--upstream") && i + 1 < argc) upstream = argv[++i];
        else if (!strcmp(argv[i], "--upstream2") && i + 1 < argc) upstream2 = argv[++i];
        else if (!strcmp(argv[i], "--ratio") && i + 1 < argc) ratio = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--target") && i + 1 < argc) target = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--min") && i + 1 < argc) min_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) max_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--start-pool") && i + 1 < argc) start_pool = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reconnect-loop") && i + 1 < argc) recon_loop = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--alt-start")) alt_start = 1;
    }
    if (port <= 0 || !upstream) {
        fprintf(stderr,
                "usage: %s --listen <port> --upstream <host:port>"
                " [--upstream2 <host:port>] [--ratio N] [--target N]"
                " [--min N] [--max N] [--start-pool 0|1]"
                " [--reconnect-loop N] [--alt-start]\n", argv[0]);
        return 2;
    }

    /* -------- M5 reconnect-slice loop: accept miner, run, and when the mux
     * returns (fallback drop or miner leaving) accept the next reconnect with
     * the next start_pool. Ends cleanly once no miner reconnects. -------- */
    if (recon_loop > 0) {
        if (!upstream2) {
            fprintf(stderr, "--reconnect-loop needs --upstream2\n");
            return 2;
        }
        int ls = make_listener(port);
        if (ls < 0) return 1;
        int rc = 0;
        for (int it = 0; it < recon_loop; it++) {
            int down_fd = accept_one(ls, 5000);
            if (down_fd == -2) break;             /* no more reconnects: done */
            if (down_fd < 0) { rc = 1; break; }

            int up = dial(upstream);
            int up2 = dial(upstream2);
            if (up < 0 || up2 < 0) {
                if (up >= 0) close(up);
                if (up2 >= 0) close(up2);
                close(down_fd);
                rc = 1; break;
            }
            int sp = alt_start ? (it & 1) : start_pool;
            int up_fd[2] = { up, up2 };
            splitmux_run(down_fd, up_fd, ratio, target, min_s, max_s, sp);

            close(up2);
            close(up);
            close(down_fd);
        }
        close(ls);
        return rc;
    }

    /* -------- single-shot (M3/M4): one miner, one session. -------- */
    int ls = make_listener(port);
    if (ls < 0) return 1;
    int down_fd = accept(ls, NULL, NULL);
    close(ls);
    if (down_fd < 0) { perror("accept"); return 1; }

    int up = dial(upstream);
    if (up < 0) { close(down_fd); return 1; }

    int up2 = -1;
    if (upstream2) {
        up2 = dial(upstream2);
        if (up2 < 0) { close(up); close(down_fd); return 1; }
    }

    int up_fd[2] = { up, up2 };
    splitmux_run(down_fd, up_fd, ratio, target, min_s, max_s, start_pool);

    if (up2 >= 0) close(up2);
    close(up);
    close(down_fd);
    return 0;
}
