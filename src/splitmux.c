/*
 * splitmux.c — Stratum-aware multiplexer. See splitmux.h.
 *
 * Two paths, selected by up_fd[1]:
 *
 *  - Single-pool passthrough (up_fd[1] == -1, the M3 probe / M6.1 fallback):
 *    line-buffered verbatim relay between the miner and one upstream. TCP is a
 *    byte stream, so a per-direction buffer accumulates bytes, complete
 *    '\n'-terminated lines are forwarded unchanged, and a partial trailing line
 *    is preserved across reads.
 *
 *  - Dual-pool swap (up_fd[1] >= 0, M4): the mux owns BOTH upstream Stratum
 *    sessions and synthesizes the miner-facing session. It drives subscribe/
 *    authorize to both pools, presents ONE pool's work at a time, alternates
 *    ("splits") between the pools on adaptive clean-job boundaries, and routes
 *    each miner submit to the pool whose job-id it was found on (with a stale
 *    grace window). Submit bytes reach their pool VERBATIM.
 *
 * Two review fixes carried over from M3 are folded into the line machinery:
 *   - resume offset: newline scanning resumes at the last scanned index rather
 *     than rescanning the whole buffer from 0 on every read (avoids O(n^2));
 *   - ring-poison guard: when a single line overflows the whole buffer, the
 *     trailing fragment after the flush is marked "skip until next newline" so
 *     it is never parsed as a complete line (cannot inject a bogus job-id).
 *
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#define _POSIX_C_SOURCE 200809L   /* clock_gettime, poll under -std=c11 */

#include "splitmux.h"
#include "stratum_msg.h"
#include "split_sched.h"

#include <errno.h>
#include <jansson.h>
#include <netdb.h>
#include <stdarg.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define SPLITMUX_BUF   131072      /* per-direction line-assembly buffer */
#define SPLITMUX_LINE  16384       /* max single line we bother to parse */
#define SPLITMUX_RING  128         /* job-id -> pool routing ring size */
#define GRACE_US       30000000LL  /* stale-share grace after leaving a pool */
#define HS_TIMEOUT_MS  5000        /* per-read handshake timeout (abort dead pool) */

/* The mux drives its OWN handshake to the pools with sentinel JSON-RPC ids the
 * miner will never use for submits. A pool's reply carrying one of these is the
 * mux's own handshake ack and is dropped; any other result is a submit ack (or
 * ack to a post-handshake miner request) and is relayed to the miner verbatim. */
#define SENTINEL_SUBSCRIBE  90000001LL
#define SENTINEL_AUTHORIZE  90000002LL
#define SENTINEL_CONFIGURE  90000003LL

/* Asynchronous secondary-pool bring-up. The PRIMARY pool is handshaked
 * synchronously and the miner mines it at once; the SECONDARY is brought up by a
 * non-blocking state machine driven from the main poll loop, so a pool that
 * isn't ready the instant we subscribe (real ckproxy: "Temporarily insufficient
 * proxies") never blocks the miner and is retried until it becomes ready. */
#define SEC_HANDSHAKING  1   /* subscribe/authorize sent, awaiting result+notify */
#define SEC_READY        2   /* enonce1/diff/notify captured — swaps may target it */
#define SEC_WAIT         3   /* attempt failed; backing off before a reconnect */

#define SEC_ATTEMPT_MS   8000     /* abort one bring-up attempt after this long */
#define SEC_BACKOFF_MIN  3        /* first retry backoff (s) */
#define SEC_BACKOFF_MAX  30       /* backoff cap (s) */

/* Per-direction line-assembly buffer. */
typedef struct {
    char   data[SPLITMUX_BUF];
    size_t len;      /* bytes currently buffered */
    size_t scan;     /* resume offset: bytes already searched for '\n' */
    bool   poison;   /* skip-parse of the next line (oversized-line guard) */
} linebuf_t;

/* One entry of the job-id -> pool routing ring. */
typedef struct {
    char    job_id[64];
    int     pool;      /* 0 = A, 1 = B */
    int64_t seen_us;
} jobref_t;

typedef struct {
    jobref_t e[SPLITMUX_RING];
    int      count;
    int      head;
} ring_t;

/* Per-upstream session state synthesized from that pool's notifies/diffs. */
typedef struct {
    int    fd;
    char   enonce1[64];
    int    n2len;
    double diff;
    char   cur_job[64];
    char   last_clean_job[64];
    int64_t left_us;                    /* FIX-2: when the mux last left this pool */
    char    last_notify[SPLITMUX_LINE]; /* FIX-5: latest notify line from this pool */
    size_t  last_notify_len;
} pool_t;

/* Whole dual-mode multiplexer state. */
typedef struct {
    int      down_fd;
    pool_t   pool[2];
    int      active;             /* pool currently presented to the miner */
    ring_t   ring;

    /* slice state machine */
    int64_t  slice_start_us;
    int64_t  slice_deadline_us;
    int64_t  a_us, b_us;         /* cumulative completed active time per pool */
    bool     pending;            /* a switch is armed, waiting on target clean */
    int      target;             /* pool we are switching to */

    /* hashrate estimate: sum of diff*2^32 over routed submits */
    double   total_work;

    /* config knobs */
    int      ratio_a, target_shares, min_s, max_s;
    int      start_pool;         /* <0 seed from ratio; 0/1 begin on that pool */

    /* M5 capability detection: true once the miner sends
     * mining.extranonce.subscribe (=> honours set_extranonce, smooth swaps).
     * false => reconnect-slice fallback at the deadline. */
    bool     miner_ext_ok;

    /* asynchronous secondary-pool bring-up (the pool that is NOT the primary) */
    int         sec;             /* secondary pool index (0/1) */
    int         sec_state;       /* SEC_HANDSHAKING / SEC_READY / SEC_WAIT */
    bool        sec_got_sub;     /* this attempt captured the subscribe result */
    bool        sec_degrade_logged; /* logged the single-pool-degrade note once */
    int64_t     sec_attempt_deadline_us;  /* abort the current attempt at this time */
    int64_t     sec_retry_at_us;          /* reconnect when now >= this (SEC_WAIT) */
    int         sec_backoff_s;   /* current retry backoff (s) */
    char        worker[128];     /* miner's worker, replayed to authorize the sec */
    char        pass[128];       /* miner's password, ditto */
    const char *up_addr[2];      /* "host:port" per pool, for secondary reconnect */

    linebuf_t bmin;              /* miner  -> mux */
    linebuf_t bup[2];            /* upstream[p] -> mux */
} mux_t;

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/* ---- env-gated handshake debug (SPLITMUX_DEBUG=1) -----------------------
 * Dumps every complete Stratum line the mux SENDS to / RECEIVES from each pool
 * during handshake + steady state, and each submit's routing decision, so a
 * real-ckproxy transcript can be compared line-for-line against a byte relay.
 * Off (one getenv, cached) unless SPLITMUX_DEBUG is set — zero prod overhead. */
