/*
 * test_share.c — Dual-Pool Proxy host tests for difficulty-weighted per-pool share
 * accounting. GPLv3.
 *
 * Raw accepted-share COUNTS are meaningless across two pools with different
 * vardiff. These tests pin that every share is weighted by the session
 * difficulty *at the moment it was submitted*, and that submit/result pairs are
 * correlated by JSON-RPC id and tallied to the owning pool.
 */
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "share_accounting.h"

static void test_accept_and_reject_weighted(void) {
    share_totals_t tot; share_totals_init(&tot);
    share_session_t s; share_session_init(&s, POOL_A);

    share_session_set_difficulty(&s, 1000.0);
    share_session_on_submit(&s, 1);
    share_session_on_result(&s, 1, true, &tot);     /* accepted @1000 */

    share_session_set_difficulty(&s, 500.0);
    share_session_on_submit(&s, 2);
    share_session_on_result(&s, 2, false, &tot);    /* rejected @500 */

    assert(fabs(tot.accepted_diff[POOL_A] - 1000.0) < 1e-9);
    assert(fabs(tot.rejected_diff[POOL_A] - 500.0) < 1e-9);
    assert(tot.accepted_n[POOL_A] == 1);
    assert(tot.rejected_n[POOL_A] == 1);
    assert(tot.accepted_n[POOL_B] == 0 && tot.rejected_n[POOL_B] == 0);
}

/* The share is weighted by the difficulty in force WHEN IT WAS SUBMITTED, even
 * if difficulty changes before the result comes back. */
static void test_difficulty_at_submit_time(void) {
    share_totals_t tot; share_totals_init(&tot);
    share_session_t s; share_session_init(&s, POOL_B);

    share_session_set_difficulty(&s, 1000.0);
    share_session_on_submit(&s, 10);                /* pending @1000 */
    share_session_set_difficulty(&s, 4000.0);       /* diff changes... */
    share_session_on_submit(&s, 11);                /* pending @4000 */
    share_session_on_result(&s, 10, true, &tot);    /* id10 counts @1000 */
    share_session_on_result(&s, 11, true, &tot);    /* id11 counts @4000 */

    assert(fabs(tot.accepted_diff[POOL_B] - 5000.0) < 1e-9);
    assert(tot.accepted_n[POOL_B] == 2);
}

/* A result for an unknown / already-consumed id is ignored (no double count). */
static void test_unknown_result_ignored(void) {
    share_totals_t tot; share_totals_init(&tot);
    share_session_t s; share_session_init(&s, POOL_A);

    share_session_set_difficulty(&s, 256.0);
    share_session_on_submit(&s, 7);
    share_session_on_result(&s, 7, true, &tot);     /* counted */
    share_session_on_result(&s, 7, true, &tot);     /* duplicate -> ignored */
    share_session_on_result(&s, 999, false, &tot);  /* never submitted -> ignored */

    assert(fabs(tot.accepted_diff[POOL_A] - 256.0) < 1e-9);
    assert(tot.accepted_n[POOL_A] == 1);
    assert(tot.rejected_n[POOL_A] == 0);
}

/* Two sessions on different pools aggregate into the same totals independently. */
static void test_two_pools_aggregate(void) {
    share_totals_t tot; share_totals_init(&tot);
    share_session_t a; share_session_init(&a, POOL_A);
    share_session_t b; share_session_init(&b, POOL_B);

    share_session_set_difficulty(&a, 100.0);
    share_session_set_difficulty(&b, 300.0);
    share_session_on_submit(&a, 1); share_session_on_result(&a, 1, true, &tot);
    share_session_on_submit(&b, 1); share_session_on_result(&b, 1, true, &tot);

    assert(fabs(tot.accepted_diff[POOL_A] - 100.0) < 1e-9);
    assert(fabs(tot.accepted_diff[POOL_B] - 300.0) < 1e-9);
}

int main(void) {
    test_accept_and_reject_weighted();
    test_difficulty_at_submit_time();
    printf("share: weighting + submit-time diff passed\n");
    test_unknown_result_ignored();
    test_two_pools_aggregate();
    printf("share: dedupe + multi-pool passed\n");
    printf("ALL share host tests passed\n");
    return 0;
}
