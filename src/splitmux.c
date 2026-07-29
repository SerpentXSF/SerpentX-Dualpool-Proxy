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

/* dual_handshake outcomes. DH_DEGRADED means exactly one pool failed its
 * subscribe and the handshake has already handed the miner off to a single-pool
 * relay on the survivor (D1b): the caller must NOT run the dual swap loop. */
#define DH_OK        0
#define DH_FAIL     (-1)
#define DH_DEGRADED  1

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

    linebuf_t bmin;              /* miner  -> mux */
    linebuf_t bup[2];            /* upstream[p] -> mux */
} mux_t;

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
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

/* D1b: exactly one pool's subscribe failed mid-handshake (it went workless/dead
 * between the splitter's readiness gate and here). Rather than drop the miner,
 * present the SURVIVING pool's session and relay verbatim to it for the rest of
 * this connection — never arming a swap. `surv` is the good pool (0/1); the
 * miner's subscribe id is `sub_id`. The miner mines a working pool instead of
 * reconnect-looping against a dead one; a future reconnect re-attempts the
 * split (self-healing). Returns DH_DEGRADED (relay ran to completion) or
 * DH_FAIL if even the survivor's reply/relay could not be written. */
static int single_pool_degrade(mux_t *m, int surv, int64_t sub_id)
{
    pool_t *ap = &m->pool[surv];
    /* enonce1 was hex-validated in read_subscribe_result, so it cannot break
     * the synthesized JSON here. */
    char rep[512];
    int rl = snprintf(rep, sizeof rep,
        "{\"id\":%lld,\"result\":[[[\"mining.set_difficulty\",\"%s\"],"
        "[\"mining.notify\",\"%s\"]],\"%s\",%d],\"error\":null}\n",
        (long long)sub_id, ap->enonce1, ap->enonce1, ap->enonce1, ap->n2len);
    if (rl < 0 || (size_t)rl >= sizeof rep ||
        write_all(m->down_fd, rep, (size_t)rl) < 0)
        return DH_FAIL;

    fprintf(stderr, "splitmux: single-pool degrade -> mining pool %c "
            "(other pool's handshake failed)\n", surv == 0 ? 'A' : 'B');

    /* The mux already drove its sentinel subscribe to the survivor and consumed
     * that reply; from here the miner's authorize/submits relay straight to the
     * survivor. Seed the relay with anything already buffered during handshake. */
    splitmux_relay_seeded(m->down_fd, ap->fd, &m->bmin, &m->bup[surv]);
    return DH_DEGRADED;
}

/* Drive subscribe + authorize to both pools and synthesize the miner session.
 * Returns DH_OK on the both-good dual path, DH_DEGRADED if exactly one pool
 * failed and the miner was handed to a single-pool relay on the survivor (D1b),
 * or DH_FAIL if the miner disconnected or both pools failed. */
