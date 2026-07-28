/*
 * ckproxy_config.c — emit ckpool proxy configs + spawn ckproxy. See header.
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#define _GNU_SOURCE
#include "ckproxy_config.h"

#include <jansson.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

static json_t *endpoint_json(const endpoint_t *ep)
{
    json_t *o = json_object();
    json_object_set_new(o, "url",  json_string(ep->url));
    json_object_set_new(o, "auth", json_string(ep->user));
    json_object_set_new(o, "pass", json_string(ep->pass));
    return o;
}

int ckproxy_config_write(const pool_cfg_t *pool, int local_port,
                         const char *sockdir, const char *path,
                         char *err, size_t errlen)
{
    json_t *root  = json_object();
    json_t *proxy = json_array();

    /* One upstream proxy instance per pool. (Do NOT duplicate the primary to
     * create a second instance — classic ckpool v1.0.0 keys proxy notify
     * instances wrongly with multiple entries to the same pool, which corrupts
     * the job/extranonce a client mines against and makes every forwarded share
     * INVALID upstream. A real backup pool via `failover` is fine.) */
    json_array_append_new(proxy, endpoint_json(&pool->primary));
    if (pool->has_failover)
        json_array_append_new(proxy, endpoint_json(&pool->failover));
    json_object_set_new(root, "proxy", proxy);

    char surl[128];
    snprintf(surl, sizeof(surl), "127.0.0.1:%d", local_port);
    json_t *serverurl = json_array();
    json_array_append_new(serverurl, json_string(surl));
    json_object_set_new(root, "serverurl", serverurl);

    /* Per-pool difficulty, falling back to low defaults tuned for small ASICs.
     * A pool that enforces a higher floor (e.g. public-pool.io ~100000) sets its
     * own startdiff/mindiff in the pool config; ckproxy still adopts an even
     * higher upstream-dictated diff on top of this. */
    int startdiff = (pool->startdiff > 0) ? pool->startdiff : 42;
    int mindiff   = (pool->mindiff   > 0) ? pool->mindiff   : 1;
    json_object_set_new(root, "update_interval", json_integer(30));
    json_object_set_new(root, "mindiff",  json_integer(mindiff));
    json_object_set_new(root, "startdiff", json_integer(startdiff));
    json_object_set_new(root, "maxdiff",  json_integer(0));
    json_object_set_new(root, "logdir",   json_string(sockdir ? sockdir : "logs"));

    int rc = json_dump_file(root, path, JSON_INDENT(2));
    json_decref(root);
    if (rc != 0) {
        if (err && errlen) snprintf(err, errlen, "failed writing %s", path);
        return -1;
    }
    return 0;
}

pid_t ckproxy_spawn(const char *ckpool_bin, const pool_cfg_t *pool,
                    const char *config_path, const char *sockdir,
                    const char *name)
{
    const char *mode_flag =
        (strcmp(pool->ckproxy_mode, "userproxy") == 0) ? "-u" : "-p";

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* child: redirect ckproxy's chatty console output to a log file so it
         * doesn't flood the container console. Append (not truncate) so a death
         * message survives a respawn for diagnosis. */
        char logpath[300];
        snprintf(logpath, sizeof(logpath), "%s/console.log", sockdir);
        int lf = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (lf >= 0) {
            dprintf(lf, "\n===== %s starting (pid via fork) =====\n", name);
            dup2(lf, 1); dup2(lf, 2); if (lf > 2) close(lf);
        }

        /* exec stock ckproxy */
        execl(ckpool_bin, ckpool_bin, mode_flag, "-k",
              "-c", config_path, "-s", sockdir, "-n", name, (char *)NULL);
        /* only reached on exec failure */
        fprintf(stderr, "dualpool: exec %s failed\n", ckpool_bin);
        _exit(127);
    }
    return pid;
}
