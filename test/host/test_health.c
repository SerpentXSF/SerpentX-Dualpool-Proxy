/*
 * test_health.c — Dual-Pool Proxy tests for the pool health tracker (wraps
 * pool_failover). GPLv3. Pure logic, no sockets/jansson.
 */
#include <assert.h>
#include <stdio.h>
#include "health.h"

/* A pool starts assumed-up, goes DOWN after enough consecutive failures, and
 * comes back UP on a successful probe (recovery). */
static void test_down_then_recover(void) {
    health_t h;
    health_init(&h, 1);                 /* tolerate 1 retry before DOWN */

    assert(health_pool_up(&h, POOL_A) == true);
    assert(health_pool_up(&h, POOL_B) == true);

    health_report(&h, POOL_A, false);   /* miss 1 */
    assert(health_pool_up(&h, POOL_A) == true);   /* still within tolerance */
    health_report(&h, POOL_A, false);   /* miss 2 -> DOWN */
    assert(health_pool_up(&h, POOL_A) == false);
    assert(health_pool_up(&h, POOL_B) == true);   /* B unaffected */

    health_report(&h, POOL_A, true);    /* probe succeeds -> recover */
    assert(health_pool_up(&h, POOL_A) == true);
}

/* A successful probe resets the failure streak. */
static void test_intermittent_stays_up(void) {
    health_t h;
    health_init(&h, 1);
    health_report(&h, POOL_B, false);
    health_report(&h, POOL_B, true);    /* recovered before hitting DOWN */
    health_report(&h, POOL_B, false);
    assert(health_pool_up(&h, POOL_B) == true);
}

int main(void) {
    test_down_then_recover();
    test_intermittent_stays_up();
    printf("ALL health host tests passed\n");
    return 0;
}
