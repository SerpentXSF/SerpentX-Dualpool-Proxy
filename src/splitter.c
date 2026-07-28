/*
 * splitter.c — SerpentX farm-split proxy front-end (minimal, CLI-configured).
 * Part of SerpentX (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 *
 * This is the connection router: it listens for miners, assigns each to pool A
 * or B by the hashrate-weighted allocator, connects to that pool's upstream
 * (a ckproxy in production, or a pool directly in the T2 harness), and relays
 * transparently. Upstream Stratum semantics and per-pool failover are handled
 * by ckproxy; this process owns allocation, relay, and (later) accounting.
 *
 * Config is via CLI flags here so the T2 integration harness can drive it
 * without the jansson config layer:
 *   splitter --listen PORT --poolA host:port --poolB host:port --ratio N
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <signal.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <stdint.h>
#include <jansson.h>

#include "alloc.h"
#include "relay.h"
#include "config.h"
#include "ckproxy_config.h"
#include "health.h"
#include "share_accounting.h"
#include "webui.h"
#include "dual_clamp.h"
#include "version.h"

static const char *g_pool_host[2];
static const char *g_pool_port[2];
static alloc_t     g_alloc;
static health_t    g_health;
static pthread_mutex_t g_alloc_lock = PTHREAD_MUTEX_INITIALIZER;

/* -------- time-slice mode (single miner across both pools) -------------- */
static bool             g_time_slice   = false;
static int              g_interval_ms  = 180000;
static pool_scheduler_t g_sched;              /* reused error-diffusion, 1 slice/boundary */
static pool_id_t        g_current_pool = POOL_A;
static long             g_slice_n      = 0;

/* -------- live stats for the dashboard --------------------------------- */
static share_totals_t g_totals;
static pthread_mutex_t g_totals_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long   g_routed[2];              /* miners routed per pool */
static serpentx_config_t g_cfg;                  /* current config snapshot */
static char g_webroot[512] = "/usr/local/share/serpentx/web";
static char g_config_path[512] = "";             /* set in config mode */
static int64_t g_last_notify_us[2];              /* job-flow liveness per pool */
static volatile sig_atomic_t g_reload = 0;       /* SIGHUP -> reload config */

/* ckproxy crash-loop tracking (a broken pool is donated away, not tight-looped) */
static volatile int     g_pool_crashloop[2];     /* 1 => force pool down */
static int64_t          g_pool_last_exit_us[2];
static int              g_pool_fail_streak[2];

static int64_t mono_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void sighup_handler(int sig) { (void)sig; g_reload = 1; }

/* -------- live-connection registry (for eviction on pool-down) ---------- */
#define SX_MAX_CONNS 4096
typedef struct {
    bool      used;
    pool_id_t pool;
    int       down_fd;
    char      worker[64];
    time_t    since;
} conn_slot_t;
static conn_slot_t g_conns[SX_MAX_CONNS];
static pthread_mutex_t g_conns_lock = PTHREAD_MUTEX_INITIALIZER;

static int conn_register(pool_id_t pool, int down_fd)
{
    int idx = -1;
    pthread_mutex_lock(&g_conns_lock);
    for (int i = 0; i < SX_MAX_CONNS; i++) {
        if (!g_conns[i].used) {
            g_conns[i].used = true;
            g_conns[i].pool = pool;
            g_conns[i].down_fd = down_fd;
            g_conns[i].worker[0] = '\0';
            g_conns[i].since = time(NULL);
            idx = i;
            break;
        }
    }
    pthread_mutex_unlock(&g_conns_lock);
    return idx;
}

static void conn_unregister(int idx)
{
    if (idx < 0) return;
    pthread_mutex_lock(&g_conns_lock);
    g_conns[idx].used = false;
    pthread_mutex_unlock(&g_conns_lock);
}

/* Force miners pinned to a dead pool to disconnect so they reconnect and get
 * reallocated onto the surviving pool. shutdown() unblocks their relay poll. */
static void evict_pool(pool_id_t pool)
{
    int n = 0;
    pthread_mutex_lock(&g_conns_lock);
    for (int i = 0; i < SX_MAX_CONNS; i++) {
        if (g_conns[i].used && g_conns[i].pool == pool) {
            shutdown(g_conns[i].down_fd, SHUT_RDWR);
            n++;
        }
    }
    pthread_mutex_unlock(&g_conns_lock);
    if (n) fprintf(stderr, "serpentx: pool %c down -> evicted %d miner(s)\n",
                   (pool == POOL_A) ? 'A' : 'B', n);
}

/* Disconnect ALL miners (used at a time-slice boundary so the single miner
 * reconnects and is re-subscribed to the next slice's pool). */