static int smx_dbg_on(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = getenv("SPLITMUX_DEBUG") ? 1 : 0;
    return cached;
}

/* pool: 0=A, 1=B, -1=miner. dir: "TX" (mux->peer) or "RX" (peer->mux). */
static void smx_dbg_line(const char *dir, int pool, const char *line, size_t len)
{
    if (!smx_dbg_on())
        return;
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        len--;
    char tag = pool < 0 ? 'M' : (pool == 0 ? 'A' : 'B');
    fprintf(stderr, "[smux %s %c] %.*s\n", dir, tag, (int)len, line);
}

__attribute__((format(printf, 1, 2)))
static void smx_dbg(const char *fmt, ...)
{
    if (!smx_dbg_on())
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[smux    ] ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* Connect a fresh TCP socket to a "host:port" string (last colon splits). Used
 * to re-open the secondary upstream between retry attempts. Returns the fd, or
 * -1 on any failure. getaddrinfo is available under _POSIX_C_SOURCE 200809L. */
static int connect_hostport(const char *hostport)
{
    if (!hostport)
        return -1;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", hostport);
    char *colon = strrchr(buf, ':');
    if (!colon)
        return -1;
    *colon = '\0';

    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(buf, colon + 1, &hints, &res) != 0)
        return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
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

/* True iff s is a non-empty string of hex digits (defensive validation of a
 * pool-supplied extranonce1 before it is spliced into a JSON reply). */
static bool is_hex_str(const char *s)
{
    if (!s || !*s)
        return false;
    for (; *s; s++) {
        char c = *s;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

/* Rewrite a mining.notify line with clean_jobs forced true (params[8]).
 * On success writes the compact line + '\n' + NUL into out and returns 0. */
static int rewrite_notify_clean(const char *line, char *out, size_t outsz,
                                size_t *outlen)
{
    json_error_t e;
    json_t *r = json_loads(line, 0, &e);
    if (!r)
        return -1;
    json_t *params = json_object_get(r, "params");
    if (!json_is_array(params) || json_array_size(params) < 9) {
        json_decref(r);
        return -1;
    }
    json_array_set_new(params, 8, json_true());
    char *dump = json_dumps(r, JSON_COMPACT);
    json_decref(r);
    if (!dump)
        return -1;
    size_t dl = strlen(dump);
    if (dl + 2 > outsz) {           /* need room for '\n' and NUL */
        free(dump);
        return -1;
    }
    memcpy(out, dump, dl);
    out[dl] = '\n';
    out[dl + 1] = '\0';
    *outlen = dl + 1;
    free(dump);
    return 0;
}

/* Forward `line` to fd but with its JSON-RPC "id" replaced by new_id (used to
 * stamp the mux's sentinel id onto a passed-through mining.configure). Falls
 * back to a verbatim relay if the line does not parse. */
static int forward_with_id(int fd, const char *line, int64_t new_id)
{
    json_error_t e;
    json_t *r = json_loads(line, 0, &e);
    if (!r)
        return write_all(fd, line, strlen(line));
    json_object_set_new(r, "id", json_integer(new_id));
    char *dump = json_dumps(r, JSON_COMPACT);
    json_decref(r);
    if (!dump)
        return -1;
    int rc = write_all(fd, dump, strlen(dump));
    if (rc == 0)
        rc = write_all(fd, "\n", 1);
    free(dump);
    return rc;
}

/* ---- routing ring -------------------------------------------------------- */

static void ring_add(ring_t *r, const char *job_id, int pool, int64_t t)
{
    if (!job_id[0])
        return;
    snprintf(r->e[r->head].job_id, sizeof r->e[r->head].job_id, "%s", job_id);
    r->e[r->head].pool = pool;
    r->e[r->head].seen_us = t;
    r->head = (r->head + 1) % SPLITMUX_RING;
    if (r->count < SPLITMUX_RING)
        r->count++;
}

/* Return the ring index of the newest entry for job_id, or -1. */
static int ring_find(const ring_t *r, const char *job_id)
{
    for (int k = 1; k <= r->count; k++) {
        int i = (r->head - k + SPLITMUX_RING) % SPLITMUX_RING;
        if (!strcmp(r->e[i].job_id, job_id))
            return i;
    }
    return -1;
}

/* ============================ single-pool path =========================== */

/* Read a chunk into lb; forward every complete line verbatim to `to_fd`;
 * preserve the partial tail. Uses the resume offset and honours the poison
 * flag (an oversized line's trailing fragment is forwarded but not "counted").
 * Returns 0 to keep going, -1 when the direction closed or errored. */
static int pump_verbatim(int from_fd, int to_fd, linebuf_t *lb)
{
    ssize_t r = read(from_fd, lb->data + lb->len, SPLITMUX_BUF - lb->len);
    if (r < 0)
        return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    if (r == 0)
        return -1;
    lb->len += (size_t)r;

    size_t flush = 0;
    for (size_t i = lb->scan; i < lb->len; i++)
        if (lb->data[i] == '\n')
            flush = i + 1;
    lb->scan = lb->len;

    if (flush > 0) {
        if (write_all(to_fd, lb->data, flush) < 0)
            return -1;
        /* One complete line closes any poisoned fragment. */
        lb->poison = false;
        memmove(lb->data, lb->data + flush, lb->len - flush);
        lb->len -= flush;
        lb->scan = lb->len;
    } else if (lb->len == SPLITMUX_BUF) {
        /* A single line longer than the whole buffer: forward what we have
         * verbatim rather than deadlock, and poison the trailing fragment. */
        if (write_all(to_fd, lb->data, lb->len) < 0)
            return -1;
        lb->len = 0;
        lb->scan = 0;
        lb->poison = true;
    }
    return 0;
}

/* Forward the complete lines already buffered in lb to to_fd, leaving only a
 * partial trailing line. Used to seed the degrade-to-single-pool relay with
 * bytes the miner/pool pipelined during the dual handshake so nothing is lost.
 * Returns 0, or -1 on write failure. */
static int flush_buffered(int to_fd, linebuf_t *lb)
{
    size_t flush = 0;
    for (size_t i = 0; i < lb->len; i++)
        if (lb->data[i] == '\n')
            flush = i + 1;
    if (flush > 0) {
        if (write_all(to_fd, lb->data, flush) < 0)
            return -1;
        memmove(lb->data, lb->data + flush, lb->len - flush);
        lb->len -= flush;
    }
    lb->scan = lb->len;
    lb->poison = false;
    return 0;
}

/* Verbatim two-way relay between the miner and one upstream, seeded from
 * pre-filled linebufs (empty for a from-scratch passthrough, or carrying
 * handshake leftovers on the degrade path). */
static void splitmux_relay_seeded(int down_fd, int up,
                                  linebuf_t *d2u, linebuf_t *u2d)
{
    if (flush_buffered(up, d2u) < 0)      return;   /* miner  -> upstream leftovers */
    if (flush_buffered(down_fd, u2d) < 0) return;   /* upstream -> miner leftovers */

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
        if ((pfds[0].revents | pfds[1].revents) & POLLNVAL)
            break;

        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (pump_verbatim(down_fd, up, d2u) < 0)
                break;
        }
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (pump_verbatim(up, down_fd, u2d) < 0)
                break;
        }
    }
}

