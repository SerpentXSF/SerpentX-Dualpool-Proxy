/*
 * dual_clamp.h — input clamps for the dual-pool allocator.
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
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details. You should have received a copy of the GNU General Public
 * License along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef DUAL_CLAMP_H
#define DUAL_CLAMP_H
#include <stdint.h>

// Clamp a Pool A share percentage into [0, 100].
uint8_t dual_clamp_ratio(int32_t v);

// Clamp a slice interval (ms) into [100, 60000].
uint16_t dual_clamp_interval(int32_t v);

#endif // DUAL_CLAMP_H
