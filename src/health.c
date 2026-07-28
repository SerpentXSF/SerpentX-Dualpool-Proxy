/*
 * health.c — per-pool up/down tracker. See health.h.
 * Part of SerpentX (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#include "health.h"

void health_init(health_t *h, int max_retries)
{
    /* has_failover=false: at the splitter tier each ckproxy owns its own
     * primary->failover; here we only track whether the pool is reachable. */
    for (int i = 0; i < 2; i++)
        pool_failover_init(&h->fo[i], max_retries, false);
}

void health_report(health_t *h, pool_id_t p, bool connected)
{
    if (p != POOL_A && p != POOL_B) return;
    pool_failover_step(&h->fo[p], connected ? PF_EV_CONNECTED : PF_EV_DISCONNECTED);
}

bool health_pool_up(const health_t *h, pool_id_t p)
{
    if (p != POOL_A && p != POOL_B) return false;
    return pool_failover_endpoint(&h->fo[p]) != -1;   /* -1 == PF_DOWN */
}