static void splitmux_passthrough(int down_fd, int up)
{
    linebuf_t d2u = { .len = 0, .scan = 0, .poison = false };
    linebuf_t u2d = { .len = 0, .scan = 0, .poison = false };
    splitmux_relay_seeded(down_fd, up, &d2u, &u2d);
}

/* ============================== dual-pool path =========================== */

/* Blocking read of one complete line from fd via lb. Returns 1 with the line
 * (including trailing '\n', NUL-terminated) in out, 0 on EOF/error. */
static int blocking_line(int fd, linebuf_t *lb, char *out, size_t outsz,
                         size_t *outlen)
{
    for (;;) {
        for (size_t i = 0; i < lb->len; i++) {
            if (lb->data[i] != '\n')
                continue;
            size_t ln = i + 1;
            if (lb->poison) {
                /* Tail of a previously dropped oversized line: discard it
                 * (never let the fragment parse as a real line), then rescan. */
                lb->poison = false;
                memmove(lb->data, lb->data + ln, lb->len - ln);
                lb->len -= ln;
                lb->scan = 0;
                i = (size_t)-1;          /* ++ -> restart scan at 0 */
                continue;
            }
            size_t cp = ln < outsz ? ln : outsz - 1;
            memcpy(out, lb->data, cp);
            out[cp] = '\0';
            *outlen = cp;
            memmove(lb->data, lb->data + ln, lb->len - ln);
            lb->len -= ln;
            lb->scan = 0;
            return 1;
        }
        if (lb->len == SPLITMUX_BUF) {   /* oversized during handshake: drop */
            lb->len = 0;
            lb->scan = 0;
            lb->poison = true;           /* FIX-10: skip the trailing fragment */
        }
        /* FIX-6: bound the wait so a pool that accepts TCP but never answers
         * aborts the handshake cleanly instead of hanging the miner forever. */
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, HS_TIMEOUT_MS);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        if (pr == 0)
            return 0;                    /* timed out -> treat as dead peer */
        ssize_t r = read(fd, lb->data + lb->len, SPLITMUX_BUF - lb->len);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            return 0;
        }
        lb->len += (size_t)r;
    }
}

/* Update per-pool state (diff / cur_job / last_notify) from a notify or
 * set_difficulty line WITHOUT forwarding it to the miner. Used mid-handshake so
 * an early set_difficulty/notify is not discarded (leaving pool.diff at 0.0). */
static void pool_capture_state(mux_t *m, int p, const char *line)
{
    stratum_msg_t msg;
    if (stratum_msg_parse(line, &msg) != 0)
        return;
    if (msg.type == SM_SET_DIFFICULTY) {
        m->pool[p].diff = msg.diff;
    } else if (msg.type == SM_NOTIFY) {
        snprintf(m->pool[p].cur_job, sizeof m->pool[p].cur_job, "%s",
                 msg.job_id);
        if (msg.clean_jobs)
            snprintf(m->pool[p].last_clean_job,
                     sizeof m->pool[p].last_clean_job, "%s", msg.job_id);
        size_t len = strlen(line);
        if (len > 0 && len < sizeof m->pool[p].last_notify) {
            memcpy(m->pool[p].last_notify, line, len);
            m->pool[p].last_notify[len] = '\0';
            m->pool[p].last_notify_len = len;
        }
    }
}

/* Blocking read of one pool's mining.subscribe result; capture enonce1/n2len. */
static int read_subscribe_result(mux_t *m, int p)
{
    char line[SPLITMUX_LINE];
    size_t ll;
    for (;;) {
        if (!blocking_line(m->pool[p].fd, &m->bup[p], line, sizeof line, &ll))
            return -1;
        smx_dbg_line("RX", p, line, ll);
        json_error_t e;
        json_t *r = json_loads(line, 0, &e);
        if (!r)
            continue;
        json_t *res = json_object_get(r, "result");
        if (json_is_array(res)) {
            const char *en = json_string_value(json_array_get(res, 1));
            json_t *n2 = json_array_get(res, 2);
            snprintf(m->pool[p].enonce1, sizeof m->pool[p].enonce1, "%s",
                     en ? en : "");
            m->pool[p].n2len = (int)json_integer_value(n2);
            json_decref(r);
            /* FIX-10: a non-hex enonce1 could break the synthesized subscribe
             * reply's JSON — reject the pool rather than emit a malformed line. */
            if (!is_hex_str(m->pool[p].enonce1))
                return -1;
            return 0;
        }
        json_decref(r);
        /* FIX-6: not the subscribe result — an early set_difficulty/notify.
         * Capture its state instead of dropping it, then keep looking. */
        pool_capture_state(m, p, line);
    }
}

/* Synchronously bring up ONE pool (the PRIMARY) and reply to the miner from it.
 * Prefers `start_pool` (0/1) else A when ratio favours A; on the preferred
 * pool's subscribe failing, falls back to the other pool as primary. Reads the
 * miner's subscribe + authorize (tolerating configure / extranonce.subscribe),
 * captures the worker/password for the async secondary's own authorize, and
 * replies to the miner exactly as the old happy path did. Returns the primary
 * pool index (0/1) with m->active set, or -1 if the miner left or BOTH pools
 * failed their subscribe. The secondary is NOT touched here — the poll loop
 * brings it up asynchronously. */
