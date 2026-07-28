/*
 * alloc.h — hashrate-weighted pool allocator for SerpentX farm-split mode.
 *
 * Part of SerpentX (Dual-Pool Stratum Proxy). GPLv3 (derivative of ckpool).
 * Copyright (C) 2025-2026 The SerpentX authors.
 *
 * Assigns each new/reconnecting miner to pool A or B so that the realized split
 * of *hashrate* (not connection count) converges on ratio_a. This is a weighted
 * generalization of the error-diffusion (Bresenham) scheme in pool_scheduler:
 * with equal weights it reproduces that per-unit distribution; with real
 * per-miner hashrate weights it tracks the ratio by work, which is what matters
 * for small mixed fleets (one S19 dwarfs three BitAxes).
 */
#ifndef SERPENTX_ALLOC_H
#define SERPENTX_ALLOC_H

#include <stdint.h>
#include <stdbool.h>
#include "pool_scheduler.h"   /* pool_id_t: POOL_A / POOL_B */

typedef struct {
    uint8_t ratio_a;         /* target Pool A share, percent [0..100] */
    double  assigned_a;      /* total weight assigned to A so far */
    double  assigned_total;  /* total weight assigned to A + B */
    bool    up[2];           /* pool availability, indexed by pool_id_t */
} alloc_t;

/* Initialize with a target Pool A percentage (clamped to [0,100]). */
void alloc_init(alloc_t *al, uint8_t ratio_a);

/* Mark a pool up/down. A down pool receives no new miners (donation): all new
 * miners go to the survivor until it recovers. */
void alloc_set_pool_up(alloc_t *al, pool_id_t pool, bool up);

/* Pick the pool for a new miner whose hashrate weight is `weight`
 * (e.g. GH/s; any consistent unit). weight 0 means "unknown yet" and is treated
 * as one unit. Updates internal accounting and returns POOL_A or POOL_B. */
pool_id_t alloc_pick(alloc_t *al, uint32_t weight);

#endif /* SERPENTX_ALLOC_H */
