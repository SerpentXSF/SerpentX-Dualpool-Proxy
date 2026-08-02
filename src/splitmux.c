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
#define SPLITMUX_PEND  64          /* routed submits awaiting a pool ack */
#define GRACE_US       30000000LL  /* stale-share grace after leaving a pool */
#define HS_TIMEOUT_MS  5000        /* per-read handshake timeout (abort dead pool) */

/* The mux drives its OWN handshake to the pools with sentinel JSON-RPC ids the
 * miner will never use for submits. A pool's reply carrying one of these is the
 * mux's own handshake ack and is dropped; any other result is a submit ack (or
 * ack to a post-handshake miner request) and is relayed to the miner verbatim. */
#define SENTINEL_SUBSCRIBE  90000001LL
#define SENTINEL_AUTHORIZE  90000002LL
#define SENTINEL_CONFIGURE  90000003LL
#define SENTINEL_SUGGEST    90000004LL   /* mux's own mining.suggest_difficulty */

/* Proactive mining.suggest_difficulty (see mux_suggest_check).
 *
 * A pool that opens a session at a huge difficulty (Kryptex opens at 1,000,000)
 * starves a miner that does not send its own suggest_difficulty: at 1,000,000 a
 * 12.9 TH/s miner finds a share only every ~5.5 min, so the pool's vardiff gets
 * almost no samples to ramp down with, the miner sees almost no accepted shares,
 * and its watchdog eventually drops the session and reconnects — forever. The
 * mux measures the miner's rate anyway (total_work / active time), so it can tell
 * the pool how big this miner is on the miner's behalf. */
#define SUGGEST_TARGET_S        15.0        /* aim for ~one share every N seconds */
#define SUGGEST_DIFF_MIN        1024.0      /* clamp: never suggest below this */
#define SUGGEST_DIFF_MAX        4194304.0   /* clamp: never suggest above this */
#define SUGGEST_CLOSE           2.0         /* within this factor: leave the pool alone */
#define SUGGEST_MISMATCH        4.0         /* beyond this factor: send one suggestion */
#define SUGGEST_MIN_INTERVAL_US 120000000LL /* at most one per pool per 120 s */
#define SUGGEST_MIN_SAMPLES     4           /* shares needed before suggesting UP */

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
    char   last_shown_job[64];          /* FIX-11: newest job actually PRESENTED to
                                         * the miner for this pool (swap or active
                                         * notify). A swap-back never re-presents an
                                         * already-shown job as forced-clean — that
                                         * would make the miner flush + re-mine the
                                         * SAME enonce2 space, i.e. duplicate shares
                                         * at a low-churn (solo) pool. */
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
     * false => reconnect-slice fallback at the deadline.
     * Pre-seeded true by the assume_extranonce operator opt-in (see splitmux.h),
     * which asserts the fleet honours set_extranonce without advertising it. */
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

    /* ASICBoost version-rolling negotiation (mining.configure / BIP310).
     * The miner negotiates a version-rolling mask with the PRIMARY pool; the mux
     * relays the pool's REAL granted mask back to the miner (not {}). The
     * SECONDARY negotiates the same configure; if it grants a narrower mask the
     * mux tells the miner mining.set_version_mask with the AND-intersection so a
     * rolled share is valid on BOTH pools. */
    bool        miner_vroll;         /* miner sent mining.configure (version-rolling) */
    uint32_t    miner_vmask_req;     /* mask the miner requested */
    char        miner_cfg[SPLITMUX_LINE]; /* raw miner configure line (replayed to sec) */
    /* Raw mining.suggest_difficulty from the miner, replayed to the secondary when
     * it comes up so BOTH pools size this miner the same way (a secondary that
     * misses it sits at its default difficulty and wrecks the first swap). */
    char        miner_sugg[512];
    size_t      miner_sugg_len;
    uint64_t    routed_shares;   /* submits routed so far — the estimate's sample count */
    bool        vmask_active;        /* version-rolling was granted to the miner */
    uint32_t    vmask;               /* mask currently advertised to the miner */
    int         vmask_pool;          /* pool whose grant the miner was told (-1 none) */
    bool        sec_vroll_seen;      /* secondary returned a configure result */
    uint32_t    sec_vmask;           /* secondary's granted mask (0 = declined) */
    bool        sec_vmask_applied;   /* reconciled sec grant into set_version_mask */

    /* Difficulty presented to the miner: always the ACTIVE pool's own value (see
     * emit_miner_diff). Telling it max(diffA, diffB) instead was tried and
     * REVERTED — it does remove the "Above target" rejects that a difficulty
     * change strands in-flight work into, but a pool credits a share at the
     * difficulty IT assigned, so mining at the higher pool's difficulty silently
     * throws away the difference on every share routed to the lower one. Measured
     * live: ~31% of credited hashrate lost. Per-pool difficulty credits every
     * share in full and costs only the few shares in flight across a change. */
    double      miner_diff;      /* what the miner was last told (0 = nothing yet) */

    /* Share accounting: a routed submit is pending until its owning pool acks it.
     * The ring correlates the ack (by JSON-RPC id) back to the pool it was routed
     * to and the difficulty it was submitted under, so the callback can attribute
     * the share to the right pool. Oldest entry is overwritten on overflow. */
    struct {
        int64_t id;
        int     pool;
        double  diff;
        bool    used;
    }           pend[SPLITMUX_PEND];
    int         pend_head;
    splitmux_share_cb share_cb;
    void       *share_ctx;

    /* Proactive mining.suggest_difficulty rate limiting: when the mux last sent a
     * suggestion of its own to each pool (0 = never). Reset for a pool whose
     * session is re-established, since a fresh session starts back at that pool's
     * default difficulty. */
    int64_t     sugg_last_us[2];

    linebuf_t bmin;              /* miner  -> mux */
    linebuf_t bup[2];            /* upstream[p] -> mux */
} mux_t;