static int primary_handshake(mux_t *m)
{
    char line[SPLITMUX_LINE];
    size_t ll;
    int64_t sub_id = 1;

    int pref = (m->start_pool == 0 || m->start_pool == 1)
                   ? m->start_pool
                   : ((m->ratio_a >= 50) ? 0 : 1);

    /* 1. miner subscribe (tolerate a leading mining.configure). Forward the
     * configure only to the PRIMARY candidate side is not knowable yet, so send
     * it to both existing fds under the sentinel id (harmless; the secondary may
     * be reconnected later and simply re-subscribes without it). */
    for (;;) {
        if (!blocking_line(m->down_fd, &m->bmin, line, sizeof line, &ll))
            return -1;
        smx_dbg_line("RX", -1, line, ll);
        stratum_msg_t msg;
        if (stratum_msg_parse(line, &msg) != 0)
            continue;
        if (msg.type == SM_CONFIGURE) {
            smx_dbg("primary: forwarding miner mining.configure to BOTH pools "
                    "(sentinel id), answering miner {} verbatim\n");
            if (forward_with_id(m->pool[0].fd, line, SENTINEL_CONFIGURE) < 0) return -1;
            if (forward_with_id(m->pool[1].fd, line, SENTINEL_CONFIGURE) < 0) return -1;
            char rep[128];
            int rl = snprintf(rep, sizeof rep,
                "{\"id\":%lld,\"result\":{},\"error\":null}\n",
                (long long)msg.id);
            if (rl < 0 || (size_t)rl >= sizeof rep) return -1;
            if (write_all(m->down_fd, rep, (size_t)rl) < 0)
                return -1;
            continue;
        }
        if (msg.type == SM_SUBSCRIBE) {
            sub_id = msg.id;
            break;
        }
    }

    /* 2. subscribe the PRIMARY (preferred pool, else the other). Sentinel id. */
    static const char sub[] =
        "{\"id\":90000001,\"method\":\"mining.subscribe\","
        "\"params\":[\"dualpool-mux/1.0\"]}\n";
    int primary = -1;
    smx_dbg("primary: trying pref pool %c — sending mux subscribe\n",
            pref == 0 ? 'A' : 'B');
    smx_dbg_line("TX", pref, sub, sizeof sub - 1);
    if (write_all(m->pool[pref].fd, sub, sizeof sub - 1) == 0 &&
        read_subscribe_result(m, pref) == 0) {
        primary = pref;
    } else {
        int other = pref ^ 1;
        smx_dbg("primary: pref pool %c subscribe FAILED — trying pool %c\n",
                pref == 0 ? 'A' : 'B', other == 0 ? 'A' : 'B');
        smx_dbg_line("TX", other, sub, sizeof sub - 1);
        if (write_all(m->pool[other].fd, sub, sizeof sub - 1) == 0 &&
            read_subscribe_result(m, other) == 0)
            primary = other;
    }
    if (primary < 0) {
        smx_dbg("primary: BOTH pools failed their subscribe — aborting\n");
        return -1;                       /* both pools failed their subscribe */
    }
    m->active = primary;
    smx_dbg("primary: pool %c selected; enonce1=%s n2len=%d\n",
            primary == 0 ? 'A' : 'B', m->pool[primary].enonce1,
            m->pool[primary].n2len);

    /* 3. reply to the miner with the primary's session params */
    pool_t *ap = &m->pool[primary];
    char rep[512];
    int rl = snprintf(rep, sizeof rep,
        "{\"id\":%lld,\"result\":[[[\"mining.set_difficulty\",\"%s\"],"
        "[\"mining.notify\",\"%s\"]],\"%s\",%d],\"error\":null}\n",
        (long long)sub_id, ap->enonce1, ap->enonce1, ap->enonce1, ap->n2len);
    if (rl < 0 || (size_t)rl >= sizeof rep || write_all(m->down_fd, rep, (size_t)rl) < 0)
        return -1;
    smx_dbg_line("TX", -1, rep, (size_t)rl);

    /* 4. miner authorize (tolerate configure / extranonce.subscribe). Capture
     * the worker/password so the async secondary can authorize with them. */
    int64_t auth_id = 2;
    for (;;) {
        if (!blocking_line(m->down_fd, &m->bmin, line, sizeof line, &ll))
            return -1;
        smx_dbg_line("RX", -1, line, ll);
        stratum_msg_t msg;
        if (stratum_msg_parse(line, &msg) != 0)
            continue;
        if (msg.type == SM_CONFIGURE) {
            if (forward_with_id(m->pool[primary].fd, line, SENTINEL_CONFIGURE) < 0) return -1;
            char cr[128];
            int cl = snprintf(cr, sizeof cr,
                "{\"id\":%lld,\"result\":{},\"error\":null}\n",
                (long long)msg.id);
            if (cl < 0 || (size_t)cl >= sizeof cr) return -1;
            if (write_all(m->down_fd, cr, (size_t)cl) < 0)
                return -1;
            continue;
        }
        if (msg.type == SM_EXTRANONCE_SUBSCRIBE) {
            /* M5: miner advertises set_extranonce support (smooth swaps). Record
             * capability and forward to the primary so its ack relays back. */
            m->miner_ext_ok = true;
            smx_dbg("primary: miner is set_extranonce-capable; forwarding "
                    "extranonce.subscribe to pool %c\n", primary == 0 ? 'A' : 'B');
            smx_dbg_line("TX", primary, line, ll);
            if (write_all(m->pool[primary].fd, line, ll) < 0)
                return -1;
            continue;
        }
        if (msg.type == SM_AUTHORIZE) {
            auth_id = msg.id;
            const char *pw = "";
            json_error_t je;
            json_t *mj = json_loads(line, 0, &je);
            if (mj) {
                json_t *mp = json_object_get(mj, "params");
                const char *jp = json_string_value(json_array_get(mp, 1));
                if (jp) pw = jp;
            }
            /* stash worker/pass for the secondary's own mining.authorize */
            snprintf(m->worker, sizeof m->worker, "%s", msg.worker);
            snprintf(m->pass, sizeof m->pass, "%s", pw);
            /* Send the mux's OWN authorize (sentinel id) to the PRIMARY. */
            json_t *aj = json_pack("{s:I,s:s,s:[s,s]}",
                "id", (json_int_t)SENTINEL_AUTHORIZE,
                "method", "mining.authorize",
                "params", msg.worker, pw);
            char *ad = aj ? json_dumps(aj, JSON_COMPACT) : NULL;
            if (mj) json_decref(mj);
            if (aj) json_decref(aj);
            if (!ad) return -1;
            smx_dbg("primary: sending mux's OWN authorize (sentinel id) to pool "
                    "%c for worker '%s'\n", primary == 0 ? 'A' : 'B', msg.worker);
            smx_dbg_line("TX", primary, ad, strlen(ad));
            int arc = write_all(m->pool[primary].fd, ad, strlen(ad));
            if (arc == 0) arc = write_all(m->pool[primary].fd, "\n", 1);
            free(ad);
            if (arc < 0) return -1;
            break;
        }
    }

    /* 5. reply true to the miner */
    char ar[128];
    int al = snprintf(ar, sizeof ar,
        "{\"id\":%lld,\"result\":true,\"error\":null}\n", (long long)auth_id);
    if (al < 0 || (size_t)al >= sizeof ar || write_all(m->down_fd, ar, (size_t)al) < 0)
        return -1;
    smx_dbg_line("TX", -1, ar, (size_t)al);
    smx_dbg("primary: handshake COMPLETE on pool %c (miner authorized true). "
            "NOTE: mux did NOT wait for the pool's authorize RESULT.\n",
            primary == 0 ? 'A' : 'B');
    return primary;
}

