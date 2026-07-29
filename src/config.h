/*
 * config.h — Dual-Pool Proxy configuration model + JSON parser (jansson).
 *
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3 (derivative of ckpool).
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#ifndef DUALPOOL_CONFIG_H
#define DUALPOOL_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char url[256];    /* host:port */
    char user[256];   /* wallet.worker or pool username */
    char pass[128];
} endpoint_t;

typedef struct {
    endpoint_t primary;
    bool       has_failover;
    endpoint_t failover;
    char       ckproxy_mode[16];   /* "proxy" or "userproxy" (default userproxy) */
    int        startdiff;          /* per-pool ckproxy startdiff; 0 => built-in default */
    int        mindiff;            /* per-pool ckproxy mindiff;   0 => built-in default */
} pool_cfg_t;

typedef struct {
    int  stratum_port;    /* downstream miners (default 3333) */
    int  web_port;        /* dashboard (default 8080) */
    int  ratio_a;         /* Pool A target percent [0..100] */
    char mode[16];        /* "farm_split" | "time_slice" | "hashrate_split" */
    int  interval_ms;     /* time_slice slice length (default 180000) */
    int  target_shares;   /* hashrate_split: shares per slice (default 10) */
    int  min_slice_s;     /* hashrate_split: min slice seconds (default 10) */
    int  max_slice_s;     /* hashrate_split: max slice seconds (default 120) */
    char web_password[128];
    pool_cfg_t pools[2];
} dualpool_config_t;

/* Parse from a JSON string / file into `out`. Returns 0 on success, -1 on error
 * with a human-readable message in err (if err != NULL). ratio_a and interval_ms
 * are clamped; optional fields get defaults; exactly two pools are required. */
int config_parse_string(const char *json, dualpool_config_t *out,
                        char *err, size_t errlen);
int config_parse_file(const char *path, dualpool_config_t *out,
                      char *err, size_t errlen);

/* Clamp the hashrate_split slice knobs to sane ranges in place and enforce
 * min_slice_s <= max_slice_s. Shared by the JSON config parser and the CLI
 * (atoi) path so an out-of-range knob (e.g. max_slice_s <= 0, which makes every
 * deadline already-past -> perpetual swap churn) cannot reach the scheduler.
 *   target_shares -> [1,1000], min_slice_s -> [1,3600], max_slice_s -> [1,3600];
 * an inverted pair raises max up to min. */
void config_clamp_slice_knobs(int *target_shares, int *min_slice_s,
                              int *max_slice_s);

#endif /* DUALPOOL_CONFIG_H */
