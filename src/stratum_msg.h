/*
 * stratum_msg.h — typed Stratum JSON message model (parse + emit). Pure,
 * socket-free (see docs/design/hashrate-split.md).
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#ifndef DUALPOOL_STRATUM_MSG_H
#define DUALPOOL_STRATUM_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { SM_UNKNOWN, SM_SUBSCRIBE, SM_AUTHORIZE, SM_CONFIGURE,
               SM_SUBMIT, SM_NOTIFY, SM_SET_DIFFICULTY, SM_SET_EXTRANONCE,
               SM_EXTRANONCE_SUBSCRIBE, SM_RESULT } sm_type_t;

typedef struct {
    sm_type_t type;
    int64_t   id;            /* JSON-RPC id, -1 if none */
    char      job_id[64];    /* notify/submit */
    bool      clean_jobs;    /* notify */
    double    diff;          /* set_difficulty */
    char      enonce1[64];   /* set_extranonce */
    int       n2len;         /* set_extranonce */
    char      worker[128];   /* submit */
} stratum_msg_t;

/* Parse one Stratum JSON line into `out`. Returns 0 on success (out
 * populated), -1 on parse failure. */
int stratum_msg_parse(const char *line, stratum_msg_t *out);

/* Emit helpers: format a Stratum JSON-RPC notification line into `buf`
 * (capacity n). Return bytes written (excl. NUL), or -1 on overflow. */
int sm_emit_set_extranonce(char *buf, size_t n, const char *enonce1, int n2len);
int sm_emit_set_difficulty(char *buf, size_t n, double diff);

/* Emit a mining.set_version_mask notification carrying `mask` as 8 lowercase hex
 * digits (BIP310 ASICBoost version-rolling). Return bytes written, -1 on
 * overflow. */
int sm_emit_set_version_mask(char *buf, size_t n, uint32_t mask);

/* Emit a mining.suggest_difficulty REQUEST (it is a call, so it carries an id —
 * the mux stamps its own sentinel id so the pool's ack is recognisable as the
 * mux's own and never relayed to the miner). Return bytes written, -1 on
 * overflow. */
int sm_emit_suggest_difficulty(char *buf, size_t n, int64_t id, double diff);

#endif /* DUALPOOL_STRATUM_MSG_H */
