/*
 * alloc.c — hashrate-weighted pool allocator. See alloc.h.
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#include "alloc.h"
#include "dual_clamp.h"
#include <math.h>

void alloc_init(alloc_t *al, uint8_t ratio_a)
{
    al->ratio_a        = dual_clamp_ratio(ratio_a);
    al->assigned_a     = 0.0;
    al->assigned_total = 0.0;
    al->up[POOL_A]     = true;
    al->up[POOL_B]     = true;
}

void alloc_set_pool_up(alloc_t *al, pool_id_t pool, bool up)
{
    if (pool == POOL_A || pool == POOL_B)
        al->up[pool] = up;
}

pool_id_t alloc_pick(alloc_t *al, uint32_t weight)
{
    double w = weight ? (double)weight : 1.0;
    pool_id_t choice;

    if (al->up[POOL_A] && !al->up[POOL_B]) {
        choice = POOL_A;                 /* B down -> donate to A */
    } else if (!al->up[POOL_A] && al->up[POOL_B]) {
        choice = POOL_B;                 /* A down -> donate to B */
    } else {
        /* Weighted error diffusion: keep e = assigned_a - f*total near zero.
         * Assigning to A moves e by +w(1-f); to B by -w*f. Pick the move that
         * leaves |e| smaller. Reduces to Bresenham when all weights are 1.
         * When both pools are down we still fall through here and pick the pool
         * that best tracks the ratio (the caller decides whether to reject). */
        double f  = al->ratio_a / 100.0;
        double e  = al->assigned_a - f * al->assigned_total;
        double eA = fabs(e + w * (1.0 - f));
        double eB = fabs(e - w * f);
        choice = (eA <= eB) ? POOL_A : POOL_B;
    }

    if (choice == POOL_A)
        al->assigned_a += w;
    al->assigned_total += w;
    return choice;
}