static int dual_handshake(mux_t *m)
{
    char line[SPLITMUX_LINE];
    size_t ll;
    int64_t sub_id = 1;

    /* 1. miner subscribe (tolerate a leading mining.configure) */
    for (;;) {
        if (!blocking_line(m->down_fd, &m->bmin, line, sizeof line, &ll))
            return -1;
        stratum_msg_t msg;
        if (stratum_msg_parse(line, &msg) != 0)
            continue;
        if (msg.type == SM_CONFIGURE) {
            /* Drive our OWN configure to the pools under a sentinel id so the
             * pools' acks are recognizable (and dropped) rather than mistaken
             * for submit acks; synthesize the miner's ack with the miner's id. */
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

    /* 2. subscribe to both upstreams (sentinel id) */
    static const char sub[] =
        "{\"id\":90000001,\"method\":\"mining.subscribe\","
        "\"params\":[\"dualpool-mux/1.0\"]}\n";
    if (write_all(m->pool[0].fd, sub, sizeof sub - 1) < 0) return -1;
    if (write_all(m->pool[1].fd, sub, sizeof sub - 1) < 0) return -1;

    /* 3. capture each pool's extranonce1 / n2 size. D1b: capture each result
     * INDEPENDENTLY. If exactly ONE pool fails (timeout/EOF), degrade to a
     * single-pool relay on the survivor instead of dropping the miner; only if
     * BOTH fail do we give up. */
    bool okA = (read_subscribe_result(m, 0) == 0);
    bool okB = (read_subscribe_result(m, 1) == 0);
    if (!okA && !okB) return DH_FAIL;
    if (!okA) return single_pool_degrade(m, 1, sub_id);   /* A dead -> mine B */
    if (!okB) return single_pool_degrade(m, 0, sub_id);   /* B dead -> mine A */

    /* 4. seed the active pool: an explicit start_pool (0/1) wins so the splitter
     * can alternate reconnecting fallback miners; otherwise fall back to the
     * ratio (A when the ratio favours A). */
    if (m->start_pool == 0 || m->start_pool == 1)
        m->active = m->start_pool;
    else
        m->active = (m->ratio_a >= 50) ? 0 : 1;

    /* 5. reply to the miner with the active pool's session params */
    pool_t *ap = &m->pool[m->active];
    char rep[512];
    /* enonce1 was hex-validated in read_subscribe_result, so it cannot break
     * the JSON here; range-check the format anyway (FIX-10). */
    int rl = snprintf(rep, sizeof rep,
        "{\"id\":%lld,\"result\":[[[\"mining.set_difficulty\",\"%s\"],"
        "[\"mining.notify\",\"%s\"]],\"%s\",%d],\"error\":null}\n",
        (long long)sub_id, ap->enonce1, ap->enonce1, ap->enonce1, ap->n2len);
    if (rl < 0 || (size_t)rl >= sizeof rep || write_all(m->down_fd, rep, (size_t)rl) < 0)
        return -1;

    /* 6. miner authorize (tolerate a mining.configure here too) */
    int64_t auth_id = 2;
    for (;;) {
        if (!blocking_line(m->down_fd, &m->bmin, line, sizeof line, &ll))
            return -1;
        stratum_msg_t msg;
        if (stratum_msg_parse(line, &msg) != 0)
            continue;
        if (msg.type == SM_CONFIGURE) {
            if (forward_with_id(m->pool[0].fd, line, SENTINEL_CONFIGURE) < 0) return -1;
            if (forward_with_id(m->pool[1].fd, line, SENTINEL_CONFIGURE) < 0) return -1;
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
            /* M5: the miner advertises set_extranonce support here (common:
             * right after subscribe, before authorize). Record capability and
             * forward to the active pool as normal chatter so its ack relays
             * back to the miner (FIX-1). Keep waiting for the authorize. */
            m->miner_ext_ok = true;
            if (write_all(m->pool[m->active].fd, line, ll) < 0)
                return -1;
            continue;
        }
        if (msg.type == SM_AUTHORIZE) {
            auth_id = msg.id;
            /* Send the mux's OWN mining.authorize (sentinel id) to both pools,
             * carrying the worker/password the miner supplied. Building via
             * jansson keeps worker/pass safe from JSON injection; the sentinel
             * id lets us drop the pools' acks instead of relaying them. */
            const char *pw = "";
            json_error_t je;
            json_t *mj = json_loads(line, 0, &je);
            if (mj) {
                json_t *mp = json_object_get(mj, "params");
                const char *jp = json_string_value(json_array_get(mp, 1));
                if (jp) pw = jp;
            }
            json_t *aj = json_pack("{s:I,s:s,s:[s,s]}",
                "id", (json_int_t)SENTINEL_AUTHORIZE,
                "method", "mining.authorize",
                "params", msg.worker, pw);
            char *ad = aj ? json_dumps(aj, JSON_COMPACT) : NULL;
            if (mj) json_decref(mj);
            if (aj) json_decref(aj);
            if (!ad) return -1;
            int arc = write_all(m->pool[0].fd, ad, strlen(ad));
            if (arc == 0) arc = write_all(m->pool[0].fd, "\n", 1);
            if (arc == 0) arc = write_all(m->pool[1].fd, ad, strlen(ad));
            if (arc == 0) arc = write_all(m->pool[1].fd, "\n", 1);
            free(ad);
            if (arc < 0) return -1;
            break;
        }
    }

    /* 7. reply true to the miner */
    char ar[128];
    int al = snprintf(ar, sizeof ar,
        "{\"id\":%lld,\"result\":true,\"error\":null}\n", (long long)auth_id);
    if (al < 0 || (size_t)al >= sizeof ar || write_all(m->down_fd, ar, (size_t)al) < 0)
        return -1;
    return 0;
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

static void splitmux_dual(mux_t *m)
{
    /* DH_OK -> run the swap loop; DH_FAIL -> give up; DH_DEGRADED -> the
     * handshake already handed the miner to a single-pool relay on the survivor
     * (D1b) and it has now returned, so we're done. */
    if (dual_handshake(m) != DH_OK)
        return;

    m->slice_start_us = now_us();
    m->slice_deadline_us = m->slice_start_us + next_slice_us(m, m->active);

    /* Drain the set_difficulty + first notify the pools queued after authorize
     * (they may already be buffered from the handshake reads). */
    if (process_lines(m, &m->bup[0], true, 0) < 0) return;
    if (process_lines(m, &m->bup[1], true, 1) < 0) return;
    /* Also drain anything the miner pipelined after authorize (e.g. a
     * mining.extranonce.subscribe in the same TCP segment): otherwise a capable
     * miner that then stays silent through the first slice would be misdetected
     * as naive and needlessly reconnect-sliced. */
    if (process_lines(m, &m->bmin, false, -1) < 0) return;

    for (;;) {
        struct pollfd pfds[3];
        pfds[0].fd = m->down_fd;    pfds[0].events = POLLIN; pfds[0].revents = 0;
        pfds[1].fd = m->pool[0].fd; pfds[1].events = POLLIN; pfds[1].revents = 0;
        pfds[2].fd = m->pool[1].fd; pfds[2].events = POLLIN; pfds[2].revents = 0;

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
         * active pool. (Swapping first would misroute an in-flight old-job
         * submit to the newly-active pool — visible only under a shared job-id
         * namespace, where the two pools' job strings collide.) */
        if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (lb_fill(m->down_fd, &m->bmin) < 0) break;
            if (process_lines(m, &m->bmin, false, -1) < 0) break;
        }
        if (pfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (lb_fill(m->pool[0].fd, &m->bup[0]) < 0) break;
            if (process_lines(m, &m->bup[0], true, 0) < 0) break;
        }
        if (pfds[2].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (lb_fill(m->pool[1].fd, &m->bup[1]) < 0) break;
            if (process_lines(m, &m->bup[1], true, 1) < 0) break;
        }

        /* Deadline check (fires on data or on the 200 ms timeout), AFTER the
         * buffered submits above have been routed under the old active pool. */
        int64_t t = now_us();
        if (!m->pending && t >= m->slice_deadline_us) {
            bank_slice(m, t);            /* bank the finished slice first */
            int tgt = split_sched_next_pool(m->active, m->a_us, m->b_us,
                                            m->ratio_a);
            if (tgt != m->active) {
                /* M5 reconnect-slice fallback: a miner that never advertised
                 * set_extranonce support cannot follow a smooth swap (it would
                 * keep the old pool's extranonce1 while mining the new pool's
                 * jobs -> 100% rejects). Instead of swapping, drop the miner so
                 * it reconnects; the splitter (M6.2) alternates start_pool to
                 * bind it to the next pool. We mined only the active upstream on
                 * this connection; the caller owns and closes all fds. */
                if (!m->miner_ext_ok) {
                    fprintf(stderr, "splitmux: fallback reconnect-slice "
                                    "(miner lacks set_extranonce)\n");
                    shutdown(m->down_fd, SHUT_RDWR);
                    return;
                }
                m->target = tgt;
                /* FIX-5: if the target has already sent a notify, swap NOW onto
                 * its current job (rewritten clean_jobs=true) instead of waiting
                 * for its next clean notify (which real pools issue ~per block).
                 * Only if it has never notified do we arm pending. */
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
                    m->pending = true;   /* no notify yet: swap on its first one */
                }
            } else {
                m->slice_deadline_us = t + next_slice_us(m, m->active);
            }
        }
    }
}

void splitmux_run(int down_fd, int up_fd[2], int ratio_a,
                  int target_shares, int min_s, int max_s, int start_pool)
{
    if (up_fd[1] < 0) {
        /* Single-pool passthrough (M3 / fallback): scheduler knobs unused. */
        (void)ratio_a; (void)target_shares; (void)min_s; (void)max_s;
        (void)start_pool;
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
    splitmux_dual(&m);
}