static void evict_all(void)
{
    pthread_mutex_lock(&g_conns_lock);
    for (int i = 0; i < SX_MAX_CONNS; i++)
        if (g_conns[i].used) shutdown(g_conns[i].down_fd, SHUT_RDWR);
    pthread_mutex_unlock(&g_conns_lock);
}

/* One synthetic scheduler tick == one real slice boundary. The scheduler clamps
 * its interval to a 100ms floor, so we advance the synthetic clock 100ms per
 * boundary (SLICE_TICK_US) to trigger exactly one error-diffusion step. This
 * decouples the scheduler's uint16/clamped interval from our real (minute-scale)
 * wall-clock interval. */
#define SLICE_TICK_US 100000  /* 100 ms == the scheduler's clamped interval */

static void timeslice_advance(void)
{
    pthread_mutex_lock(&g_alloc_lock);
    g_slice_n++;
    g_current_pool = pool_scheduler_select(&g_sched, (int64_t)g_slice_n * SLICE_TICK_US);
    pthread_mutex_unlock(&g_alloc_lock);
}

static void *timeslice_thread(void *arg)
{
    (void)arg;
    for (;;) {
        usleep((useconds_t)g_interval_ms * 1000);
        timeslice_advance();
        evict_all();   /* recycle connections onto the new slice's pool */
        fprintf(stderr, "serpentx: time-slice boundary -> pool %c\n",
                (g_current_pool == POOL_A) ? 'A' : 'B');
    }
    return NULL;
}

static void setup_timeslice(int ratio, int interval_ms)
{
    g_time_slice  = true;
    g_interval_ms = interval_ms;
    pool_scheduler_init(&g_sched, (uint8_t)ratio, 100 /*synthetic tick, ms*/, 0);
    g_slice_n = 0;
    g_current_pool = pool_scheduler_select(&g_sched, 0);   /* first slice */
    pthread_t t;
    pthread_create(&t, NULL, timeslice_thread, NULL);
    pthread_detach(t);
    fprintf(stderr, "serpentx: TIME-SLICE interval=%dms ratio A=%d%% (single-miner mode)\n",
            interval_ms, ratio);
}

/* Connect a TCP socket to host:port. Returns fd or -1. */
static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints = {0}, *res, *rp;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;

    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

typedef struct {
    int down_fd; int up_fd; int slot;
    pool_id_t pool;
    share_session_t sess;
} conn_t;

/* Passive sniff of relayed Stratum lines: track per-pool difficulty-weighted
 * shares and capture the worker name. Bytes are forwarded verbatim regardless. */
static void sniff(void *ctx, int up, const char *line, size_t len)
{
    conn_t *c = ctx;
    json_error_t je;
    json_t *m = json_loadb(line, len, 0, &je);
    if (!m) return;

    json_t *method = json_object_get(m, "method");
    const char *ms = (method && json_is_string(method)) ? json_string_value(method) : NULL;

    if (up) {
        /* miner -> pool */
        if (ms && !strcmp(ms, "mining.submit")) {
            json_t *id = json_object_get(m, "id");
            if (json_is_integer(id))
                share_session_on_submit(&c->sess, (long)json_integer_value(id));
        } else if (ms && !strcmp(ms, "mining.authorize")) {
            json_t *p = json_object_get(m, "params");
            if (json_is_array(p) && json_array_size(p) >= 1) {
                json_t *w = json_array_get(p, 0);
                if (json_is_string(w) && c->slot >= 0) {
                    pthread_mutex_lock(&g_conns_lock);
                    snprintf(g_conns[c->slot].worker, sizeof(g_conns[c->slot].worker),
                             "%s", json_string_value(w));
                    pthread_mutex_unlock(&g_conns_lock);
                }
            }
        }
    } else {
        /* pool -> miner */
        if (ms && !strcmp(ms, "mining.set_difficulty")) {
            json_t *p = json_object_get(m, "params");
            if (json_is_array(p) && json_array_size(p) >= 1) {
                json_t *d = json_array_get(p, 0);
                if (json_is_number(d))
                    share_session_set_difficulty(&c->sess, json_number_value(d));
            }
        } else if (ms && !strcmp(ms, "mining.notify")) {
            g_last_notify_us[c->pool] = mono_us();   /* job flowing from this pool */
        } else if (!ms) {
            /* a response: {id, result} — only counts if id was a pending submit */
            json_t *id = json_object_get(m, "id");
            json_t *res = json_object_get(m, "result");
            if (json_is_integer(id) && res && !json_is_null(res)) {
                pthread_mutex_lock(&g_totals_lock);
                share_session_on_result(&c->sess, (long)json_integer_value(id),
                                        json_is_true(res), &g_totals);
                pthread_mutex_unlock(&g_totals_lock);
            }
        }
    }
    json_decref(m);
}

