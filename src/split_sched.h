/*
 * split_sched.h — adaptive slice scheduler: slice-length sizing and
 * ratio-weighted pool selection. Pure, socket-free (clock injected).
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#ifndef DUALPOOL_SPLIT_SCHED_H
#define DUALPOOL_SPLIT_SCHED_H

#include <stdint.h>

/* Compute how long (in microseconds) the next slice on a pool should run,
 * targeting ~target_shares valid shares over the slice, clamped to
 * [min_s, max_s] seconds. hashrate is in H/s, diff in shares.
 * slice ~= target_shares * diff * 2^32 / hashrate (seconds).
 * Guard: hashrate <= 0 returns max_s seconds (in microseconds). */
int64_t split_sched_slice_us(double hashrate, double diff,
                              int target_shares, int min_s, int max_s);

/* Given the pool that just finished its slice (0=A, 1=B) and the running
 * per-pool cumulative time totals in microseconds, return the pool (0=A,
 * 1=B) to run next so that A's long-run time-share tracks ratio_a percent.
 * Ties alternate rather than repeating just_finished. ratio_a<=0 always
 * returns B; ratio_a>=100 always returns A. */
int split_sched_next_pool(int just_finished, int64_t a_us, int64_t b_us,
                           int ratio_a);

#endif /* DUALPOOL_SPLIT_SCHED_H */
