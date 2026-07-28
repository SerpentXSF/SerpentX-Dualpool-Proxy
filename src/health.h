/*
 * health.h — per-pool up/down tracker for Dual-Pool Proxy, wrapping pool_failover.
 *
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3 (derivative of ckpool).
 * Copyright (C) 2025-2026 The SerpentX authors.
 *
 * A background probe reports each pool's reachability; this turns a stream of
 * connected/disconnected signals into a stable up/down verdict (tolerating a
 * few transient misses before declaring DOWN, and recovering on success). The
 * splitter uses the verdict to stop allocating to a dead pool (donation) and to
 * evict its pinned miners.
 */
#ifndef DUALPOOL_HEALTH_H
#define DUALPOOL_HEALTH_H

#include <stdbool.h>
#include "pool_scheduler.h"   /* pool_id_t */
#include "pool_failover.h"

typedef struct {
    pool_failover_t fo[2];
} health_t;

/* max_retries: consecutive failures tolerated before a pool is declared DOWN. */
void health_init(health_t *h, int max_retries);

/* Report the result of a reachability probe (or a real connect attempt). */
void health_report(health_t *h, pool_id_t p, bool connected);

/* True if the pool is usable (not DOWN). Pools start assumed-up. */
bool health_pool_up(const health_t *h, pool_id_t p);

#endif /* DUALPOOL_HEALTH_H */
