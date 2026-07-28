/*
 * share_accounting.c — difficulty-weighted per-pool share accounting core.
 * See share_accounting.h. Part of Dual-Pool Proxy. GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#include "share_accounting.h"
#include <string.h>

void share_totals_init(share_totals_t *t)
{
    memset(t, 0, sizeof(*t));
}

void share_session_init(share_session_t *s, pool_id_t pool)
{
    memset(s, 0, sizeof(*s));
    s->pool     = pool;
    s->cur_diff = 0.0;
}

void share_session_set_difficulty(share_session_t *s, double diff)
{
    s->cur_diff = diff;
}

void share_session_on_submit(share_session_t *s, long id)
{
    /* Find a free slot; if none, overwrite the oldest (slot 0 rotation). */
    int slot = -1, oldest = 0;
    for (int i = 0; i < SHARE_PENDING_MAX; i++) {
        if (!s->pending[i].used) { slot = i; break; }
    }
    if (slot < 0) slot = oldest;   /* overflow: reuse slot 0 */

    s->pending[slot].id   = id;
    s->pending[slot].diff = s->cur_diff;
    s->pending[slot].used = true;
}

share_result_t share_session_on_result(share_session_t *s, long id, bool accepted,
                                       share_totals_t *t)
{
    share_result_t r = { false, false, 0.0 };
    for (int i = 0; i < SHARE_PENDING_MAX; i++) {
        if (s->pending[i].used && s->pending[i].id == id) {
            double d = s->pending[i].diff;
            if (accepted) {
                t->accepted_diff[s->pool] += d;
                t->accepted_n[s->pool]    += 1;
            } else {
                t->rejected_diff[s->pool] += d;
                t->rejected_n[s->pool]    += 1;
            }
            s->pending[i].used = false;   /* consume: no double count */
            r.counted = true; r.accepted = accepted; r.diff = d;
            return r;
        }
    }
    /* Unknown or already-consumed id: ignore. */
    return r;
}
