/*
 * pool_failover.c — per-pool primary->failover->down state machine.
 *
 * Part of SerpentX (Dual-Pool Stratum Proxy).
 * Copyright (C) 2025-2026 The SerpentX authors.
 *
 * Provenance: originally written by the author for the BitAxe/NerdAxe ESP32
 * dual-pool mining firmware project (components/dual_pool). Relicensed to
 * GPLv3 for this project, which is a derivative of ckpool (GPLv3).
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. Distributed WITHOUT ANY WARRANTY; see the GNU General
 * Public License for details: <https://www.gnu.org/licenses/>.
 */

#include "pool_failover.h"

void pool_failover_init(pool_failover_t *f, int max_retries, bool has_failover)
{
    f->state = PF_TRY_PRIMARY;
    f->retry_count = 0;
    f->max_retries = max_retries < 0 ? 0 : max_retries;
    f->has_failover = has_failover;
}

int pool_failover_endpoint(const pool_failover_t *f)
{
    switch (f->state) {
        case PF_TRY_PRIMARY:
        case PF_ON_PRIMARY:   return 0;
        case PF_TRY_FAILOVER:
        case PF_ON_FAILOVER:  return 1;
        default:              return -1; // PF_DOWN
    }
}

void pool_failover_step(pool_failover_t *f, pf_event_t ev)
{
    if (ev == PF_EV_CONNECTED) {
        if (f->state == PF_TRY_PRIMARY || f->state == PF_ON_PRIMARY) {
            f->state = PF_ON_PRIMARY;
        } else {
            f->state = PF_ON_FAILOVER;
        }
        f->retry_count = 0;
        return;
    }

    // ev == PF_EV_DISCONNECTED
    switch (f->state) {
        case PF_ON_PRIMARY:
            f->state = PF_TRY_PRIMARY;
            f->retry_count = 1;
            break;
        case PF_TRY_PRIMARY:
            f->retry_count++;
            if (f->retry_count > f->max_retries) {
                if (f->has_failover) { f->state = PF_TRY_FAILOVER; f->retry_count = 0; }
                else                 { f->state = PF_DOWN; }
            }
            break;
        case PF_ON_FAILOVER:
            // failover dropped -> go back and probe primary
            f->state = PF_TRY_PRIMARY;
            f->retry_count = 0;
            break;
        case PF_TRY_FAILOVER:
            f->retry_count++;
            if (f->retry_count > f->max_retries) { f->state = PF_TRY_PRIMARY; f->retry_count = 0; }
            break;
        case PF_DOWN:
            f->state = PF_TRY_PRIMARY;
            f->retry_count = 0;
            break;
    }
}