/* ------------------- asynchronous secondary-pool bring-up ---------------- */

static int lb_fill(int fd, linebuf_t *lb);   /* defined with the dual poll loop */

/* Send the mux's own subscribe + authorize (sentinel ids) to the secondary fd,
 * pipelined. Returns 0, or -1 on a write failure. */
static int sec_send_handshake(mux_t *m)
{
    int fd = m->pool[m->sec].fd;
    static const char sub[] =
        "{\"id\":90000001,\"method\":\"mining.subscribe\","
        "\"params\":[\"dualpool-mux/1.0\"]}\n";
    smx_dbg("secondary(%c): sending pipelined subscribe+authorize\n",
            m->sec == 0 ? 'A' : 'B');
    smx_dbg_line("TX", m->sec, sub, sizeof sub - 1);
    if (write_all(fd, sub, sizeof sub - 1) < 0)
        return -1;
    json_t *aj = json_pack("{s:I,s:s,s:[s,s]}",
        "id", (json_int_t)SENTINEL_AUTHORIZE,
        "method", "mining.authorize",
        "params", m->worker, m->pass);
    char *ad = aj ? json_dumps(aj, JSON_COMPACT) : NULL;
    if (aj) json_decref(aj);
    if (!ad)
        return -1;
    smx_dbg_line("TX", m->sec, ad, strlen(ad));
    int rc = write_all(fd, ad, strlen(ad));
    if (rc == 0) rc = write_all(fd, "\n", 1);
    free(ad);
    return rc;
}

/* Close the secondary fd, log a one-time single-pool-degrade note, and schedule
 * a backoff reconnect. Called on a failed/timed-out bring-up attempt. */
static void sec_fail(mux_t *m, int64_t t)
{
    int p = m->sec;
    if (m->pool[p].fd >= 0) {
        close(m->pool[p].fd);
        m->pool[p].fd = -1;
    }
    m->sec_state = SEC_WAIT;
    m->sec_got_sub = false;
    m->sec_retry_at_us = t + (int64_t)m->sec_backoff_s * 1000000LL;
    if (!m->sec_degrade_logged) {
        fprintf(stderr, "splitmux: secondary pool %c not ready — single-pool "
                "degrade to pool %c, retrying (backoff %ds)\n",
                p == 0 ? 'A' : 'B', m->active == 0 ? 'A' : 'B', m->sec_backoff_s);
        m->sec_degrade_logged = true;
    }
    int nb = m->sec_backoff_s * 2;
    if (nb > SEC_BACKOFF_MAX) nb = SEC_BACKOFF_MAX;
    m->sec_backoff_s = nb;
}

/* Begin (or retry) a secondary bring-up attempt: (re)connect the fd if needed,
 * reset per-attempt state, and send subscribe+authorize. On connect/send
 * failure, drops back into SEC_WAIT with backoff. */
static void sec_begin_attempt(mux_t *m, int64_t t)
{
    int p = m->sec;
    if (m->pool[p].fd < 0) {
        m->pool[p].fd = connect_hostport(m->up_addr[p]);
        if (m->pool[p].fd < 0) {
            m->sec_state = SEC_WAIT;      /* connect failed: back off and retry */
            m->sec_retry_at_us = t + (int64_t)m->sec_backoff_s * 1000000LL;
            int nb = m->sec_backoff_s * 2;
            if (nb > SEC_BACKOFF_MAX) nb = SEC_BACKOFF_MAX;
            m->sec_backoff_s = nb;
            return;
        }
    }
    /* fresh per-attempt state */
    m->bup[p].len = 0; m->bup[p].scan = 0; m->bup[p].poison = false;
    m->sec_got_sub = false;
    m->pool[p].last_notify_len = 0;
    m->pool[p].enonce1[0] = '\0';
    m->sec_state = SEC_HANDSHAKING;
    m->sec_attempt_deadline_us = t + (int64_t)SEC_ATTEMPT_MS * 1000LL;
    if (sec_send_handshake(m) < 0)
        sec_fail(m, t);
}

/* Parse one secondary handshake line. Captures the sentinel subscribe result
 * (enonce1/n2len) and, via pool_capture_state, set_difficulty/notify. Returns 0
 * to keep going, -1 to fail the attempt (subscribe error or non-hex enonce1). */
static int sec_handshake_line(mux_t *m, const char *line)
{
    int p = m->sec;
    smx_dbg_line("RX", p, line, strlen(line));
    json_error_t e;
    json_t *r = json_loads(line, 0, &e);
    if (!r)
        return 0;
    json_t *idj = json_object_get(r, "id");
    if (idj && json_is_integer(idj) &&
        json_integer_value(idj) == SENTINEL_SUBSCRIBE) {
        json_t *res = json_object_get(r, "result");
        if (!json_is_array(res)) {             /* subscribe error -> fail attempt */
            json_decref(r);
            return -1;
        }
        const char *en = json_string_value(json_array_get(res, 1));
        json_t *n2 = json_array_get(res, 2);
        snprintf(m->pool[p].enonce1, sizeof m->pool[p].enonce1, "%s", en ? en : "");
        m->pool[p].n2len = (int)json_integer_value(n2);
        json_decref(r);
        if (!is_hex_str(m->pool[p].enonce1))
            return -1;                         /* malformed enonce1 -> fail */
        m->sec_got_sub = true;
        return 0;
    }
    json_decref(r);
    /* not the subscribe result: capture set_difficulty / notify state */
    pool_capture_state(m, p, line);
    return 0;
}

