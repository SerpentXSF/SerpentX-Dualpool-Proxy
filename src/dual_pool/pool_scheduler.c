/*
 * pool_scheduler.c — weighted error-diffusion (Bresenham) pool allocator.
 *
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy).
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

#include "pool_scheduler.h"
#include "dual_clamp.h"

// Error-diffusion (Bresenham-style) slice assignment: over a rolling cycle the
// fraction of slices given to Pool A converges to ratio_a percent.
static void advance_slice(pool_scheduler_t *s)
{
    s->acc += s->ratio_a;
    if (s->acc >= 100) { s->acc -= 100; s->current = POOL_A; }
    else               { s->current = POOL_B; }
}

void pool_scheduler_init(pool_scheduler_t *s, uint8_t ratio_a, uint16_t interval_ms, int64_t now_us)
{
    s->ratio_a       = dual_clamp_ratio(ratio_a);
    s->interval_ms   = dual_clamp_interval(interval_ms);
    s->acc           = 0;
    s->current       = POOL_A;
    s->slice_start_us = now_us;
    s->initialized   = false;
}

pool_id_t pool_scheduler_select(pool_scheduler_t *s, int64_t now_us)
{
    if (!s->initialized) {
        s->initialized = true;
        s->slice_start_us = now_us;
        advance_slice(s);
        return s->current;
    }

    int64_t elapsed_ms = (now_us - s->slice_start_us) / 1000;
    while (s->interval_ms > 0 && elapsed_ms >= s->interval_ms) {
        s->slice_start_us += (int64_t)s->interval_ms * 1000;
        elapsed_ms        -= s->interval_ms;
        advance_slice(s);
    }
    return s->current;
}