static void *conn_thread(void *arg)
{
    conn_t *c = arg;
    relay_pump(c->down_fd, c->up_fd, sniff, c);
    close(c->down_fd);
    close(c->up_fd);
    conn_unregister(c->slot);
    free(c);
    return NULL;
}

/* Pick a pool (allocator already skips DOWN pools) and connect upstream,
 * donating to the survivor if this single connect happens to fail. Global
 * up/down state is owned by the probe thread, not mutated here. */
static int open_upstream(pool_id_t *out_pool)
{
    pthread_mutex_lock(&g_alloc_lock);
    pool_id_t p = g_time_slice ? g_current_pool   /* time-slice: current slice */
                               : alloc_pick(&g_alloc, 0);  /* farm-split */
    pthread_mutex_unlock(&g_alloc_lock);

    int up = tcp_connect(g_pool_host[p], g_pool_port[p]);
    if (up < 0) {
        pool_id_t other = (p == POOL_A) ? POOL_B : POOL_A;
        up = tcp_connect(g_pool_host[other], g_pool_port[other]);
        p = other;
    }
    *out_pool = p;
    return up;
}

static int conn_count_pool(pool_id_t pool)
{
    int n = 0;
    pthread_mutex_lock(&g_conns_lock);
    for (int i = 0; i < SX_MAX_CONNS; i++)
        if (g_conns[i].used && g_conns[i].pool == pool) n++;
    pthread_mutex_unlock(&g_conns_lock);
    return n;
}

/* Rewrite the full config (ratio/mode/interval + pools) back to the config file,
 * preserving any other keys (downstream, web_password). */
static void persist_config(void)
{
    if (!g_config_path[0]) return;
    json_error_t je;
    json_t *root = json_load_file(g_config_path, 0, &je);
    if (!root) root = json_object();

    json_object_set_new(root, "ratio_a", json_integer(g_cfg.ratio_a));
    json_object_set_new(root, "mode", json_string(g_cfg.mode));
    json_object_set_new(root, "interval_ms", json_integer(g_cfg.interval_ms));

    json_t *pools = json_array();
    for (int i = 0; i < 2; i++) {
        pool_cfg_t *pc = &g_cfg.pools[i];
        json_t *p = json_object();
        json_object_set_new(p, "url",  json_string(pc->primary.url));
        json_object_set_new(p, "user", json_string(pc->primary.user));
        json_object_set_new(p, "pass", json_string(pc->primary.pass));
        json_object_set_new(p, "ckproxy_mode", json_string(pc->ckproxy_mode));
        if (pc->has_failover) {
            json_t *f = json_object();
            json_object_set_new(f, "url",  json_string(pc->failover.url));
            json_object_set_new(f, "user", json_string(pc->failover.user));
            json_object_set_new(f, "pass", json_string(pc->failover.pass));
            json_object_set_new(p, "failover", f);
        }
        json_array_append_new(pools, p);
    }
    json_object_set_new(root, "pools", pools);

    json_dump_file(root, g_config_path, JSON_INDENT(2));
    json_decref(root);
}

/* Re-read the config file and hot-apply ratio/mode/interval (SIGHUP). Pool
 * changes need a restart (they respawn ckproxy) and are not hot-applied. */
static void reload_config(void)
{
    if (!g_config_path[0]) return;
    serpentx_config_t c;
    char err[256];
    if (config_parse_file(g_config_path, &c, err, sizeof(err)) != 0) {
        fprintf(stderr, "serpentx: reload failed: %s\n", err);
        return;
    }
    pthread_mutex_lock(&g_alloc_lock);
    g_alloc.ratio_a = (uint8_t)c.ratio_a;
    g_cfg.ratio_a = c.ratio_a;
    snprintf(g_cfg.mode, sizeof(g_cfg.mode), "%s", c.mode);
    g_cfg.interval_ms = c.interval_ms;
    pthread_mutex_unlock(&g_alloc_lock);
    fprintf(stderr, "serpentx: config reloaded (ratio A=%d%% mode=%s)\n",
            c.ratio_a, c.mode);
}

/* Background probe: connect-test each pool + job-flow liveness, update
 * health -> allocator, evict on DOWN, and service SIGHUP reloads. */