/* Read + process pending secondary bytes during SEC_HANDSHAKING. Returns 1 once
 * the pool is READY (subscribe result + a notify captured), 0 to keep going, or
 * -1 if the attempt failed (EOF/error/bad line) and must be retried. */
static int sec_pump(mux_t *m)
{
    int p = m->sec;
    if (lb_fill(m->pool[p].fd, &m->bup[p]) < 0)
        return -1;                             /* EOF / socket error */

    linebuf_t *lb = &m->bup[p];
    size_t flush = 0;
    for (size_t i = lb->scan; i < lb->len; i++)
        if (lb->data[i] == '\n')
            flush = i + 1;
    lb->scan = lb->len;
    if (flush == 0) {
        if (lb->len == SPLITMUX_BUF) {         /* oversized line: drop + poison */
            lb->len = 0; lb->scan = 0; lb->poison = true;
        }
        return 0;
    }

    int result = 0;                            /* -1 fail; else 0 */
    bool became_ready = false;
    size_t start = 0;
    for (size_t i = 0; i < flush; i++) {
        if (lb->data[i] != '\n')
            continue;
        size_t linelen = i + 1 - start;
        if (lb->poison) {
            lb->poison = false;
        } else if (linelen < SPLITMUX_LINE) {
            char tmp[SPLITMUX_LINE];
            memcpy(tmp, lb->data + start, linelen);
            tmp[linelen] = '\0';
            if (sec_handshake_line(m, tmp) < 0) { result = -1; break; }
            if (!became_ready && m->sec_got_sub &&
                m->pool[p].last_notify_len > 0)
                became_ready = true;
        }
        start = i + 1;
    }
    memmove(lb->data, lb->data + flush, lb->len - flush);
    lb->len -= flush;
    lb->scan = lb->len;

    if (result < 0)
        return -1;
    return became_ready ? 1 : 0;
}

/* Bank the active pool's elapsed time since slice_start into its cumulative
 * total and restart the slice clock at t. Called at every slice boundary
 * (swap OR extend) so the ratio weighting sees real per-pool time. */
static void bank_slice(mux_t *m, int64_t t)
{
    int64_t slice = t - m->slice_start_us;
    if (slice < 0)
        slice = 0;
    if (m->active == 0)
        m->a_us += slice;
    else
        m->b_us += slice;
    m->slice_start_us = t;
}

/* Slice length for `pool` given the current hashrate estimate. */
static int64_t next_slice_us(mux_t *m, int pool)
{
    double active_us = (double)(m->a_us + m->b_us);
    double hr = 0.0;
    if (active_us > 0.0 && m->total_work > 0.0)
        hr = m->total_work / (active_us / 1e6);
    return split_sched_slice_us(hr, m->pool[pool].diff,
                                m->target_shares, m->min_s, m->max_s);
}

/* Perform the armed swap onto the target pool, on its clean notify. */
static int do_swap(mux_t *m, const char *notify_line, size_t nlen, int64_t t)
{
    int tgt = m->target;
    pool_t *tp = &m->pool[tgt];
    char buf[512];
    int n;

    n = sm_emit_set_extranonce(buf, sizeof buf - 1, tp->enonce1, tp->n2len);
    if (n > 0) {
        buf[n] = '\n';
        if (write_all(m->down_fd, buf, (size_t)n + 1) < 0) return -1;
    }
    n = sm_emit_set_difficulty(buf, sizeof buf - 1, tp->diff);
    if (n > 0) {
        buf[n] = '\n';
        if (write_all(m->down_fd, buf, (size_t)n + 1) < 0) return -1;
    }
    if (write_all(m->down_fd, notify_line, nlen) < 0)
        return -1;

    /* FIX-3: this notify's job is now shown to the miner — record it, tagged to
     * the target pool, so a later submit for it routes to the right pool. */
    stratum_msg_t nm;
    if (stratum_msg_parse(notify_line, &nm) == 0 && nm.type == SM_NOTIFY)
        ring_add(&m->ring, nm.job_id, tgt, t);

    /* FIX-2: remember when we left the departing pool so its stale-share grace
     * runs from the SWAP moment, not from that pool's last (possibly old) notify. */
    m->pool[m->active].left_us = t;

    /* Bank the time spent up to the swap against the pool we just left, flip. */
    bank_slice(m, t);
    m->active = tgt;
    m->pending = false;
    m->slice_deadline_us = t + next_slice_us(m, tgt);
    return 0;
}

/* Handle one parsed line from upstream pool `p`. Returns 0, or -1 on write
 * failure to the miner. */
static int handle_upstream_line(mux_t *m, int p, const char *line, size_t len)
{
    smx_dbg_line("RX", p, line, len);
    stratum_msg_t msg;
    if (stratum_msg_parse(line, &msg) != 0)
        return 0;
    int64_t t = now_us();

    if (msg.type == SM_SET_DIFFICULTY) {
        m->pool[p].diff = msg.diff;
        /* FIX-4: the ACTIVE pool's diff must always reach the miner (else it
         * mines an outdated diff and gets low-diff rejects). The target pool's
         * diff (p != active) is delivered by the swap's set_difficulty emit. */
        if (p == m->active)
            return write_all(m->down_fd, line, len);
        return 0;
    }

    if (msg.type == SM_NOTIFY) {
        snprintf(m->pool[p].cur_job, sizeof m->pool[p].cur_job, "%s",
                 msg.job_id);
        if (msg.clean_jobs)
            snprintf(m->pool[p].last_clean_job,
                     sizeof m->pool[p].last_clean_job, "%s", msg.job_id);
        /* FIX-5: keep the full latest notify so a swap can present the target's
         * CURRENT job immediately instead of waiting for its next clean notify. */
        if (len > 0 && len < sizeof m->pool[p].last_notify) {
            memcpy(m->pool[p].last_notify, line, len);
            m->pool[p].last_notify[len] = '\0';
            m->pool[p].last_notify_len = len;
        }

        /* FIX-5: an armed swap fires on the target's very next notify (any
         * clean flag) — no waiting for a rare clean_jobs==true. */
        if (m->pending && p == m->target)
            return do_swap(m, line, len, t);

        if (p == m->active) {
            /* FIX-3: record ONLY jobs actually shown to the miner, tagged with
             * the owning pool, so overlapping id namespaces cannot cross-route. */
            ring_add(&m->ring, msg.job_id, p, t);
            return write_all(m->down_fd, line, len);
        }

        /* non-active, non-target notifies are not shown to the miner */
        return 0;
    }

    if (msg.type == SM_RESULT) {
        /* FIX-1: drop only the mux's OWN sentinel-id handshake acks; relay any
         * other result (submit acks, and acks to post-handshake miner requests
         * like extranonce.subscribe / suggest_difficulty) to the miner verbatim.
         * Each submit goes to exactly one pool and miner submit ids are unique,
         * so no id translation is needed. */
        if (msg.id == SENTINEL_SUBSCRIBE || msg.id == SENTINEL_AUTHORIZE ||
            msg.id == SENTINEL_CONFIGURE)
            return 0;
        return write_all(m->down_fd, line, len);
    }

    /* other upstream chatter is not forwarded */
    return 0;
}

