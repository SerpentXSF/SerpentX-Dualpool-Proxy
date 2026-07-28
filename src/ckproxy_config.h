/*
 * ckproxy_config.h — emit a stock-ckpool proxy config for one pool, and
 * spawn/supervise the ckproxy process for it.
 *
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3 (derivative of ckpool).
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#ifndef DUALPOOL_CKPROXY_CONFIG_H
#define DUALPOOL_CKPROXY_CONFIG_H

#include <sys/types.h>
#include <stddef.h>
#include "config.h"

/* Write a ckpool proxy config for `pool` to `path`:
 *   { "proxy": [ {url,auth,pass}, (failover) ... ],
 *     "serverurl": [ "127.0.0.1:<local_port>" ],
 *     "mindiff":1, "startdiff":42, "logdir": "<sockdir>" }
 * `local_port` is where ckproxy listens for the splitter; `sockdir` is its unix
 * socket / log dir. Returns 0 on success, -1 on error (msg in err). */
int ckproxy_config_write(const pool_cfg_t *pool, int local_port,
                         const char *sockdir, const char *path,
                         char *err, size_t errlen);

/* Fork+exec a stock ckproxy for the given config:
 *   ckpool {-p|-u} -k -c <config_path> -s <sockdir> -n <name>
 * (-u when pool->ckproxy_mode == "userproxy", else -p). Returns the child pid,
 * or -1 on error. `ckpool_bin` is the path to the ckpool binary. */
pid_t ckproxy_spawn(const char *ckpool_bin, const pool_cfg_t *pool,
                    const char *config_path, const char *sockdir,
                    const char *name);

#endif /* DUALPOOL_CKPROXY_CONFIG_H */