static void *probe_thread(void *arg)
{
    (void)arg;
    for (;;) {
        if (g_reload) { g_reload = 0; reload_config(); }

        for (int i = 0; i < 2; i++) {
            int fd = tcp_connect(g_pool_host[i], g_pool_port[i]);
            bool reachable = (fd >= 0);
            if (reachable) close(fd);

            /* Job-flow liveness: if a pool has miners but hasn't sent work in a
             * long time, its upstream is effectively dead even if the ckproxy
             * socket still accepts (ckproxy up, both real pools dead). */
            bool stale = false;
            if (conn_count_pool(i) > 0 && g_last_notify_us[i] > 0) {
                if (mono_us() - g_last_notify_us[i] > 90LL * 1000 * 1000)
                    stale = true;
            }

            /* Clear a crash-loop flag once the ckproxy has been stable (no exit)
             * for a while and is reachable again (e.g. the login was fixed). */
            if (g_pool_crashloop[i] && reachable && g_pool_last_exit_us[i] &&
                mono_us() - g_pool_last_exit_us[i] > 30LL * 1000000) {
                g_pool_crashloop[i] = 0;
                g_pool_fail_streak[i] = 0;
                fprintf(stderr, "serpentx: pool %c ckproxy stable again\n", 'A' + i);
            }

            bool ok = reachable && !stale && !g_pool_crashloop[i];

            pthread_mutex_lock(&g_alloc_lock);
            bool was_up = health_pool_up(&g_health, i);
            health_report(&g_health, i, ok);
            bool now_up = health_pool_up(&g_health, i);
            alloc_set_pool_up(&g_alloc, i, now_up);
            pthread_mutex_unlock(&g_alloc_lock);

            if (was_up && !now_up)
                fprintf(stderr, "serpentx: pool %c down (%s)\n", 'A' + i,
                        stale ? "no work" : "unreachable");
            if (was_up && !now_up) evict_pool(i);
            else if (!was_up && now_up)
                fprintf(stderr, "serpentx: pool %c recovered\n", 'A' + i);
        }
        usleep(3 * 1000 * 1000);   /* 3s between probe rounds */
    }
    return NULL;
}

static int make_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    if (listen(fd, 128) < 0) { close(fd); return -1; }
    return fd;
}

/* Split "host:port" into host and port (last colon). Returns 0 on success. */
static int split_hostport(char *s, const char **host, const char **port)
{
    char *colon = strrchr(s, ':');
    if (!colon) return -1;
    *colon = '\0';
    *host = s;
    *port = colon + 1;
    return 0;
}

/* -------- dashboard callbacks ------------------------------------------ */

static char *build_status_json(void)
{
    pthread_mutex_lock(&g_alloc_lock);
    int  ratio = g_alloc.ratio_a;
    bool up[2] = { health_pool_up(&g_health, POOL_A), health_pool_up(&g_health, POOL_B) };
    unsigned long r[2] = { g_routed[0], g_routed[1] };
    serpentx_config_t cfg = g_cfg;   /* snapshot: pools can change concurrently */
    pthread_mutex_unlock(&g_alloc_lock);

    pthread_mutex_lock(&g_totals_lock);
    share_totals_t t = g_totals;
    pthread_mutex_unlock(&g_totals_lock);

    /* CURRENTLY connected miners per pool (not the cumulative routing count). */
    int connected[2] = { conn_count_pool(POOL_A), conn_count_pool(POOL_B) };

    json_t *root = json_object();
    json_t *svc = json_object();
    json_object_set_new(svc, "online", json_true());
    json_object_set_new(svc, "stratum_port", json_integer(cfg.stratum_port));
    json_object_set_new(root, "service", svc);
    json_object_set_new(root, "version", json_string(SERPENTX_VERSION));
    json_object_set_new(root, "mode", json_string(cfg.mode));
    json_object_set_new(root, "ratio_a", json_integer(ratio));
    json_object_set_new(root, "interval_ms", json_integer(cfg.interval_ms));

    /* actual split from currently-connected miners (falls back to target). */
    double ctot = (double)(connected[0] + connected[1]);
    json_t *act = json_object();
    json_object_set_new(act, "a", json_real(ctot ? 100.0 * connected[0] / ctot : ratio));
    json_object_set_new(act, "b", json_real(ctot ? 100.0 * connected[1] / ctot : 100 - ratio));
    json_object_set_new(root, "actual", act);

    json_t *pools = json_array();
    for (int i = 0; i < 2; i++) {
        json_t *p = json_object();
        json_object_set_new(p, "id",   json_string(i == 0 ? "A" : "B"));
        json_object_set_new(p, "name", json_string(i == 0 ? "Pool A" : "Pool B"));
        json_object_set_new(p, "state", json_string(up[i] ? "on" : "off"));
        json_object_set_new(p, "url",  json_string(cfg.pools[i].primary.url));
        json_object_set_new(p, "user", json_string(cfg.pools[i].primary.user));
        json_object_set_new(p, "connected", json_integer(connected[i]));   /* live */
        json_object_set_new(p, "routed", json_integer((json_int_t)r[i]));   /* cumulative */
        char hr[48];
        snprintf(hr, sizeof(hr), "%d connected", connected[i]);
        json_object_set_new(p, "hashrate", json_string(hr));
        json_object_set_new(p, "accepted", json_integer((json_int_t)t.accepted_n[i]));
        json_object_set_new(p, "rejected", json_integer((json_int_t)t.rejected_n[i]));
        json_object_set_new(p, "accepted_diff", json_real(t.accepted_diff[i]));
        json_object_set_new(p, "rejected_diff", json_real(t.rejected_diff[i]));
        json_array_append_new(pools, p);
    }
    json_object_set_new(root, "pools", pools);

    json_t *miners = json_array();
    time_t now = time(NULL);
    pthread_mutex_lock(&g_conns_lock);
    for (int i = 0; i < SX_MAX_CONNS; i++) {
        if (!g_conns[i].used) continue;
        json_t *mm = json_object();
        json_object_set_new(mm, "worker",
            json_string(g_conns[i].worker[0] ? g_conns[i].worker : "(unauth)"));
        json_object_set_new(mm, "pool", json_string(g_conns[i].pool == POOL_A ? "A" : "B"));
        long secs = (long)(now - g_conns[i].since); if (secs < 0) secs = 0;
        char since[32];
        snprintf(since, sizeof(since), "%ldm %02lds", secs / 60, secs % 60);
        json_object_set_new(mm, "since", json_string(since));
        json_object_set_new(mm, "hashrate", json_string("\xe2\x80\x94"));  /* em dash */
        json_object_set_new(mm, "accepted", json_integer(0));
        json_object_set_new(mm, "rejected", json_integer(0));
        json_array_append_new(miners, mm);
    }
    pthread_mutex_unlock(&g_conns_lock);
    json_object_set_new(root, "miners", miners);

    char *s = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    return s;
}

