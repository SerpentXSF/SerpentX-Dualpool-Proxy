/*
 * splitmux_probe.c — standalone harness that drives splitmux_run() directly
 * (the splitter is not wired to splitmux until M6.2). Listens for ONE miner,
 * connects ONE upstream, and relays between them via splitmux_run. GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#define _POSIX_C_SOURCE 200112L   /* getaddrinfo/freeaddrinfo under -std=c11 */

#include "splitmux.h"

#include <netdb.h>
#include <signal.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int listen_one(int port)
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
    if (listen(ls, 1) < 0) { perror("listen"); close(ls); return -1; }

    int c = accept(ls, NULL, NULL);
    if (c < 0) perror("accept");
    close(ls);
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
    int ratio = 100, target = 10, min_s = 10, max_s = 120;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--listen") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--upstream") && i + 1 < argc) upstream = argv[++i];
        else if (!strcmp(argv[i], "--ratio") && i + 1 < argc) ratio = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--target") && i + 1 < argc) target = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--min") && i + 1 < argc) min_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) max_s = atoi(argv[++i]);
    }
    if (port <= 0 || !upstream) {
        fprintf(stderr,
                "usage: %s --listen <port> --upstream <host:port>"
                " [--ratio N] [--target N] [--min N] [--max N]\n", argv[0]);
        return 2;
    }

    int down_fd = listen_one(port);
    if (down_fd < 0) return 1;

    int up = dial(upstream);
    if (up < 0) { close(down_fd); return 1; }

    int up_fd[2] = { up, -1 };
    splitmux_run(down_fd, up_fd, ratio, target, min_s, max_s);

    close(up);
    close(down_fd);
    return 0;
}