/* Defined further down (it needs the emit helpers): tell the miner `pool`'s
 * difficulty if that differs from what it was last told. Declared here because
 * both the swap path and the upstream set_difficulty handler call it. */
static int emit_miner_diff(mux_t *m, int pool);

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

/* ---- ASICBoost version-rolling (mining.configure) helpers ---------------- */

/* Test-only negative control (SPLITMUX_VROLL_OFF=1, one cached getenv): revert
 * to the OLD broken behaviour — answer the miner's mining.configure with {} and
 * never relay the pool's granted mask. Lets one binary demonstrate the RED
 * (rejects) vs GREEN (accepted) split-vroll behaviour. Unset in production. */
static int smx_vroll_off(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("SPLITMUX_VROLL_OFF");
        /* Treat unset, empty, and "0" all as OFF-disabled (so an empty export
         * from a test wrapper does not accidentally revert the relay). */
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached;
}

/* Test-only negative control (SPLITMUX_SWAP_REPRESENT=1, one cached getenv):
 * revert FIX-11 to the OLD behaviour — on every swap-back force-clean re-present
 * the target's CURRENT job even when the miner was already shown it, and fire an
 * armed swap on any target notify regardless of whether the job is new. This
 * reproduces the RED (duplicate-share) swap behaviour so one binary can show
 * RED (rejects) vs GREEN (no dupes). Unset in production. */
static int smx_swap_represent(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("SPLITMUX_SWAP_REPRESENT");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached;
}

/* Test-only negative control (SPLITMUX_NO_SUGGEST=1, one cached getenv): never
 * send the mux's OWN mining.suggest_difficulty, i.e. the pre-fix behaviour where
 * a pool that opens at a huge difficulty is left there. Lets one binary show RED
 * (pool stuck at its opening difficulty, ~no shares) vs GREEN (pool comes down).
 * Unset in production. Note this does NOT touch the relay/replay of a suggestion
 * the MINER sent — only the mux's own. */
static int smx_no_suggest(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("SPLITMUX_NO_SUGGEST");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached;
}

/* Seconds-per-share the suggested difficulty aims for. SUGGEST_TARGET_S (15 s)
 * in production; SPLITMUX_SUGGEST_TARGET_S (test-only, one cached getenv, range
 * [0.1, 600]) compresses that time constant so an integration test can reproduce
 * the "pool opened far too high" failure in seconds instead of the ~4 minutes the
 * real 15 s constant implies at a 16x mismatch. Unset in production. */
static double smx_suggest_target_s(void)
{
    static double cached = -1.0;
    if (cached < 0.0) {
        cached = SUGGEST_TARGET_S;
        const char *v = getenv("SPLITMUX_SUGGEST_TARGET_S");
        if (v && v[0]) {
            double d = strtod(v, NULL);
            if (d >= 0.1 && d <= 600.0)
                cached = d;
        }
    }
    return cached;
}

/* Parse a version-rolling mask "1fffe000" (hex, optional 0x) into *out. Returns
 * true iff the whole string is valid hex. */
static bool vmask_parse(const char *hex, uint32_t *out)
{
    if (!hex || !*hex)
        return false;
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
        hex += 2;
    if (!*hex)
        return false;
    char *end = NULL;
    unsigned long v = strtoul(hex, &end, 16);
    if (!end || *end)
        return false;
    *out = (uint32_t)v;
    return true;
}

/* Parse the miner's mining.configure request. params = [ [ext...], {args} ].
 * Returns true iff "version-rolling" is requested; sets *reqmask to the
 * requested mask (0xffffffff if the extension is present without an explicit
 * mask). */
static bool cfg_request_parse(const char *line, uint32_t *reqmask)
{
    *reqmask = 0xffffffffu;
    json_error_t e;
    json_t *r = json_loads(line, 0, &e);
    if (!r)
        return false;
    bool wants = false;
    json_t *params = json_object_get(r, "params");
    json_t *exts = json_array_get(params, 0);
    if (json_is_array(exts)) {
        size_t i, n = json_array_size(exts);
        for (i = 0; i < n; i++) {
            const char *s = json_string_value(json_array_get(exts, i));
            if (s && !strcmp(s, "version-rolling"))
                wants = true;
        }
    }
    json_t *args = json_array_get(params, 1);
    if (json_is_object(args)) {
        const char *mv =
            json_string_value(json_object_get(args, "version-rolling.mask"));
        uint32_t m;
        if (mv && vmask_parse(mv, &m))
            *reqmask = m;
    }
    json_decref(r);
    return wants;
}

/* Parse a pool's mining.configure RESULT. result = {"version-rolling":true,
 * "version-rolling.mask":"1fffe000"}. Returns true iff version-rolling was
 * granted; sets *mask to the granted mask (0 if granted without a mask). */