/* Hot-apply ratio/mode from the settings form. Connected miners are not dropped;
 * the new ratio simply governs subsequent allocations. */
static int apply_config_json(const char *body)
{
    json_error_t je;
    json_t *m = json_loads(body, 0, &je);
    if (!m) return -1;

    json_t *r = json_object_get(m, "ratio_a");
    json_t *mode = json_object_get(m, "mode");
    json_t *iv = json_object_get(m, "interval_ms");
    json_t *pools = json_object_get(m, "pools");

    pthread_mutex_lock(&g_alloc_lock);
    if (json_is_integer(r)) {
        int nr = dual_clamp_ratio((int)json_integer_value(r));
        g_alloc.ratio_a = (uint8_t)nr;
        g_cfg.ratio_a = nr;
    }
    if (json_is_string(mode)) {
        const char *mv = json_string_value(mode);
        if (!strcmp(mv, "farm_split") || !strcmp(mv, "time_slice"))
            snprintf(g_cfg.mode, sizeof(g_cfg.mode), "%s", mv);
    }
    if (json_is_integer(iv)) {
        int v = (int)json_integer_value(iv);
        if (v < 1000) v = 1000; else if (v > 3600000) v = 3600000;
        g_cfg.interval_ms = v;
    }
    /* Pool fields: update url/user/ckproxy_mode; update pass ONLY if provided
     * (the form leaves it blank to keep the current password). Saved to disk;
     * the pool switch itself takes effect on the next restart. */
    if (json_is_array(pools)) {
        size_t np = json_array_size(pools);
        for (size_t i = 0; i < np && i < 2; i++) {
            json_t *pj = json_array_get(pools, i);
            if (!json_is_object(pj)) continue;
            pool_cfg_t *P = &g_cfg.pools[i];
            const char *url  = json_string_value(json_object_get(pj, "url"));
            const char *user = json_string_value(json_object_get(pj, "user"));
            const char *pass = json_string_value(json_object_get(pj, "pass"));
            const char *cm   = json_string_value(json_object_get(pj, "ckproxy_mode"));
            if (url  && *url)  snprintf(P->primary.url,  sizeof(P->primary.url),  "%s", url);
            if (user && *user) snprintf(P->primary.user, sizeof(P->primary.user), "%s", user);
            if (pass && *pass) snprintf(P->primary.pass, sizeof(P->primary.pass), "%s", pass);
            if (cm && (!strcmp(cm, "proxy") || !strcmp(cm, "userproxy")))
                snprintf(P->ckproxy_mode, sizeof(P->ckproxy_mode), "%s", cm);
        }
    }
    pthread_mutex_unlock(&g_alloc_lock);

    json_decref(m);
    persist_config();   /* write full config to disk */
    fprintf(stderr, "serpentx: config saved (ratio A=%d%% mode=%s); "
                    "pool/credential changes take effect on restart\n",
            g_cfg.ratio_a, g_cfg.mode);
    return 0;
}

