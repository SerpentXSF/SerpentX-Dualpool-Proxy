/*
 * webui.h — minimal embedded HTTP server for the Dual-Pool Proxy dashboard.
 *
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3 (derivative of ckpool).
 * Copyright (C) 2025-2026 The SerpentX authors.
 *
 * Serves the static dashboard from `webroot` and two JSON endpoints via
 * callbacks: GET /api/status and POST /api/config. Self-contained (no external
 * HTTP library); thread-per-request; intended for a LAN management UI.
 */
#ifndef DUALPOOL_WEBUI_H
#define DUALPOOL_WEBUI_H

/* Build the status JSON. Must return a malloc'd, NUL-terminated string that the
 * server will free. */
typedef char *(*webui_status_fn)(void);

/* Apply a posted JSON config body. Return 0 on success, non-zero on bad input. */
typedef int (*webui_config_fn)(const char *body);

/* Build the Prometheus text exposition. Returns a malloc'd string, freed by the
 * server. May be NULL to disable /metrics. */
typedef char *(*webui_metrics_fn)(void);

/* Start the dashboard server on `port`, serving files under `webroot`. If
 * `password` is non-empty, the /api endpoints and /metrics require it (header
 * `X-DualPool-Key: <password>` or query `?key=<password>`). Spawns a detached
 * listener thread. Returns 0 on success, -1 if it can't bind. */
int webui_start(int port, const char *webroot,
                webui_status_fn status_cb, webui_config_fn config_cb,
                webui_metrics_fn metrics_cb, const char *password);

#endif /* DUALPOOL_WEBUI_H */
