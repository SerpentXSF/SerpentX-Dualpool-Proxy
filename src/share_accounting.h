/*
 * share_accounting.h — difficulty-weighted per-pool share accounting.
 *
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3 (derivative of ckpool).
 * Copyright (C) 2025-2026 The SerpentX authors.
 *
 * This is the PURE accounting core: it consumes already-decoded Stratum events
 * (set_difficulty, submit, result) and maintains per-pool weighted tallies. The
 * thin layer that extracts these events from raw JSON-RPC lines (jansson) lives
 * elsewhere and is exercised by the integration tests, so this logic stays
 * dependency-free and unit-testable.
 *
 * Accounting is per session (each miner connection has its own id space and
 * vardiff), aggregating into one shared totals struct. A share is weighted by
 * the session difficulty in force at the moment it was SUBMITTED, correlated to
 * its result by JSON-RPC id, and tallied to the pool the session is pinned to.
 */
#ifndef DUALPOOL_SHARE_ACCOUNTING_H
#define DUALPOOL_SHARE_ACCOUNTING_H

#include <stdbool.h>
#include <stdint.h>
#include "pool_scheduler.h"   /* pool_id_t: POOL_A / POOL_B */

/* Aggregate weighted counters, indexed by pool_id_t. */
typedef struct {
    double   accepted_diff[2];   /* sum of difficulty of accepted shares */
    double   rejected_diff[2];   /* sum of difficulty of rejected shares */
    uint64_t accepted_n[2];      /* accepted share count */
    uint64_t rejected_n[2];      /* rejected share count */
} share_totals_t;

/* Ring of outstanding submits awaiting a result. 64 is ample: shares are acked
 * long before a miner has 64 in flight. Oldest is overwritten on overflow. */
#define SHARE_PENDING_MAX 64

typedef struct {
    long   id;
    double diff;   /* difficulty at submit time */
    bool   used;
} share_pending_t;

typedef struct {
    pool_id_t       pool;      /* pool this session is pinned to (farm-split) */
    double          cur_diff;  /* current set_difficulty */
    share_pending_t pending[SHARE_PENDING_MAX];
} share_session_t;

void share_totals_init(share_totals_t *t);
void share_session_init(share_session_t *s, pool_id_t pool);

/* Update the session's current difficulty (from mining.set_difficulty). */
void share_session_set_difficulty(share_session_t *s, double diff);

/* Record a submit (mining.submit) with the given JSON-RPC id; snapshots the
 * current difficulty as this share's weight. */
void share_session_on_submit(share_session_t *s, long id);

/* Outcome of a result, so callers can also keep per-connection tallies. */
typedef struct {
    bool   counted;    /* the id matched a pending submit */
    bool   accepted;   /* accepted (true) or rejected (false); only if counted */
    double diff;       /* the share's difficulty weight; only if counted */
} share_result_t;

/* Record a result for a previously-submitted id. If the id is unknown or already
 * consumed, it is ignored (counted=false). Otherwise the share's snapshotted
 * difficulty is added to the accepted or rejected tally for this session's pool,
 * and the outcome is returned. */
share_result_t share_session_on_result(share_session_t *s, long id, bool accepted,
                                       share_totals_t *t);

#endif /* DUALPOOL_SHARE_ACCOUNTING_H */