/* Prometheus text exposition for /metrics. */
static char *build_metrics(void)
{
    pthread_mutex_lock(&g_alloc_lock);
    int ratio = g_alloc.ratio_a;
    int up[2] = { health_pool_up(&g_health, POOL_A), health_pool_up(&g_health, POOL_B) };
    unsigned long r[2] = { g_routed[0], g_routed[1] };
    pthread_mutex_unlock(&g_alloc_lock);

    pthread_mutex_lock(&g_totals_lock);
    share_totals_t t = g_totals;
    pthread_mutex_unlock(&g_totals_lock);

    int connected[2] = { conn_count_pool(POOL_A), conn_count_pool(POOL_B) };
    int miners = connected[0] + connected[1];

    double ctot = (double)miners;
    double act[2] = { ctot ? 100.0 * connected[0] / ctot : ratio,
                      ctot ? 100.0 * connected[1] / ctot : 100 - ratio };

    char *buf = malloc(4096);
    if (!buf) return NULL;
    const char *nm[2] = { "A", "B" };
    int n = 0;
    n += snprintf(buf + n, 4096 - n,
        "# HELP serpentx_ratio_target_percent Target Pool A share.\n"
        "# TYPE serpentx_ratio_target_percent gauge\n"
        "serpentx_ratio_target_percent %d\n"
        "# HELP serpentx_miners_connected Currently connected miners.\n"
        "# TYPE serpentx_miners_connected gauge\n"
        "serpentx_miners_connected %d\n"
        "# HELP serpentx_pool_up Pool reachable (1) or down (0).\n"
        "# TYPE serpentx_pool_up gauge\n"
        "# HELP serpentx_routed_total Miners routed to a pool since start (cumulative).\n"
        "# TYPE serpentx_routed_total counter\n"
        "# HELP serpentx_miners_connected_pool Currently connected miners per pool.\n"
        "# TYPE serpentx_miners_connected_pool gauge\n"
        "# HELP serpentx_actual_percent Realized share of currently-connected miners.\n"
        "# TYPE serpentx_actual_percent gauge\n"
        "# HELP serpentx_shares_accepted_total Accepted shares per pool.\n"
        "# TYPE serpentx_shares_accepted_total counter\n"
        "# HELP serpentx_shares_rejected_total Rejected shares per pool.\n"
        "# TYPE serpentx_shares_rejected_total counter\n"
        "# HELP serpentx_shares_accepted_difficulty_total Difficulty-weighted accepted.\n"
        "# TYPE serpentx_shares_accepted_difficulty_total counter\n"
        "# HELP serpentx_build_info Build/version info.\n"
        "# TYPE serpentx_build_info gauge\n"
        "serpentx_build_info{version=\"" SERPENTX_VERSION "\"} 1\n",
        ratio, miners);
    for (int i = 0; i < 2; i++) {
        n += snprintf(buf + n, 4096 - n,
            "serpentx_pool_up{pool=\"%s\"} %d\n"
            "serpentx_routed_total{pool=\"%s\"} %lu\n"
            "serpentx_miners_connected_pool{pool=\"%s\"} %d\n"
            "serpentx_actual_percent{pool=\"%s\"} %.1f\n"
            "serpentx_shares_accepted_total{pool=\"%s\"} %llu\n"
            "serpentx_shares_rejected_total{pool=\"%s\"} %llu\n"
            "serpentx_shares_accepted_difficulty_total{pool=\"%s\"} %.0f\n",
            nm[i], up[i], nm[i], r[i], nm[i], connected[i], nm[i], act[i],
            nm[i], (unsigned long long)t.accepted_n[i],
            nm[i], (unsigned long long)t.rejected_n[i],
            nm[i], t.accepted_diff[i]);
    }
    return buf;
}

static void webui_boot(int web_port)
{
    const char *wr = getenv("SERPENTX_WEBROOT");
    if (wr) snprintf(g_webroot, sizeof(g_webroot), "%s", wr);
    if (web_port > 0)
        webui_start(web_port, g_webroot, build_status_json, apply_config_json,
                    build_metrics, g_cfg.web_password);
}

/* Accept miners forever, routing each to its allocated pool. */
static int run_accept_loop(int listen_port)
{
    int lfd = make_listener(listen_port);
    if (lfd < 0) { perror("listen"); return 1; }
    fprintf(stderr, "SerpentX splitter: listening :%d  A=%s:%s  B=%s:%s\n",
            listen_port, g_pool_host[POOL_A], g_pool_port[POOL_A],
            g_pool_host[POOL_B], g_pool_port[POOL_B]);

    /* start health tracking + the reachability probe (donation + eviction) */
    health_init(&g_health, 1);   /* tolerate 1 miss (~6s) before DOWN */
    pthread_t probe;
    pthread_create(&probe, NULL, probe_thread, NULL);
    pthread_detach(probe);

    for (;;) {
        int down = accept(lfd, NULL, NULL);
        if (down < 0) { if (errno == EINTR) continue; break; }

        pool_id_t pool;
        int up = open_upstream(&pool);
        if (up < 0) { close(down); continue; }   /* both pools down */
        fprintf(stderr, "serpentx: route -> %c\n", (pool == POOL_A) ? 'A' : 'B');

        pthread_mutex_lock(&g_alloc_lock);
        g_routed[pool]++;
        pthread_mutex_unlock(&g_alloc_lock);

        int slot = conn_register(pool, down);
        conn_t *c = malloc(sizeof(*c));
        c->down_fd = down;
        c->up_fd   = up;
        c->slot    = slot;
        c->pool    = pool;
        share_session_init(&c->sess, pool);
        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread, c) != 0) {
            close(down); close(up); conn_unregister(slot); free(c);
            continue;
        }
        pthread_detach(t);
    }
    close(lfd);
    return 0;
}

