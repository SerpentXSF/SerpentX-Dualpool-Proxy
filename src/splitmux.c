/*
 * splitmux.c — Stratum-aware multiplexer. See splitmux.h.
 *
 * M3 scope: relay ONE miner (down_fd) through ONE upstream (up_fd[0]) with a
 * poll() loop. Relay is line-buffered and verbatim: TCP is a byte stream, so a
 * per-direction buffer accumulates bytes, complete '\n'-terminated lines are
 * forwarded unchanged (trailing '\n' included), and partial trailing data is
 * preserved across reads. Each line is parsed with stratum_msg_parse; when an
 * upstream line is SM_NOTIFY its job_id is recorded into a small in-module ring
 * (groundwork for M4 routing/swap — no routing here). up_fd[1] is -1 in M3 and
 * is ignored entirely.
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#include "splitmux.h"
#include "stratum_msg.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SPLITMUX_BUF   131072   /* per-direction line-assembly buffer */
#define SPLITMUX_LINE  16384    /* max single line we bother to parse */
#define SPLITMUX_JOBS  32       /* current upstream job-ids remembered (M4 use) */

/* Per-direction line-assembly buffer holding an as-yet-incomplete trailing
 * line plus any complete lines read in the latest chunk. */
typedef struct {
    char   data[SPLITMUX_BUF];
    size_t len;
} linebuf_t;

/* Fixed-size ring of the most recent job-ids seen from up_fd[0]. */
typedef struct {
    char ids[SPLITMUX_JOBS][64];
    int  count;   /* number of valid entries (capped at SPLITMUX_JOBS) */
    int  head;    /* next write slot */
} jobset_t;

static void jobset_add(jobset_t *js, const char *job_id)
{
    if (!job_id[0])
        return;
    snprintf(js->ids[js->head], sizeof js->ids[js->head], "%s", job_id);
    js->head = (js->head + 1) % SPLITMUX_JOBS;
    if (js->count < SPLITMUX_JOBS)
        js->count++;
}

/* Write exactly n bytes, retrying short writes and EINTR. Returns 0 on
 * success, -1 if the peer went away. */
static int write_all(int fd, const char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (w == 0)
            return -1;
        off += (size_t)w;
    }
    return 0;
}

/* Parse each complete '\n'-terminated line in [buf, buf+n); when from_upstream
 * and the line is a mining.notify, record its job_id. Parsing is best-effort:
 * lines that fail to parse (or are absurdly long) are simply not recorded —
 * they are still forwarded verbatim by the caller. */
static void scan_lines(const char *buf, size_t n, bool from_upstream,
                       jobset_t *js)
{
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] != '\n')
            continue;
        size_t line_len = i - start;
        if (line_len < SPLITMUX_LINE) {
            char tmp[SPLITMUX_LINE];
            memcpy(tmp, buf + start, line_len);
            tmp[line_len] = '\0';
            stratum_msg_t m;
            if (stratum_msg_parse(tmp, &m) == 0 &&
                from_upstream && m.type == SM_NOTIFY)
                jobset_add(js, m.job_id);
        }
        start = i + 1;
    }
}

/* Read one chunk from `from_fd`, append to the direction buffer, forward every
 * complete line verbatim to `to_fd`, and preserve the partial tail. Returns 0
 * to keep going, -1 when the direction closed or errored. */
static int pump(int from_fd, int to_fd, linebuf_t *lb, bool from_upstream,
                jobset_t *js)
{
    ssize_t r = read(from_fd, lb->data + lb->len, SPLITMUX_BUF - lb->len);
    if (r < 0)
        return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    if (r == 0)
        return -1;                 /* peer closed */
    lb->len += (size_t)r;

    /* Find the end of the last complete line in the buffer. */
    size_t flush = 0;
    for (size_t i = 0; i < lb->len; i++)
        if (lb->data[i] == '\n')
            flush = i + 1;

    if (flush > 0) {
        if (write_all(to_fd, lb->data, flush) < 0)
            return -1;
        scan_lines(lb->data, flush, from_upstream, js);
        memmove(lb->data, lb->data + flush, lb->len - flush);
        lb->len -= flush;
    } else if (lb->len == SPLITMUX_BUF) {
        /* A single line longer than the whole buffer: forward what we have
         * verbatim rather than deadlock. */
        if (write_all(to_fd, lb->data, lb->len) < 0)
            return -1;
        lb->len = 0;
    }
    return 0;
}

void splitmux_run(int down_fd, int up_fd[2], int ratio_a,
                  int target_shares, int min_s, int max_s)
{
    /* M3 uses neither the scheduler knobs nor the second upstream. */
    (void)ratio_a;
    (void)target_shares;
    (void)min_s;
    (void)max_s;

    int up = up_fd[0];             /* up_fd[1] == -1 in M3, ignored */

    linebuf_t d2u = { .len = 0 };  /* miner  -> upstream */
    linebuf_t u2d = { .len = 0 };  /* upstream -> miner */
    jobset_t  jobs = { .count = 0, .head = 0 };

    for (;;) {
        struct pollfd pfds[2];
        pfds[0].fd = down_fd; pfds[0].events = POLLIN; pfds[0].revents = 0;
        pfds[1].fd = up;      pfds[1].events = POLLIN; pfds[1].revents = 0;

        int rc = poll(pfds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        /* An invalid fd (e.g. one a future embedder closed) yields POLLNVAL and
         * neither read branch below; bail rather than spin at 100% CPU. */
        if ((pfds[0].revents | pfds[1].revents) & POLLNVAL)
            break;

        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (pump(down_fd, up, &d2u, false, &jobs) < 0)
                break;
        }
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (pump(up, down_fd, &u2d, true, &jobs) < 0)
                break;
        }
    }
}
