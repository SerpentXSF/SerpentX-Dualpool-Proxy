/*
 * relay.c — transparent bidirectional byte relay. See relay.h.
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#include "relay.h"

#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define RELAY_BUF 4096

/* Per-direction line accumulator for the optional sniff callback. Bytes are
 * always forwarded verbatim; this only assembles copies for the callback. */
typedef struct {
    char   buf[8192];
    size_t len;
} line_acc_t;

static void acc_feed(line_acc_t *a, const char *data, ssize_t n, int up,
                     relay_sniff_fn sniff, void *ctx)
{
    for (ssize_t i = 0; i < n; i++) {
        char c = data[i];
        if (a->len < sizeof(a->buf) - 1)
            a->buf[a->len++] = c;
        if (c == '\n') {
            a->buf[a->len] = '\0';
            sniff(ctx, up, a->buf, a->len);
            a->len = 0;
        }
    }
}

/* Write all n bytes, retrying short writes. Returns 0 on success, -1 on error. */
static int write_all(int fd, const char *p, ssize_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= w;
    }
    return 0;
}

void relay_pump(int down_fd, int up_fd, relay_sniff_fn sniff, void *ctx)
{
    struct pollfd fds[2];
    fds[0].fd = down_fd; fds[0].events = POLLIN;
    fds[1].fd = up_fd;   fds[1].events = POLLIN;

    line_acc_t acc_up = {0};   /* downstream -> upstream lines */
    line_acc_t acc_dn = {0};   /* upstream -> downstream lines */
    char buf[RELAY_BUF];

    for (;;) {
        int r = poll(fds, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            return;
        }

        /* downstream -> upstream */
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(down_fd, buf, sizeof(buf));
            if (n <= 0) return;
            if (write_all(up_fd, buf, n) < 0) return;
            if (sniff) acc_feed(&acc_up, buf, n, 1, sniff, ctx);
        }
        /* upstream -> downstream */
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(up_fd, buf, sizeof(buf));
            if (n <= 0) return;
            if (write_all(down_fd, buf, n) < 0) return;
            if (sniff) acc_feed(&acc_dn, buf, n, 0, sniff, ctx);
        }
    }
}