/* -------- config mode: spawn + supervise two stock ckproxy -------------- */

static void ensure_dir(const char *p) { mkdir(p, 0755); /* ignore EEXIST */ }

/* Retry-connect until the port accepts or timeout. Returns 0 if up. */
static int wait_port(const char *host, const char *port, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        int fd = tcp_connect(host, port);
        if (fd >= 0) { close(fd); return 0; }
        usleep(200 * 1000);
        waited += 200;
    }
    return -1;
}

/* Supervised ckproxy: enough to respawn it with the same args if it dies. */
typedef struct {
    const char *ckbin;
    const pool_cfg_t *pool;
    char cfg[256];
    char sock[256];
    char name[32];
    pid_t pid;
} ckproxy_proc_t;

static ckproxy_proc_t g_ck[2];

static void spawn_one(ckproxy_proc_t *p)
{
    p->pid = ckproxy_spawn(p->ckbin, p->pool, p->cfg, p->sock, p->name);
}

/* Monitor thread: reap and respawn a died ckproxy with exponential backoff. If a
 * ckproxy keeps dying quickly (e.g. the pool rejects the login), it is flagged
 * as crash-looping so the probe marks that pool DOWN and miners are donated to
 * the healthy pool instead of churning on the broken one. */
static void *monitor_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int status;
        pid_t died = waitpid(-1, &status, 0);
        if (died < 0) { usleep(500 * 1000); continue; }
        for (int i = 0; i < 2; i++) {
            if (g_ck[i].pid != died) continue;

            int64_t now = mono_us();
            if (g_pool_last_exit_us[i] && now - g_pool_last_exit_us[i] < 15LL * 1000000)
                g_pool_fail_streak[i]++;
            else
                g_pool_fail_streak[i] = 0;
            g_pool_last_exit_us[i] = now;

            if (g_pool_fail_streak[i] >= 3 && !g_pool_crashloop[i]) {
                g_pool_crashloop[i] = 1;
                fprintf(stderr, "serpentx: ckproxy %s crash-looping (x%d) — pool marked "
                        "DOWN, donating to the other pool. Check %s/console.log "
                        "(often an upstream 'Invalid login').\n",
                        g_ck[i].name, g_pool_fail_streak[i], g_ck[i].sock);
            }

            int shift = g_pool_fail_streak[i] > 5 ? 5 : g_pool_fail_streak[i];
            unsigned backoff = 1u << shift;          /* 1,2,4,8,16,32 s */
            if (backoff > 30) backoff = 30;
            fprintf(stderr, "serpentx: ckproxy %s exited; respawn in %us\n",
                    g_ck[i].name, backoff);
            sleep(backoff);
            spawn_one(&g_ck[i]);
        }
    }
    return NULL;
}

