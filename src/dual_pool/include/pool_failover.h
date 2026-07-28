/*
 * pool_failover.h — per-pool primary->failover->down state machine.
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

#ifndef POOL_FAILOVER_H
#define POOL_FAILOVER_H
#include <stdbool.h>

typedef enum {
    PF_TRY_PRIMARY,
    PF_ON_PRIMARY,
    PF_TRY_FAILOVER,
    PF_ON_FAILOVER,
    PF_DOWN
} pf_state_t;

typedef enum {
    PF_EV_CONNECTED,
    PF_EV_DISCONNECTED
} pf_event_t;

typedef struct {
    pf_state_t state;
    int  retry_count;
    int  max_retries;
    bool has_failover;
} pool_failover_t;

void pool_failover_init(pool_failover_t *f, int max_retries, bool has_failover);

// Which endpoint to attempt/use now: 0 = primary, 1 = failover, -1 = down.
int pool_failover_endpoint(const pool_failover_t *f);

void pool_failover_step(pool_failover_t *f, pf_event_t ev);

#endif // POOL_FAILOVER_H
