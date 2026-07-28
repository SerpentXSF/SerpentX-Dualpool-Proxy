/*
 * dual_clamp.c — input clamps for the dual-pool allocator.
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

#include "dual_clamp.h"

uint8_t dual_clamp_ratio(int32_t v)
{
    if (v < 0)   return 0;
    if (v > 100) return 100;
    return (uint8_t)v;
}

uint16_t dual_clamp_interval(int32_t v)
{
    if (v < 100)   return 100;
    if (v > 60000) return 60000;
    return (uint16_t)v;
}
