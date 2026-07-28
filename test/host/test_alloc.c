/*
 * test_alloc.c — SerpentX host tests for the hashrate-weighted allocator.
 * GPLv3.
 *
 * The whole point of this module: with mixed hardware, splitting by CONNECTION
 * COUNT gives the wrong hashrate split. These tests pin the weighted behavior.
 */
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "alloc.h"

/* Equal weights must reproduce pool_scheduler-style error diffusion. */
static void test_equal_weight_70_30(void) {
    alloc_t al;
    alloc_init(&al, 70);
    int a = 0, b = 0;
    for (int i = 0; i < 1000; i++)
        if (alloc_pick(&al, 1) == POOL_A) a++; else b++;
    assert(a >= 690 && a <= 710);        /* ~70% by count when weights equal */
    assert(a + b == 1000);
}

static void test_extremes(void) {
    alloc_t al;
    alloc_init(&al, 100);
    for (int i = 0; i < 50; i++) assert(alloc_pick(&al, 1) == POOL_A);
    alloc_init(&al, 0);
    for (int i = 0; i < 50; i++) assert(alloc_pick(&al, 1) == POOL_B);
}

/* The discriminating test: one 700 GH/s miner + seven 100 GH/s miners at 50/50.
 * Correct (hashrate-weighted) result: the big miner alone on A (700), the seven
 * small ones on B (700) => 50/50 by HASHRATE, even though A has 1 conn and B 7.
 * A connection-count allocator would put ~4 miners on each side => wrong split. */
static void test_hashrate_weighted_split(void) {
    alloc_t al;
    alloc_init(&al, 50);
    uint32_t weights[8] = {700, 100, 100, 100, 100, 100, 100, 100};
    double wa = 0, wtot = 0;
    int count_a = 0, count_b = 0;
    for (int i = 0; i < 8; i++) {
        pool_id_t p = alloc_pick(&al, weights[i]);
        wtot += weights[i];
        if (p == POOL_A) { wa += weights[i]; count_a++; } else { count_b++; }
    }
    /* realized HASHRATE split within 2% of target */
    assert(fabs(wa / wtot - 0.50) < 0.02);
    /* and it is NOT a connection-count split */
    assert(count_a == 1 && count_b == 7);
}

/* Weight 0 (unknown hashrate) is treated as one unit, so early miners before we
 * know their rate still diffuse sensibly. */
static void test_unknown_weight_is_unit(void) {
    alloc_t al;
    alloc_init(&al, 50);
    int a = 0;
    for (int i = 0; i < 100; i++) if (alloc_pick(&al, 0) == POOL_A) a++;
    assert(a >= 48 && a <= 52);
}

/* Donation: if a pool is down, every new miner goes to the survivor. */
static void test_donation_when_pool_down(void) {
    alloc_t al;
    alloc_init(&al, 70);
    alloc_set_pool_up(&al, POOL_B, false);        /* B dead */
    for (int i = 0; i < 20; i++) assert(alloc_pick(&al, 100) == POOL_A);
    alloc_set_pool_up(&al, POOL_A, false);        /* A dead too */
    alloc_set_pool_up(&al, POOL_B, true);         /* B back */
    for (int i = 0; i < 5; i++) assert(alloc_pick(&al, 100) == POOL_B);
}

int main(void) {
    test_equal_weight_70_30();
    test_extremes();
    printf("alloc: equal-weight + extremes passed\n");
    test_hashrate_weighted_split();
    test_unknown_weight_is_unit();
    printf("alloc: hashrate-weighted split passed\n");
    test_donation_when_pool_down();
    printf("alloc: donation passed\n");
    printf("ALL alloc host tests passed\n");
    return 0;
}