static bool cfg_result_parse(const char *line, uint32_t *mask)
{
    *mask = 0;
    json_error_t e;
    json_t *r = json_loads(line, 0, &e);
    if (!r)
        return false;
    bool granted = false;
    json_t *res = json_object_get(r, "result");
    if (json_is_object(res)) {
        granted = json_is_true(json_object_get(res, "version-rolling"));
        const char *mv =
            json_string_value(json_object_get(res, "version-rolling.mask"));
        uint32_t m;
        if (mv && vmask_parse(mv, &m))
            *mask = m;
    }
    json_decref(r);
    return granted;
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

/* Answer the miner's mining.configure. granted => relay the real version-rolling
 * mask; else {} (miner won't roll). Returns 0, or -1 on a miner write failure. */
static int reply_configure_miner(mux_t *m, int64_t id, bool granted, uint32_t mask)
{
    char rep[256];
    int rl;
    if (granted)
        rl = snprintf(rep, sizeof rep,
            "{\"id\":%lld,\"result\":{\"version-rolling\":true,"
            "\"version-rolling.mask\":\"%08x\"},\"error\":null}\n",
            (long long)id, (unsigned)mask);
    else
        rl = snprintf(rep, sizeof rep,
            "{\"id\":%lld,\"result\":{},\"error\":null}\n", (long long)id);
    if (rl < 0 || (size_t)rl >= sizeof rep)
        return -1;
    if (write_all(m->down_fd, rep, (size_t)rl) < 0)
        return -1;
    smx_dbg_line("TX", -1, rep, (size_t)rl);
    return 0;
}

/* Synchronously negotiate version-rolling with pool p: forward the miner's
 * mining.configure under the sentinel id and read the pool's configure RESULT
 * (tolerating an interleaved early set_difficulty/notify, which is captured).
 * On a sentinel result sets *granted / *mask and returns 0; returns -1 if the
 * pool dropped or never answered (timeout). */
static int negotiate_configure_sync(mux_t *m, int p, const char *miner_line,
                                    bool *granted, uint32_t *mask)
{
    *granted = false;
    *mask = 0;
    smx_dbg("configure: negotiating version-rolling with pool %c (sentinel id)\n",
            p == 0 ? 'A' : 'B');
    if (forward_with_id(m->pool[p].fd, miner_line, SENTINEL_CONFIGURE) < 0)
        return -1;
    char line[SPLITMUX_LINE];
    size_t ll;
    for (;;) {
        if (!blocking_line(m->pool[p].fd, &m->bup[p], line, sizeof line, &ll))
            return -1;                       /* EOF / handshake timeout */
        smx_dbg_line("RX", p, line, ll);
        json_error_t e;
        json_t *r = json_loads(line, 0, &e);
        if (!r)
            continue;
        json_t *idj = json_object_get(r, "id");
        bool is_cfg = idj && json_is_integer(idj) &&
                      json_integer_value(idj) == SENTINEL_CONFIGURE;
        json_decref(r);
        if (is_cfg) {
            *granted = cfg_result_parse(line, mask);
            smx_dbg("configure: pool %c %s mask=%08x\n", p == 0 ? 'A' : 'B',
                    *granted ? "granted version-rolling" : "declined version-rolling",
                    (unsigned)*mask);
            return 0;
        }
        /* not our configure ack — an early set_difficulty/notify: keep its state */
        pool_capture_state(m, p, line);
    }
}

/* Intersect the miner's current version-rolling mask with `pool_mask`; if the
 * pool covers less than what the miner already has, narrow the miner with a
 * mining.set_version_mask (so a rolled share stays valid on BOTH pools) and
 * adopt the intersection as the new advertised mask. No-op if version-rolling
 * was never granted or the pool already covers the miner's mask. Returns 0, or
 * -1 on a miner write failure. */
static int reconcile_mask(mux_t *m, uint32_t pool_mask)
{
    if (!m->vmask_active)
        return 0;
    uint32_t inter = m->vmask & pool_mask;
    if (inter == m->vmask)
        return 0;                            /* pool covers all miner bits: nothing */
    char buf[128];
    int n = sm_emit_set_version_mask(buf, sizeof buf - 1, inter);
    if (n <= 0)
        return 0;
    buf[n] = '\n';
    smx_dbg("vroll: narrowing miner mask %08x -> %08x (mining.set_version_mask)\n",
            (unsigned)m->vmask, (unsigned)inter);
    smx_dbg_line("TX", -1, buf, (size_t)n + 1);
    if (write_all(m->down_fd, buf, (size_t)n + 1) < 0)
        return -1;
    m->vmask = inter;
    return 0;
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
            /* Capture the miner's version-rolling request + the raw line (later
             * replayed to the secondary during its async bring-up). */
            m->miner_vroll = cfg_request_parse(line, &m->miner_vmask_req);
            snprintf(m->miner_cfg, sizeof m->miner_cfg, "%s", line);

            if (smx_vroll_off() || !m->miner_vroll) {
                /* Test negative control, or a configure with no version-rolling:
                 * forward to BOTH pools (sentinel) and answer {} as before. */
                smx_dbg("primary: configure relay OFF/none — forwarding to both "
                        "CONNECTED pools, answering miner {}\n");
                /* Only forward to a pool that is actually connected. A pool whose
                 * ckproxy was still warming when the miner arrived is legitimately
                 * fd -1 here (the async secondary dials it later), and writing to
                 * -1 would fail the handshake and drop the miner — which for a
                 * miner that does NOT request version-rolling would repeat for the
                 * whole outage, mining nothing while one pool is down. */
                for (int q = 0; q < 2; q++) {
                    if (m->pool[q].fd >= 0 &&
                        forward_with_id(m->pool[q].fd, line, SENTINEL_CONFIGURE) < 0)
                        return -1;
                }
                if (reply_configure_miner(m, msg.id, false, 0) < 0) return -1;
                continue;
            }

            /* Negotiate version-rolling with the PREFERRED (likely primary) pool
             * and relay its REAL granted mask to the miner. The secondary
             * negotiates its own during async bring-up (reconciled later via
             * mining.set_version_mask). */
            bool granted = false;
            uint32_t gmask = 0;
            if (negotiate_configure_sync(m, pref, line, &granted, &gmask) == 0 &&
                granted) {
                m->vmask_active = true;
                m->vmask = gmask;
                m->vmask_pool = pref;
                smx_dbg("primary: relaying pool %c granted mask %08x to miner\n",
                        pref == 0 ? 'A' : 'B', (unsigned)gmask);
                if (reply_configure_miner(m, msg.id, true, gmask) < 0) return -1;
            } else {
                /* pool declined or never answered: miner won't roll (still works,
                 * just no ASICBoost). */
                m->vmask_pool = -1;
                if (reply_configure_miner(m, msg.id, false, 0) < 0) return -1;
            }
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

    /* If we fell back to a primary OTHER than the pool we negotiated the miner's
     * version-rolling mask with, negotiate with the real primary and reconcile
     * (narrow the miner if the primary grants less). */
    if (m->vmask_active && !smx_vroll_off() && m->vmask_pool != primary) {
        bool g2 = false;
        uint32_t m2 = 0;
        if (negotiate_configure_sync(m, primary, m->miner_cfg, &g2, &m2) == 0) {
            if (reconcile_mask(m, g2 ? m2 : 0) < 0)
                return -1;
            m->vmask_pool = primary;
        } else {
            /* N2 fail-safe: the real primary never answered configure (timeout).
             * The miner is currently rolling the mask a DIFFERENT pool granted, so
             * narrow it to 0 (stop rolling) rather than submit a mask this pool
             * never granted (which it would reject 100%). */
            if (reconcile_mask(m, 0) < 0)
                return -1;
            m->vmask_pool = primary;
        }
    }

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
            /* A configure arriving AFTER subscribe (uncommon, but tolerated).
             * The primary is known now, so negotiate directly with it. */
            m->miner_vroll = cfg_request_parse(line, &m->miner_vmask_req);
            snprintf(m->miner_cfg, sizeof m->miner_cfg, "%s", line);
            if (smx_vroll_off() || !m->miner_vroll) {
                if (forward_with_id(m->pool[primary].fd, line, SENTINEL_CONFIGURE) < 0) return -1;
                if (reply_configure_miner(m, msg.id, false, 0) < 0) return -1;
                continue;
            }
            bool granted = false;
            uint32_t gmask = 0;
            if (negotiate_configure_sync(m, primary, line, &granted, &gmask) == 0 &&
                granted) {
                m->vmask_active = true;
                m->vmask = gmask;
                m->vmask_pool = primary;
                if (reply_configure_miner(m, msg.id, true, gmask) < 0) return -1;
            } else {
                m->vmask_pool = -1;
                if (reply_configure_miner(m, msg.id, false, 0) < 0) return -1;
            }
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

/* ---- proactive mining.suggest_difficulty --------------------------------- */

/* Current hashrate estimate in H/s, or 0.0 when there is not enough data yet.
 *
 * total_work is the sum of diff * 2^32 over every submit the mux ROUTED, so
 * work / active-seconds is this miner's rate. a_us/b_us only carry time BANKED at
 * slice boundaries, so the IN-PROGRESS slice has to be added explicitly: without
 * it the estimate reads 0 for the whole first slice, and the first slice is
 * exactly the window in which a pool that opened at 1,000,000 is starving the
 * miner. (next_slice_us deliberately keeps its own banked-only figure — it is
 * part of the slice sizer and is left byte-for-byte as it was.)
 *
 * Between shares the numerator is fixed while the denominator grows, so the
 * estimate DECAYS towards the next share — it errs low, which suggests an easier
 * difficulty, which is the safe direction. */
static double mux_hashrate(const mux_t *m, int64_t t)
{
    if (m->total_work <= 0.0)
        return 0.0;
    double active_us = (double)(m->a_us + m->b_us);
    if (m->slice_start_us > 0 && t > m->slice_start_us)
        active_us += (double)(t - m->slice_start_us);   /* unbanked, in progress */
    if (active_us <= 0.0)
        return 0.0;
    return m->total_work / (active_us / 1e6);
}

/* Difficulty that yields roughly one share every smx_suggest_target_s() seconds
 * at `hr` H/s: diff = hr * target_s / 2^32. Clamped to [SUGGEST_DIFF_MIN,
 * SUGGEST_DIFF_MAX] and rounded to the nearest power of two (pools snap to powers
 * of two anyway, and rounding stops a drifting estimate from producing a slightly
 * different suggestion every time). Returns 0.0 for a zero/absent estimate. */
static double suggest_target_diff(double hr)
{
    if (hr <= 0.0)
        return 0.0;
    double d = hr * smx_suggest_target_s() / 4294967296.0;
    if (d < SUGGEST_DIFF_MIN) d = SUGGEST_DIFF_MIN;
    if (d > SUGGEST_DIFF_MAX) d = SUGGEST_DIFF_MAX;
    /* Nearest power of two on a log scale: step up while d is above the
     * geometric midpoint (p * sqrt(2)) of the current step and the next. */
    double p = SUGGEST_DIFF_MIN;
    while (p * 2.0 <= SUGGEST_DIFF_MAX && p * 1.4142135623730951 < d)
        p *= 2.0;
    return p;
}

/* Send the mux's OWN mining.suggest_difficulty to pool p under SENTINEL_SUGGEST
 * (handle_upstream_line drops that ack, so it never reaches the miner or the
 * share-accounting ring). Records the send for rate limiting. Returns 0, or -1 if
 * the pool's socket died. */
static int suggest_send(mux_t *m, int p, double diff, int64_t t, const char *why)
{
    if (m->pool[p].fd < 0 || diff <= 0.0)
        return 0;
    char buf[192];
    int n = sm_emit_suggest_difficulty(buf, sizeof buf - 1, SENTINEL_SUGGEST, diff);
    if (n <= 0)
        return 0;
    buf[n] = '\n';
    m->sugg_last_us[p] = t;
    smx_dbg("suggest: pool %c (%s) estimate %.2f TH/s -> suggesting difficulty "
            "%.0f (pool is at %.0f)\n", p == 0 ? 'A' : 'B', why,
            mux_hashrate(m, t) / 1e12, diff, m->pool[p].diff);
    smx_dbg_line("TX", p, buf, (size_t)n + 1);
    return write_all(m->pool[p].fd, buf, (size_t)n + 1);
}

/* Periodic check (driven from the poll loop): suggest a sane difficulty to any
 * pool whose current difficulty is badly mismatched to the measured rate.
 *
 * Deliberately timid — this is a hint sent on the miner's behalf, not a control
 * loop:
 *   - a miner that sends its OWN suggest_difficulty is never overridden (that one
 *     is already relayed to both pools and replayed to a late secondary);
 *   - a pool already within SUGGEST_CLOSE (2x) of target is left alone;
 *   - only a mismatch beyond SUGGEST_MISMATCH (4x, either direction) is worth a
 *     word, and then at most one per pool per SUGGEST_MIN_INTERVAL_US.
 * Vardiff does the rest; the mux only breaks the deadlock where the pool cannot
 * ramp because the miner cannot find shares fast enough to give it samples. */
static void mux_suggest_check(mux_t *m, int64_t t)
{
    if (smx_no_suggest() || m->miner_sugg_len > 0)
        return;
    double want = suggest_target_diff(mux_hashrate(m, t));
    if (want <= 0.0)
        return;                       /* no estimate yet: nothing honest to say */
    for (int p = 0; p < 2; p++) {
        if (m->pool[p].fd < 0)
            continue;
        if (p == m->sec && m->sec_state != SEC_READY)
            continue;                 /* mid-bring-up: sec_send_handshake covers it */
        double d = m->pool[p].diff;
        if (d <= 0.0)
            continue;                 /* this pool has not stated a difficulty yet */
        if (d <= want * SUGGEST_CLOSE && d * SUGGEST_CLOSE >= want)
            continue;                 /* within 2x: close enough, say nothing */
        if (d <= want * SUGGEST_MISMATCH && d * SUGGEST_MISMATCH >= want)
            continue;                 /* off, but not badly: leave it to vardiff */
        if (m->sugg_last_us[p] != 0 &&
            t - m->sugg_last_us[p] < SUGGEST_MIN_INTERVAL_US)
            continue;                 /* rate limit: never spam a pool */
        /* Asking a pool to make work HARDER needs evidence. Share timing is
         * Poisson, so one lucky early share reads several times the true rate —
         * acting on it would raise the difficulty, starve the miner, and the rate
         * limit would then pin it there for two minutes: exactly the starvation
         * this feature exists to prevent. Requiring a few samples costs nothing,
         * because the case that actually matters (a pool opening far too HIGH)
         * is a downward suggestion and stays first-sample responsive. */
        if (want > d && m->routed_shares < SUGGEST_MIN_SAMPLES) {
            smx_dbg("suggest: pool %c would go UP (%.0f -> %.0f) but only %llu "
                    "share(s) sampled — waiting for %d\n", p == 0 ? 'A' : 'B',
                    d, want, (unsigned long long)m->routed_shares,
                    SUGGEST_MIN_SAMPLES);
            continue;
        }
        (void)suggest_send(m, p, want, t, "difficulty mismatched");
    }
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
    smx_dbg("secondary(%c): sending pipelined %sconfigure+subscribe+authorize\n",
            m->sec == 0 ? 'A' : 'B',
            (m->miner_vroll && !smx_vroll_off()) ? "" : "(no-configure) ");
    /* If the miner negotiated version-rolling, replay its mining.configure to
     * the secondary too (sentinel id) so we learn THIS pool's granted mask and
     * can reconcile it against the primary's via mining.set_version_mask. */
    if (m->miner_vroll && !smx_vroll_off() && m->miner_cfg[0]) {
        smx_dbg_line("TX", m->sec, m->miner_cfg, strlen(m->miner_cfg));
        if (forward_with_id(fd, m->miner_cfg, SENTINEL_CONFIGURE) < 0)
            return -1;
    }
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
    /* Replay the miner's suggest_difficulty so this pool sizes it like the other
     * one. Without it the secondary keeps its default difficulty and the first
     * swap onto it hands the miner work orders of magnitude too hard. */
    if (rc == 0 && m->miner_sugg_len > 0) {
        smx_dbg("secondary(%c): replaying miner suggest_difficulty\n",
                m->sec == 0 ? 'A' : 'B');
        smx_dbg_line("TX", m->sec, m->miner_sugg, m->miner_sugg_len);
        rc = write_all(fd, m->miner_sugg, m->miner_sugg_len);
    } else if (rc == 0 && !smx_no_suggest()) {
        /* The miner never sent one, so suggest on its behalf from the measured
         * rate. This is the common case for the mux's own suggestion: the primary
         * has usually been mining long enough to have an estimate by the time the
         * secondary comes up, and a fresh session otherwise opens at whatever this
         * pool's default is (1,000,000 at some pools) — which the first swap onto
         * it would then hand to the miner. */
        int64_t t = now_us();
        double want = suggest_target_diff(mux_hashrate(m, t));
        if (want > 0.0)
            rc = suggest_send(m, m->sec, want, t, "secondary bring-up");
    }
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
    m->sec_vroll_seen = false;
    m->sec_vmask = 0;
    m->sec_vmask_applied = false;
    m->pool[p].last_notify_len = 0;
    m->pool[p].enonce1[0] = '\0';
    m->sec_state = SEC_HANDSHAKING;
    /* A new session starts back at this pool's default difficulty, so the
     * suggestion rate limit starts fresh with it (the limit exists to stop the
     * mux nagging ONE session, not to ration sessions). */
    m->sugg_last_us[p] = 0;
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
        json_integer_value(idj) == SENTINEL_CONFIGURE) {
        /* the secondary's version-rolling grant (used to reconcile the mask) */
        json_decref(r);
        m->sec_vroll_seen = true;
        m->sec_vmask = 0;
        if (cfg_result_parse(line, &m->sec_vmask))
            smx_dbg("secondary(%c): granted version-rolling mask %08x\n",
                    p == 0 ? 'A' : 'B', (unsigned)m->sec_vmask);
        else {
            m->sec_vmask = 0;                  /* declined: no rolling on this pool */
            smx_dbg("secondary(%c): declined version-rolling\n",
                    p == 0 ? 'A' : 'B');
        }
        return 0;
    }
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
    /* Hand the miner the TARGET pool's difficulty (m->active still points at the
     * pool we are leaving at this point, so pass tgt explicitly). */
    if (emit_miner_diff(m, tgt) < 0)
        return -1;
    if (write_all(m->down_fd, notify_line, nlen) < 0)
        return -1;

    /* FIX-3: this notify's job is now shown to the miner — record it, tagged to
     * the target pool, so a later submit for it routes to the right pool.
     * FIX-11: remember it as this pool's last-shown job so a subsequent swap-back
     * won't force-clean re-present the same job (which would cause duplicates). */
    stratum_msg_t nm;
    if (stratum_msg_parse(notify_line, &nm) == 0 && nm.type == SM_NOTIFY) {
        ring_add(&m->ring, nm.job_id, tgt, t);
        snprintf(m->pool[tgt].last_shown_job,
                 sizeof m->pool[tgt].last_shown_job, "%s", nm.job_id);
    }

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

/* Share accounting (defined below, next to the submit router that records into
 * the pending ring): resolve a pool ack into an accepted/rejected tally. */
static void pend_resolve(mux_t *m, int64_t id, bool accepted);

/* Tell the miner the difficulty of `pool` — the one whose work it is about to be
 * mining (see mux_t.miner_diff for why this is per-pool and not max(A,B)). Emits
 * only on a real change, so a pool re-announcing the same difficulty costs
 * nothing. A pool whose difficulty is still unknown (0) is ignored. Returns 0, or
 * -1 on a miner write failure. */
static int emit_miner_diff(mux_t *m, int pool)
{
    /* Present the ACTIVE pool's difficulty. Telling the miner max(diffA,diffB)
     * instead does eliminate "Above target" rejects, but it silently costs real
     * money: a pool credits a share at the difficulty IT assigned, so mining at
     * the higher pool's difficulty means every share routed to the lower one is
     * credited a fraction of the work it actually represents. Measured live: with
     * A=4096, B=8192 and 70% of shares going to A, credited hashrate came out
     * ~31% below the miner's real rate — matching the predicted 0.70 x 50%.
     * Per-pool difficulty credits every share in full; the cost is a few shares
     * stranded mid-flight whenever a pool moves its difficulty, which is far
     * cheaper than a permanent ~third of the hashrate. */
    double want = m->pool[pool].diff;
    if (want <= 0.0 || want == m->miner_diff)
        return 0;
    char buf[128];
    int n = sm_emit_set_difficulty(buf, sizeof buf - 1, want);
    if (n <= 0)
        return 0;
    buf[n] = '\n';
    smx_dbg("diff: pools A=%.0f B=%.0f -> telling miner pool %c diff %.0f (was %.0f)\n",
            m->pool[0].diff, m->pool[1].diff, pool == 0 ? 'A' : 'B',
            want, m->miner_diff);
    smx_dbg_line("TX", -1, buf, (size_t)n + 1);
    if (write_all(m->down_fd, buf, (size_t)n + 1) < 0)
        return -1;
    m->miner_diff = want;
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
        /* Only the pool the miner is currently mining sets its difficulty. The
         * other pool's value is remembered and applied when we swap to it, so a
         * background vardiff move on the idle pool never disturbs live work. */
        if (p != m->active)
            return 0;
        return emit_miner_diff(m, p);
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

        /* FIX-5: an armed swap fires on the target's next notify — no waiting for
         * a rare clean_jobs==true.
         * FIX-11: but only on a FRESH job (one the miner has not already been
         * shown for this pool). A pool that merely RE-broadcasts the same job
         * must not trigger a swap that force-cleans work the miner already did on
         * it (duplicate shares); keep waiting for genuinely new work instead. */
        if (m->pending && p == m->target &&
            (smx_swap_represent() ||
             strcmp(msg.job_id, m->pool[p].last_shown_job) != 0))
            return do_swap(m, line, len, t);

        if (p == m->active) {
            /* FIX-3: record ONLY jobs actually shown to the miner, tagged with
             * the owning pool, so overlapping id namespaces cannot cross-route.
             * FIX-11: also track it as this pool's last-shown job. */
            ring_add(&m->ring, msg.job_id, p, t);
            snprintf(m->pool[p].last_shown_job,
                     sizeof m->pool[p].last_shown_job, "%s", msg.job_id);
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
            msg.id == SENTINEL_CONFIGURE || msg.id == SENTINEL_SUGGEST)
            return 0;
        /* If this acks a share we routed, tally it before relaying. A share is
         * accepted only on result:true; result:false or an error reply is a
         * reject. Non-submit acks simply won't match the pending ring. */
        if (m->share_cb) {
            json_error_t je;
            json_t *rj = json_loads(line, 0, &je);
            if (rj) {
                json_t *res = json_object_get(rj, "result");
                json_t *err = json_object_get(rj, "error");
                bool ok = json_is_true(res) &&
                          !(err && !json_is_null(err));
                pend_resolve(m, msg.id, ok);
                json_decref(rj);
            }
        }
        return write_all(m->down_fd, line, len);
    }

    /* other upstream chatter is not forwarded */
    return 0;
}

/* ---- share accounting: correlate a routed submit with its pool's ack ------ */

/* Remember a submit we just routed, so its ack can be attributed to `pool` at the
 * difficulty it was submitted under. Ignores id<0 (a submit with no integer id
 * can never be correlated). Oldest entry is overwritten when the ring is full. */
static void pend_add(mux_t *m, int64_t id, int pool, double diff)
{
    if (!m->share_cb || id < 0)
        return;
    m->pend[m->pend_head].id = id;
    m->pend[m->pend_head].pool = pool;
    m->pend[m->pend_head].diff = diff;
    m->pend[m->pend_head].used = true;
    m->pend_head = (m->pend_head + 1) % SPLITMUX_PEND;
}

/* Resolve a pool ack against the pending ring and report the share. An id we
 * never recorded (or already consumed) is ignored, so nothing is double counted. */
static void pend_resolve(mux_t *m, int64_t id, bool accepted)
{
    if (!m->share_cb || id < 0)
        return;
    for (int k = 0; k < SPLITMUX_PEND; k++) {
        if (!m->pend[k].used || m->pend[k].id != id)
            continue;
        m->pend[k].used = false;
        smx_dbg("share: id=%lld pool %c %s (diff %.0f)\n", (long long)id,
                m->pend[k].pool == 0 ? 'A' : 'B',
                accepted ? "ACCEPTED" : "REJECTED", m->pend[k].diff);
        m->share_cb(m->share_ctx, m->pend[k].pool, accepted, m->pend[k].diff,
                    m->worker);
        return;
    }
}

/* Route one miner submit to its owning pool (verbatim), or drop it. */
static int handle_submit(mux_t *m, int64_t id, const char *job_id,
                         const char *line, size_t len)
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
    /* Weight by the ROUTED pool's difficulty — that is what this pool credits the
     * share at, so it is what the dashboard and the slice sizer should see. (Using
     * the miner's currently-advertised difficulty instead mis-weights a share that
     * the grace window routes back to the pool we just left, since it was mined at
     * that pool's difficulty, not the new one's.) */
    double w = m->pool[pool].diff > 0.0 ? m->pool[pool].diff : m->miner_diff;
    m->total_work += w * 4294967296.0;
    m->routed_shares++;
    /* Pending until this pool acks it (the ack is what tells us accept/reject). */
    pend_add(m, id, pool, w);
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
                    if (handle_submit(m, msg.id, msg.job_id, tmp, linelen) < 0) { rc = -1; break; }
                } else {
                    /* M5: a post-authorize extranonce.subscribe still marks the
                     * miner as set_extranonce-capable (smooth-swap path). */
                    if (parsed && msg.type == SM_EXTRANONCE_SUBSCRIBE)
                        m->miner_ext_ok = true;
                    /* mining.suggest_difficulty tells a pool how to size THIS
                     * miner. It must reach BOTH pools: sending it only to the
                     * active one leaves the other at its default (e.g. 1000000)
                     * until its own vardiff crawls down, so the next swap hands a
                     * big miner a difficulty orders of magnitude too high — it
                     * finds almost nothing, in-flight shares reject "Above
                     * target", and the firmware eventually reconnects. Broadcast
                     * it (and any similarly session-scoped hint) to every
                     * connected pool; the reply the miner sees is the active
                     * pool's, which is the one it is mining. */
                    bool broadcast = (strstr(tmp, "\"mining.suggest_difficulty\"") != NULL);
                    if (broadcast && linelen < sizeof m->miner_sugg) {
                        memcpy(m->miner_sugg, tmp, linelen);
                        m->miner_sugg[linelen] = '\0';
                        m->miner_sugg_len = linelen;   /* replayed to a late secondary */
                    }
                    if (broadcast) {
                        smx_dbg("miner chatter -> BOTH pools (session-scoped hint)\n");
                        for (int q = 0; q < 2; q++) {
                            if (q == m->active || m->pool[q].fd < 0)
                                continue;
                            smx_dbg_line("TX", q, tmp, linelen);
                            (void)write_all(m->pool[q].fd, tmp, linelen);
                        }
                    } else {
                        smx_dbg("miner chatter -> active pool %c\n",
                                m->active == 0 ? 'A' : 'B');
                    }
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
            /* Reconcile the secondary's version-rolling grant against the mask
             * the miner is using: if the secondary grants less, narrow the miner
             * (mining.set_version_mask with the AND-intersection) BEFORE any swap
             * routes a rolled share to it. Runs once per successful bring-up. */
            if (m->vmask_active && !m->sec_vmask_applied) {
                m->sec_vmask_applied = true;
                /* N2 fail-safe: configure precedes subscribe in the bring-up, so
                 * if the secondary hasn't confirmed version-rolling by READY it
                 * never answered configure — narrow the miner to 0 (stop rolling)
                 * rather than route a rolled share to a pool that never granted a
                 * mask. A pool that answered (grant or decline) carries its mask. */
                uint32_t sm = m->sec_vroll_seen ? m->sec_vmask : 0;
                if (reconcile_mask(m, sm) < 0)
                    return -1;
            }
            /* The secondary's difficulty is now known, but the miner is on the
             * primary — it will be applied by the swap that moves work there. */
            /* drain any lines already buffered past the first notify */
            if (process_lines(m, &m->bup[p], true, p) < 0)
                return -1;
        }
        return 0;
    }
    if (lb_fill(m->pool[p].fd, &m->bup[p]) < 0) {
        /* Whether a dead pool is recoverable depends on whether the miner is
         * currently mining it — NOT on which pool happened to be handshaked first.
         * The active pool alternates every slice, so the original primary is the
         * idle one about half the time; keying this on the static role tore down
         * sessions the mux could have carried on, dropping the miner for roughly
         * half of all mid-session outages. Any pool that is not active can simply
         * be retried in the background while the miner keeps hashing on the other.
         *
         * Re-designate the dead pool as the secondary so the async bring-up owns
         * it, and clear the version-mask reconciliation so its mask is negotiated
         * again when it returns (it may come back with a different grant). */
        if (p != m->active) {
            if (m->sec != p) {
                m->sec = p;
                m->sec_backoff_s = SEC_BACKOFF_MIN;
                m->sec_degrade_logged = false;
                m->sec_vroll_seen = false;
                m->sec_vmask = 0;
                m->sec_vmask_applied = false;
            }
            sec_fail(m, now_us());       /* idle pool died: retry it in background */
            return 0;
        }
        return -1;                       /* the pool the miner is ON died: end */
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
    if (m->pool[m->sec].fd < 0) {
        /* The secondary wasn't connectable when the miner arrived — typically its
         * ckproxy is still warming up. Do NOT dial+subscribe it immediately (that
         * no-work window is exactly what can SIGABRT stock ckpool); wait one
         * backoff and let the retry path bring it up when it's ready. */
        m->sec_state = SEC_WAIT;
        m->sec_retry_at_us = t0 + (int64_t)SEC_BACKOFF_MIN * 1000000LL;
        fprintf(stderr, "splitmux: secondary pool %c not connected yet — will "
                        "bring it up in %ds (miner mines pool %c meanwhile)\n",
                m->sec == 0 ? 'A' : 'B', SEC_BACKOFF_MIN,
                primary == 0 ? 'A' : 'B');
    } else {
        /* Secondary already connected: kick off its async bring-up now. */
        sec_begin_attempt(m, t0);
    }

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

        /* Tell a badly-mismatched pool how big this miner is (rate-limited, and
         * never when the miner sends its own suggest_difficulty). Cheap enough to
         * run on every loop tick; it self-limits. */
        mux_suggest_check(m, t);

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
                     * waiting for its next clean notify.
                     * FIX-11: but ONLY when that job is one the miner has not been
                     * shown for this pool yet (genuinely fresh work). Re-presenting
                     * an already-shown job as forced-clean makes the miner discard
                     * its progress and re-mine the identical enonce2 space, so it
                     * resubmits shares it already sent — duplicates that a low-churn
                     * solo pool rejects (a high-churn pool dodges this because each
                     * swap-back lands on a new job). When the target has nothing
                     * newer, arm the swap to fire on its next FRESH notify and keep
                     * the miner productive on the current pool meanwhile, rather
                     * than manufacturing duplicate work. */
                    if (m->pool[tgt].last_notify_len > 0 &&
                        (smx_swap_represent() ||
                         strcmp(m->pool[tgt].cur_job,
                                m->pool[tgt].last_shown_job) != 0)) {
                        char nb[SPLITMUX_LINE];
                        size_t nl;
                        if (rewrite_notify_clean(m->pool[tgt].last_notify,
                                                 nb, sizeof nb, &nl) == 0) {
                            if (do_swap(m, nb, nl, t) < 0) break;
                        } else {
                            m->pending = true;
                        }
                    } else {
                        m->pending = true;   /* swap on the target's next fresh notify */
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
                  const char *up_addr[2],
                  splitmux_share_cb share_cb, void *share_ctx,
                  bool assume_ext)
{
    /* Pure passthrough only when there is genuinely NO second pool — i.e. the
     * caller supplied no address for it (the M3 single-upstream probe). A second
     * pool that merely isn't CONNECTED yet (fd < 0 but an address given, e.g. its
     * ckproxy is still warming up after a restart) takes the normal dual path: the
     * async secondary bring-up dials it and retries with backoff, so the session
     * becomes a real split once that pool is ready instead of being pinned
     * single-pool for its entire life. Miners reconnect within seconds of a
     * restart, so without this virtually every session after a restart would be
     * stuck on one pool. */
    bool have_sec_addr = up_addr && up_addr[1] && up_addr[1][0];
    if (up_fd[1] < 0 && !have_sec_addr) {
        /* Byte-relay: never parses submits/acks — the caller's own sniffer
         * accounts for this path. */
        (void)ratio_a; (void)target_shares; (void)min_s; (void)max_s;
        (void)start_pool; (void)share_cb; (void)share_ctx;
        (void)assume_ext;   /* no slices in single-pool mode: nothing to swap */
        splitmux_passthrough(down_fd, up_fd[0]);
        return;
    }
    if (up_fd[0] < 0 && up_fd[1] < 0)
        return;             /* neither pool connected: caller closes and retries */

    mux_t m;
    memset(&m, 0, sizeof m);
    m.down_fd = down_fd;
    m.vmask_pool = -1;           /* no version-rolling grant negotiated yet */
    m.pool[0].fd = up_fd[0];
    m.pool[1].fd = up_fd[1];
    m.ratio_a = ratio_a;
    m.target_shares = target_shares;
    m.min_s = min_s;
    m.max_s = max_s;
    m.start_pool = start_pool;
    /* EXPERIMENTAL opt-in: start the session already marked set_extranonce-
     * capable so the deadline takes the smooth swap instead of the M5
     * reconnect-slice fallback. Default (false) leaves the memset zero, i.e.
     * capability is detected from mining.extranonce.subscribe exactly as before. */
    if (assume_ext)
        m.miner_ext_ok = true;
    m.up_addr[0] = up_addr ? up_addr[0] : NULL;
    m.up_addr[1] = up_addr ? up_addr[1] : NULL;
    m.share_cb = share_cb;
    m.share_ctx = share_ctx;
    splitmux_dual(&m);

    /* The secondary fd may have been closed/reopened during retries — hand the
     * FINAL upstream fds back so the caller closes the right ones. A secondary
     * that is mid-backoff at return is -1; the caller skips it. */
    up_fd[0] = m.pool[0].fd;
    up_fd[1] = m.pool[1].fd;
}
