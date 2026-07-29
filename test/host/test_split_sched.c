/*
 * test_split_sched.c — unit tests for the adaptive slice scheduler.
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#include <assert.h>
#include <stdio.h>
#include "split_sched.h"

static void test_slice(void){
    /* 1 TH/s, diff 1000, target 10 -> 10*1000*2^32/1e12 ≈ 42.9s -> in range */
    int64_t us = split_sched_slice_us(1e12, 1000, 10, 10, 120);
    assert(us >= 40*1000000LL && us <= 45*1000000LL);
    /* high diff clamps to max */
    assert(split_sched_slice_us(1e12, 1e9, 10, 10, 120) == 120LL*1000000);
    /* tiny work clamps to min */
    assert(split_sched_slice_us(1e15, 1, 10, 10, 120) == 10LL*1000000);
    /* non-positive hashrate guard -> max_s */
    assert(split_sched_slice_us(0, 1000, 10, 10, 120) == 120LL*1000000);
    assert(split_sched_slice_us(-5, 1000, 10, 10, 120) == 120LL*1000000);
}

static void test_next_pool_5050_alternates(void){
    /* equal totals, 50/50 target -> ties, and must alternate rather than
     * repeat the pool that just finished. */
    int64_t a_us = 1000000, b_us = 1000000;
    int p1 = split_sched_next_pool(0, a_us, b_us, 50);
    int p2 = split_sched_next_pool(p1, a_us, b_us, 50);
    assert(p1 != p2);
    int p3 = split_sched_next_pool(p2, a_us, b_us, 50);
    assert(p3 != p2);
}

static void test_next_pool_70_30_ratio(void){
    /* Simulate ~20 equal-length slices; the running A time-share should
     * track the 70% target within +/-0.1. */
    int64_t a_us = 0, b_us = 0;
    int last = 1; /* pretend B just finished so the first pick can be A */
    for (int i = 0; i < 20; i++) {
        int next = split_sched_next_pool(last, a_us, b_us, 70);
        int64_t slice = 1000000; /* 1s equal slice for this simulation */
        if (next == 0) a_us += slice; else b_us += slice;
        last = next;
    }
    double frac = (double)a_us / (double)(a_us + b_us);
    assert(frac > 0.6 && frac < 0.8);
}

static void test_next_pool_ratio_extremes(void){
    /* ratio_a==100 -> always A; ratio_a==0 -> always B, even repeating. */
    int64_t a_us = 5000000, b_us = 5000000;
    assert(split_sched_next_pool(0, a_us, b_us, 100) == 0);
    assert(split_sched_next_pool(0, a_us, b_us, 100) == 0);
    assert(split_sched_next_pool(1, a_us, b_us, 0) == 1);
    assert(split_sched_next_pool(1, a_us, b_us, 0) == 1);
}

int main(void){
    test_slice();
    test_next_pool_5050_alternates();
    test_next_pool_70_30_ratio();
    test_next_pool_ratio_extremes();
    printf("split_sched: sizing + ratio-weighted selection passed\n");
    return 0;
}