static int run_config_mode(const char *path)
{
    serpentx_config_t cfg;
    char err[256];
    if (config_parse_file(path, &cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "serpentx: config error: %s\n", err);
        return 1;
    }
    g_cfg = cfg;   /* snapshot for the dashboard */
    snprintf(g_config_path, sizeof(g_config_path), "%s", path);   /* for persist/reload */

    const char *ckbin = getenv("SERPENTX_CKPOOL_BIN");
    if (!ckbin) ckbin = "/usr/local/bin/ckpool";
    const char *base = getenv("SERPENTX_RUNDIR");
    if (!base) base = "/tmp/serpentx";
    ensure_dir(base);

    const int local_port[2] = { 4001, 4002 };
    const char *nm[2] = { "serpentxA", "serpentxB" };

    for (int i = 0; i < 2; i++) {
        ckproxy_proc_t *p = &g_ck[i];
        p->ckbin = ckbin;
        p->pool  = &cfg.pools[i];
        snprintf(p->sock, sizeof(p->sock), "%s/sock%c", base, 'A' + i);
        snprintf(p->cfg,  sizeof(p->cfg),  "%s/ckproxy%c.json", base, 'A' + i);
        snprintf(p->name, sizeof(p->name), "%s", nm[i]);
        ensure_dir(p->sock);
        if (ckproxy_config_write(p->pool, local_port[i], p->sock, p->cfg,
                                 err, sizeof(err)) != 0) {
            fprintf(stderr, "serpentx: %s\n", err);
            return 1;
        }
        spawn_one(p);
        if (p->pid < 0) { fprintf(stderr, "serpentx: spawn %s failed\n", nm[i]); return 1; }

        /* set the upstream the splitter connects to */
        char portbuf[16];
        snprintf(portbuf, sizeof(portbuf), "%d", local_port[i]);
        g_pool_host[i] = "127.0.0.1";
        g_pool_port[i] = strdup(portbuf);
    }

    /* Bring up the allocator + dashboard immediately so the UI is reachable
     * during startup (before the ckproxies finish connecting upstream). */
    alloc_init(&g_alloc, (uint8_t)cfg.ratio_a);
    fprintf(stderr, "serpentx: mode=%s ratio A=%d%%\n", cfg.mode, cfg.ratio_a);
    if (!strcmp(cfg.mode, "time_slice"))
        setup_timeslice(cfg.ratio_a, cfg.interval_ms);
    webui_boot(cfg.web_port);

    pthread_t mon;
    pthread_create(&mon, NULL, monitor_thread, NULL);
    pthread_detach(mon);

    /* wait for each ckproxy to start listening */
    for (int i = 0; i < 2; i++) {
        if (wait_port(g_pool_host[i], g_pool_port[i], 20000) != 0)
            fprintf(stderr, "serpentx: warning: ckproxy %s not listening yet\n", nm[i]);
        else
            fprintf(stderr, "serpentx: ckproxy %s up on %s\n", nm[i], g_pool_port[i]);
    }

    return run_accept_loop(cfg.stratum_port);
}

/* -------- CLI mode: point straight at two upstreams (T2 harness) -------- */

static int run_cli_mode(int listen_port, int ratio, char *a_arg, char *b_arg,
                        int web_port, const char *mode, int interval_ms)
{
    if (split_hostport(a_arg, &g_pool_host[POOL_A], &g_pool_port[POOL_A]) ||
        split_hostport(b_arg, &g_pool_host[POOL_B], &g_pool_port[POOL_B])) {
        fprintf(stderr, "bad host:port\n");
        return 2;
    }
    /* minimal config snapshot so the dashboard has something to show */
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.stratum_port = listen_port;
    g_cfg.web_port = web_port;
    g_cfg.ratio_a = ratio;
    snprintf(g_cfg.mode, sizeof(g_cfg.mode), "%s", mode);
    g_cfg.interval_ms = interval_ms;
    snprintf(g_cfg.pools[0].primary.url, sizeof(g_cfg.pools[0].primary.url), "%s:%s",
             g_pool_host[POOL_A], g_pool_port[POOL_A]);
    snprintf(g_cfg.pools[1].primary.url, sizeof(g_cfg.pools[1].primary.url), "%s:%s",
             g_pool_host[POOL_B], g_pool_port[POOL_B]);

    alloc_init(&g_alloc, (uint8_t)ratio);
    if (!strcmp(mode, "time_slice"))
        setup_timeslice(ratio, interval_ms);
    webui_boot(web_port);
    return run_accept_loop(listen_port);
}

int main(int argc, char **argv)
{
    fprintf(stderr, "SerpentX %s — Dual-Pool Stratum Proxy\n", SERPENTX_VERSION);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, sighup_handler);   /* reload config on SIGHUP */
    int listen_port = 3333, ratio = 50, web_port = 0, interval_ms = 180000;
    char *a_arg = NULL, *b_arg = NULL, *config_path = NULL, *mode = "farm_split";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) config_path = argv[++i];
        else if (!strcmp(argv[i], "--listen") && i + 1 < argc) listen_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ratio") && i + 1 < argc) ratio = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--poolA") && i + 1 < argc) a_arg = argv[++i];
        else if (!strcmp(argv[i], "--poolB") && i + 1 < argc) b_arg = argv[++i];
        else if (!strcmp(argv[i], "--web") && i + 1 < argc) web_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--webroot") && i + 1 < argc)
            snprintf(g_webroot, sizeof(g_webroot), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval_ms = atoi(argv[++i]);
    }

    if (config_path)
        return run_config_mode(config_path);

    if (!a_arg || !b_arg) {
        fprintf(stderr,
            "usage:\n"
            "  %s --config /config/config.json           (production: spawns 2 ckproxy)\n"
            "  %s --listen P --poolA h:p --poolB h:p --ratio N [--web P] [--webroot DIR]\n"
            "     [--mode farm_split|time_slice] [--interval MS]\n",
            argv[0], argv[0]);
        return 2;
    }
    return run_cli_mode(listen_port, ratio, a_arg, b_arg, web_port, mode, interval_ms);
}
