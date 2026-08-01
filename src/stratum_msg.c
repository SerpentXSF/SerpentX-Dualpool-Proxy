/*
 * stratum_msg.c — Stratum JSON message parser + emit helpers. See stratum_msg.h.
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#include "stratum_msg.h"
#include <jansson.h>
#include <string.h>
#include <stdio.h>

static void cpy(char *d, size_t n, json_t *v)
{
    const char *s = json_is_string(v) ? json_string_value(v) : "";
    snprintf(d, n, "%s", s ? s : "");
}

int stratum_msg_parse(const char *line, stratum_msg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->id = -1;
    json_error_t e;
    json_t *r = json_loads(line, 0, &e);
    if (!r) return -1;

    json_t *id = json_object_get(r, "id");
    if (json_is_integer(id)) out->id = json_integer_value(id);

    const char *meth = json_string_value(json_object_get(r, "method"));
    json_t *p = json_object_get(r, "params");

    if (meth && !strcmp(meth, "mining.notify")) {
        out->type = SM_NOTIFY;
        cpy(out->job_id, sizeof(out->job_id), json_array_get(p, 0));
        out->clean_jobs = json_is_true(json_array_get(p, 8));
    } else if (meth && !strcmp(meth, "mining.submit")) {
        out->type = SM_SUBMIT;
        cpy(out->worker, sizeof(out->worker), json_array_get(p, 0));
        cpy(out->job_id, sizeof(out->job_id), json_array_get(p, 1));
    } else if (meth && !strcmp(meth, "mining.set_difficulty")) {
        out->type = SM_SET_DIFFICULTY;
        out->diff = json_number_value(json_array_get(p, 0));
    } else if (meth && !strcmp(meth, "mining.set_extranonce")) {
        out->type = SM_SET_EXTRANONCE;
        cpy(out->enonce1, sizeof(out->enonce1), json_array_get(p, 0));
        out->n2len = (int)json_integer_value(json_array_get(p, 1));
    } else if (meth && !strcmp(meth, "mining.extranonce.subscribe")) {
        out->type = SM_EXTRANONCE_SUBSCRIBE;
    } else if (meth && !strcmp(meth, "mining.subscribe")) {
        out->type = SM_SUBSCRIBE;
    } else if (meth && !strcmp(meth, "mining.authorize")) {
        out->type = SM_AUTHORIZE;
        cpy(out->worker, sizeof(out->worker), json_array_get(p, 0));
    } else if (meth && !strcmp(meth, "mining.configure")) {
        out->type = SM_CONFIGURE;
    } else if (json_object_get(r, "result")) {
        out->type = SM_RESULT;
    }

    json_decref(r);
    return 0;
}

int sm_emit_set_extranonce(char *buf, size_t n, const char *enonce1, int n2len)
{
    int len = snprintf(buf, n,
        "{\"id\":null,\"method\":\"mining.set_extranonce\",\"params\":[\"%s\",%d]}",
        enonce1, n2len);
    if (len < 0 || (size_t)len >= n) return -1;
    return len;
}

int sm_emit_set_difficulty(char *buf, size_t n, double diff)
{
    int len = snprintf(buf, n,
        "{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[%.17g]}",
        diff);
    if (len < 0 || (size_t)len >= n) return -1;
    return len;
}

int sm_emit_set_version_mask(char *buf, size_t n, uint32_t mask)
{
    int len = snprintf(buf, n,
        "{\"id\":null,\"method\":\"mining.set_version_mask\",\"params\":[\"%08x\"]}",
        (unsigned)mask);
    if (len < 0 || (size_t)len >= n) return -1;
    return len;
}

int sm_emit_suggest_difficulty(char *buf, size_t n, int64_t id, double diff)
{
    int len = snprintf(buf, n,
        "{\"id\":%lld,\"method\":\"mining.suggest_difficulty\",\"params\":[%.17g]}",
        (long long)id, diff);
    if (len < 0 || (size_t)len >= n) return -1;
    return len;
}
