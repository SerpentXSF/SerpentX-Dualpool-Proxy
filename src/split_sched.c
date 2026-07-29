/*
 * split_sched.c — adaptive slice scheduler. See split_sched.h.
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#include "split_sched.h"

#define SPLIT_SCHED_TWO_POW_32 4294967296.0

int64_t split_sched_slice_us(double hashrate, double diff,
                              int target_shares, int min_s, int max_s)
{
    if (hashrate <= 0) return (int64_t)max_s * 1000000LL;

    double slice_s = target_shares * diff * SPLIT_SCHED_TWO_POW_32 / hashrate;
    if (slice_s < min_s) slice_s = min_s;
    if (slice_s > max_s) slice_s = max_s;

    return (int64_t)(slice_s * 1e6);
}

int split_sched_next_pool(int just_finished, int64_t a_us, int64_t b_us,
                           int ratio_a)
{
    if (ratio_a <= 0) return 1;   /* B always */
    if (ratio_a >= 100) return 0; /* A always */

    double target = ratio_a / 100.0;
    int64_t total = a_us + b_us;
    double a_frac = (total > 0) ? (double)a_us / (double)total : 0.5;

    if (a_frac < target) return 0;   /* A is under its target share */
    if (a_frac > target) return 1;   /* B is under its target share */

    /* Exact tie: alternate rather than repeat the pool that just finished. */
    return (just_finished == 0) ? 1 : 0;
}
