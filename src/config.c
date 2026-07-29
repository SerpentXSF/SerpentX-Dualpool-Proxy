/*
 * config.c — Dual-Pool Proxy config parser. See config.h.
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#include "config.h"
#include "dual_clamp.h"

#include <jansson.h>
#include <string.h>
#include <stdio.h>

static void set_err(char *err, size_t n, const char *msg)
{
    if (err && n) { snprintf(err, n, "%s", msg); }
}

static void copy_str(char *dst, size_t cap, json_t *v, const char *dflt)
{
    const char *s = (v && json_is_string(v)) ? json_string_value(v) : dflt;
    if (!s) s = "";
    snprintf(dst, cap, "%s", s);
}

/* Parse one endpoint object {url,user,pass}. url is required. */
static int parse_endpoint(json_t *o, endpoint_t *ep, char *err, size_t errlen)
{
    if (!json_is_object(o)) { set_err(err, errlen, "pool entry not an object"); return -1; }
    json_t *url = json_object_get(o, "url");
    if (!json_is_string(url)) { set_err(err, errlen, "pool missing url"); return -1; }
    copy_str(ep->url,  sizeof(ep->url),  url, "");
    copy_str(ep->user, sizeof(ep->user), json_object_get(o, "user"), "");
    copy_str(ep->pass, sizeof(ep->pass), json_object_get(o, "pass"), "x");
    return 0;
}

static int int_or(json_t *o, const char *key, int dflt);

static int parse_pool(json_t *o, pool_cfg_t *p, char *err, size_t errlen)
{
    if (parse_endpoint(o, &p->primary, err, errlen) != 0) return -1;

    json_t *fo = json_object_get(o, "failover");
    if (fo && json_is_object(fo)) {
        if (parse_endpoint(fo, &p->failover, err, errlen) != 0) return -1;
        p->has_failover = true;
    } else {
        p->has_failover = false;
    }

    json_t *mode = json_object_get(o, "ckproxy_mode");
    const char *m = (mode && json_is_string(mode)) ? json_string_value(mode) : "userproxy";
    if (strcmp(m, "proxy") != 0 && strcmp(m, "userproxy") != 0) m = "userproxy";
    snprintf(p->ckproxy_mode, sizeof(p->ckproxy_mode), "%s", m);

    /* Optional per-pool difficulty. 0 (or absent/negative) => use ckproxy_config's
     * built-in default. Lets each upstream run at the difficulty it requires — e.g.
     * public-pool.io needs ~100000 while a low-diff pool is fine at the default. */
    p->startdiff = int_or(o, "startdiff", 0);
    if (p->startdiff < 0) p->startdiff = 0;
    p->mindiff = int_or(o, "mindiff", 0);
    if (p->mindiff < 0) p->mindiff = 0;
    return 0;
}

static int int_or(json_t *o, const char *key, int dflt)
{
    json_t *v = json_object_get(o, key);
    return (v && json_is_integer(v)) ? (int)json_integer_value(v) : dflt;
}

/* Proxy time-slice intervals are minute-scale (unlike the firmware's sub-second
 * slices), so we clamp to [1s, 1h] here rather than reusing dual_clamp_interval. */
static int clamp_interval_ms(int v)
{
    if (v < 1000)    return 1000;
    if (v > 3600000) return 3600000;
    return v;
}

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void config_clamp_slice_knobs(int *target_shares, int *min_slice_s,
                              int *max_slice_s)
{
    *target_shares = clampi(*target_shares, 1, 1000);
    *min_slice_s   = clampi(*min_slice_s, 1, 3600);
    *max_slice_s   = clampi(*max_slice_s, 1, 3600);
    if (*min_slice_s > *max_slice_s)   /* inverted -> raise max up to min */
        *max_slice_s = *min_slice_s;
}

int config_parse_string(const char *json, dualpool_config_t *out,
                        char *err, size_t errlen)
{
    memset(out, 0, sizeof(*out));

    json_error_t je;
    json_t *root = json_loads(json, 0, &je);
    if (!root) { set_err(err, errlen, je.text); return -1; }

    int rc = -1;

    /* downstream ports */
    json_t *ds = json_object_get(root, "downstream");
    out->stratum_port = ds ? int_or(ds, "stratum_port", 3333) : 3333;
    out->web_port     = ds ? int_or(ds, "web_port", 8080) : 8080;

    /* mode / ratio / interval */
    copy_str(out->mode, sizeof(out->mode), json_object_get(root, "mode"), "farm_split");
    if (strcmp(out->mode, "farm_split") != 0 && strcmp(out->mode, "time_slice") != 0 &&
        strcmp(out->mode, "hashrate_split") != 0)
        snprintf(out->mode, sizeof(out->mode), "%s", "farm_split");
    out->ratio_a     = dual_clamp_ratio(int_or(root, "ratio_a", 50));
    out->interval_ms = clamp_interval_ms(int_or(root, "interval_ms", 180000));

    /* hashrate_split slice knobs (splitter-level, not per-pool). Defaults chosen
     * so a single miner cycles pools every ~10 shares, clamped to 10..120 s. */
    out->target_shares = int_or(root, "target_shares", 10);
    out->min_slice_s   = int_or(root, "min_slice_s", 10);
    out->max_slice_s   = int_or(root, "max_slice_s", 120);
    config_clamp_slice_knobs(&out->target_shares, &out->min_slice_s,
                             &out->max_slice_s);
    copy_str(out->web_password, sizeof(out->web_password),
             json_object_get(root, "web_password"), "");

    /* pools: exactly two required */
    json_t *pools = json_object_get(root, "pools");
    if (!json_is_array(pools) || json_array_size(pools) < 2) {
        set_err(err, errlen, "config needs a 'pools' array with two entries");
        goto done;
    }
    if (parse_pool(json_array_get(pools, 0), &out->pools[0], err, errlen) != 0) goto done;
    if (parse_pool(json_array_get(pools, 1), &out->pools[1], err, errlen) != 0) goto done;

    rc = 0;
done:
    json_decref(root);
    return rc;
}

int config_parse_file(const char *path, dualpool_config_t *out,
                      char *err, size_t errlen)
{
    json_error_t je;
    json_t *root = json_load_file(path, 0, &je);
    if (!root) { set_err(err, errlen, je.text); return -1; }
    char *dump = json_dumps(root, JSON_COMPACT);
    json_decref(root);
    if (!dump) { set_err(err, errlen, "config re-encode failed"); return -1; }
    int rc = config_parse_string(dump, out, err, errlen);
    free(dump);
    return rc;
}