/* Route one miner submit to its owning pool (verbatim), or drop it. */
static int handle_submit(mux_t *m, const char *job_id, const char *line,
                         size_t len)
{
    int64_t t = now_us();
    int pool = m->active;               /* unknown job-id -> active fallback */
    int idx = ring_find(&m->ring, job_id);
    if (idx >= 0) {
        int p = m->ring.e[idx].pool;
        if (p == m->active) {
            pool = p;                   /* active pool always routes */
        } else if (t - m->pool[p].left_us <= GRACE_US) {
            /* FIX-2: grace runs from when the mux LEFT pool p (the swap moment),
             * not from that pool's last notify — real pools notify slowly, so a
             * valid share found just before a swap must still be routed. */
            pool = p;
        } else {
            return 0;                   /* left p longer ago than grace -> drop */
        }
    }
    smx_dbg("submit: job=%s routed -> pool %c%s\n", job_id,
            pool == 0 ? 'A' : 'B',
            (idx < 0) ? " (unknown job-id, active fallback)" : "");
    smx_dbg_line("TX", pool, line, len);
    if (write_all(m->pool[pool].fd, line, len) < 0)
        return -1;
    m->total_work += m->pool[pool].diff * 4294967296.0;
    return 0;
}

/* Extract every complete line accumulated in `lb` and dispatch it. `from_up`
 * selects the upstream-line vs miner-line handler; `p` is the pool for
 * upstream lines. Returns 0, or -1 on a write failure. */
static int process_lines(mux_t *m, linebuf_t *lb, bool from_up, int p)
{
    size_t flush = 0;
    for (size_t i = lb->scan; i < lb->len; i++)
        if (lb->data[i] == '\n')
            flush = i + 1;
    lb->scan = lb->len;

    if (flush == 0) {
        if (lb->len == SPLITMUX_BUF) {   /* oversized line: drop + poison tail */
            lb->len = 0;
            lb->scan = 0;
            lb->poison = true;
        }
        return 0;
    }

    int rc = 0;
    size_t start = 0;
    for (size_t i = 0; i < flush; i++) {
        if (lb->data[i] != '\n')
            continue;
        size_t linelen = i + 1 - start;
        if (lb->poison) {
            lb->poison = false;          /* this fragment closes the poison */
        } else if (linelen < SPLITMUX_LINE) {
            char tmp[SPLITMUX_LINE];
            memcpy(tmp, lb->data + start, linelen);
            tmp[linelen] = '\0';
            if (from_up) {
                if (handle_upstream_line(m, p, tmp, linelen) < 0) { rc = -1; break; }
            } else {
                stratum_msg_t msg;
                bool parsed = (stratum_msg_parse(tmp, &msg) == 0);
                if (parsed && msg.type == SM_SUBMIT) {
                    if (handle_submit(m, msg.job_id, tmp, linelen) < 0) { rc = -1; break; }
                } else {
                    /* M5: a post-authorize extranonce.subscribe still marks the
                     * miner as set_extranonce-capable (smooth-swap path). */
                    if (parsed && msg.type == SM_EXTRANONCE_SUBSCRIBE)
                        m->miner_ext_ok = true;
                    /* non-submit miner chatter -> active pool */
                    smx_dbg("miner chatter -> active pool %c\n",
                            m->active == 0 ? 'A' : 'B');
                    smx_dbg_line("TX", m->active, tmp, linelen);
                    if (write_all(m->pool[m->active].fd, tmp, linelen) < 0) { rc = -1; break; }
                }
            }
        } else {
            /* FIX-8: a complete but over-long-to-parse line (e.g. a big-coinbase
             * mining.notify). M3 passthrough forwarded such lines; do the same
             * here to its default destination rather than dropping it. */
            if (from_up) {
                if (p == m->active &&
                    write_all(m->down_fd, lb->data + start, linelen) < 0) { rc = -1; break; }
            } else {
                if (write_all(m->pool[m->active].fd, lb->data + start, linelen) < 0) { rc = -1; break; }
            }
        }
        start = i + 1;
    }

    memmove(lb->data, lb->data + flush, lb->len - flush);
    lb->len -= flush;
    lb->scan = lb->len;
    return rc;
}

static int lb_fill(int fd, linebuf_t *lb)
{
    if (lb->len == SPLITMUX_BUF)
        return 1;                        /* full; process_lines will drain it */
    ssize_t r = read(fd, lb->data + lb->len, SPLITMUX_BUF - lb->len);
    if (r < 0)
        return (errno == EINTR || errno == EAGAIN) ? 1 : -1;
    if (r == 0)
        return -1;
    lb->len += (size_t)r;
    return 1;
}

/* Handle a readable upstream fd `p`. For the secondary mid-bring-up this drives
 * the async state machine (and transitions it to READY); otherwise it is normal
 * upstream processing. Returns 0 to keep the session going, or -1 to END it
 * (fatal: the primary dropped, or the ACTIVE pool dropped). A NON-active
 * secondary dropping is recoverable — it just schedules a backoff retry while
 * the miner keeps mining the primary. */
static int handle_pool_readable(mux_t *m, int p)
{
    if (p == m->sec && m->sec_state == SEC_HANDSHAKING) {
        int sr = sec_pump(m);
        if (sr < 0) {
            sec_fail(m, now_us());
        } else if (sr == 1) {
            m->sec_state = SEC_READY;
            m->sec_backoff_s = SEC_BACKOFF_MIN;
            m->sec_degrade_logged = false;
            fprintf(stderr, "splitmux: secondary pool %c ready\n",
                    m->sec == 0 ? 'A' : 'B');
            /* drain any lines already buffered past the first notify */
            if (process_lines(m, &m->bup[p], true, p) < 0)
                return -1;
        }
        return 0;
    }
    if (lb_fill(m->pool[p].fd, &m->bup[p]) < 0) {
        if (p == m->sec && m->active != m->sec) {
            sec_fail(m, now_us());       /* non-active secondary died: retry it */
            return 0;
        }
        return -1;                       /* primary / active pool died: end */
    }
    if (process_lines(m, &m->bup[p], true, p) < 0)
        return -1;
    return 0;
}

static void splitmux_dual(mux_t *m)
{
    /* Primary pool: synchronous handshake, miner mines it immediately. */
    int primary = primary_handshake(m);
    if (primary < 0)
        return;                          /* miner left, or BOTH pools failed */
    m->sec = primary ^ 1;
    m->sec_backoff_s = SEC_BACKOFF_MIN;
    m->sec_degrade_logged = false;

    int64_t t0 = now_us();
    /* Secondary pool: kick off the async bring-up on its already-connected fd. */
    sec_begin_attempt(m, t0);

    m->slice_start_us = t0;
    m->slice_deadline_us = t0 + next_slice_us(m, m->active);

    /* Drain the set_difficulty + first notify the PRIMARY queued after authorize,
     * plus anything the miner pipelined (so a capable miner that then goes quiet
     * isn't misdetected as naive and needlessly reconnect-sliced). */
    if (process_lines(m, &m->bup[primary], true, primary) < 0) return;
    if (process_lines(m, &m->bmin, false, -1) < 0) return;

    for (;;) {
        int64_t t = now_us();
        /* Secondary retry timer: reconnect once the backoff elapses. */
        if (m->sec_state == SEC_WAIT && t >= m->sec_retry_at_us)
            sec_begin_attempt(m, t);

        struct pollfd pfds[3];
        pfds[0].fd = m->down_fd;    pfds[0].events = POLLIN; pfds[0].revents = 0;
        pfds[1].fd = m->pool[0].fd; pfds[1].events = POLLIN; pfds[1].revents = 0;
        pfds[2].fd = m->pool[1].fd; pfds[2].events = POLLIN; pfds[2].revents = 0;
        /* the secondary fd is closed/absent while it backs off */
        if (m->sec_state == SEC_WAIT)
            pfds[m->sec + 1].fd = -1;

        int rc = poll(pfds, 3, 200);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if ((pfds[0].revents | pfds[1].revents | pfds[2].revents) & POLLNVAL)
            break;

        /* Process all pending I/O FIRST, so any miner submit already buffered
         * for the current active pool is routed BEFORE a deadline swap flips the
         * active pool. */
        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (lb_fill(m->down_fd, &m->bmin) < 0) break;
            if (process_lines(m, &m->bmin, false, -1) < 0) break;
        }
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (handle_pool_readable(m, 0) < 0) break;
        }
        if (pfds[2].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (handle_pool_readable(m, 1) < 0) break;
        }

        /* Secondary per-attempt timeout (a pool that accepts TCP but never
         * answers won't fire the data path above). */
        if (m->sec_state == SEC_HANDSHAKING &&
            now_us() >= m->sec_attempt_deadline_us)
            sec_fail(m, now_us());

        /* Deadline check (fires on data or on the 200 ms timeout), AFTER the
         * buffered submits above have been routed under the old active pool. */
        t = now_us();
        if (!m->pending && t >= m->slice_deadline_us) {
            bank_slice(m, t);            /* bank the finished slice first */
            int tgt = split_sched_next_pool(m->active, m->a_us, m->b_us,
                                            m->ratio_a);
            if (tgt != m->active) {
                if (tgt == m->sec && m->sec_state != SEC_READY) {
                    /* Secondary not ready yet: don't swap onto it — extend the
                     * primary slice and keep mining the primary. */
                    m->slice_deadline_us = t + next_slice_us(m, m->active);
                } else if (!m->miner_ext_ok) {
                    /* M5 reconnect-slice fallback: a miner that never advertised
                     * set_extranonce support cannot follow a smooth swap. Drop it
                     * so it reconnects; the splitter alternates start_pool to bind
                     * it to the next pool. The caller owns and closes all fds. */
                    fprintf(stderr, "splitmux: fallback reconnect-slice "
                                    "(miner lacks set_extranonce)\n");
                    shutdown(m->down_fd, SHUT_RDWR);
                    return;
                } else {
                    m->target = tgt;
                    /* FIX-5: if the target has already sent a notify, swap NOW
                     * onto its current job (rewritten clean_jobs=true) instead of
                     * waiting for its next clean notify. */
                    if (m->pool[tgt].last_notify_len > 0) {
                        char nb[SPLITMUX_LINE];
                        size_t nl;
                        if (rewrite_notify_clean(m->pool[tgt].last_notify,
                                                 nb, sizeof nb, &nl) == 0) {
                            if (do_swap(m, nb, nl, t) < 0) break;
                        } else {
                            m->pending = true;
                        }
                    } else {
                        m->pending = true;   /* swap on its first notify */
                    }
                }
            } else {
                m->slice_deadline_us = t + next_slice_us(m, m->active);
            }
        }
    }
}

void splitmux_run(int down_fd, int up_fd[2], int ratio_a,
                  int target_shares, int min_s, int max_s, int start_pool,
                  const char *up_addr[2])
{
    if (up_fd[1] < 0) {
        /* Single-pool passthrough (M3 / fallback): scheduler knobs + up_addr
         * unused (no secondary to reconnect). */
        (void)ratio_a; (void)target_shares; (void)min_s; (void)max_s;
        (void)start_pool; (void)up_addr;
        splitmux_passthrough(down_fd, up_fd[0]);
        return;
    }

    mux_t m;
    memset(&m, 0, sizeof m);
    m.down_fd = down_fd;
    m.pool[0].fd = up_fd[0];
    m.pool[1].fd = up_fd[1];
    m.ratio_a = ratio_a;
    m.target_shares = target_shares;
    m.min_s = min_s;
    m.max_s = max_s;
    m.start_pool = start_pool;
    m.up_addr[0] = up_addr ? up_addr[0] : NULL;
    m.up_addr[1] = up_addr ? up_addr[1] : NULL;
    splitmux_dual(&m);

    /* The secondary fd may have been closed/reopened during retries — hand the
     * FINAL upstream fds back so the caller closes the right ones. A secondary
     * that is mid-backoff at return is -1; the caller skips it. */
    up_fd[0] = m.pool[0].fd;
    up_fd[1] = m.pool[1].fd;
}
