/*
 * webui.c — minimal embedded HTTP server. See webui.h.
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#define _GNU_SOURCE
#include "webui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>

typedef struct {
    int port;
    char webroot[512];
    char password[128];
    webui_status_fn status_cb;
    webui_config_fn config_cb;
    webui_metrics_fn metrics_cb;
} webui_ctx_t;

static webui_ctx_t g_web;

/* Check the request for the shared key (header X-DualPool-Key or ?key=). Returns
 * 1 if auth passes (or no password configured), 0 otherwise. */
static int authorized(const char *req, const char *path)
{
    if (g_web.password[0] == '\0') return 1;   /* auth disabled */

    const char *h = strcasestr(req, "X-DualPool-Key:");
    if (h) {
        h += 15;
        while (*h == ' ') h++;
        size_t n = strlen(g_web.password);
        if (!strncmp(h, g_web.password, n) && (h[n] == '\r' || h[n] == '\n' || h[n] == '\0'))
            return 1;
    }
    const char *q = strstr(path, "key=");
    if (q) {
        q += 4;
        size_t n = strlen(g_web.password);
        if (!strncmp(q, g_web.password, n) && (q[n] == '\0' || q[n] == '&'))
            return 1;
    }
    return 0;
}

static void send_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return; }
        p += w; n -= (size_t)w;
    }
}

static void respond(int fd, const char *status, const char *ctype,
                    const char *body, size_t blen)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        status, ctype, blen);
    send_all(fd, hdr, (size_t)n);
    if (body && blen) send_all(fd, body, blen);
}

static const char *ctype_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".ico"))  return "image/x-icon";
    if (!strcmp(dot, ".css"))  return "text/css";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    return "application/octet-stream";
}

/* Serve a static file from webroot. Rejects any path containing "..". */
static void serve_static(int fd, const char *path)
{
    if (strstr(path, "..")) { respond(fd, "400 Bad Request", "text/plain", "bad path", 8); return; }

    char full[1024];
    if (!strcmp(path, "/") || !strcmp(path, ""))
        snprintf(full, sizeof(full), "%s/index.html", g_web.webroot);
    else
        snprintf(full, sizeof(full), "%s%s", g_web.webroot, path);

    FILE *f = fopen(full, "rb");
    if (!f) { respond(fd, "404 Not Found", "text/plain", "not found", 9); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); respond(fd, "500 Error", "text/plain", "err", 3); return; }
    char *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); respond(fd, "500 Error", "text/plain", "err", 3); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    respond(fd, "200 OK", ctype_for(full), buf, rd);
    free(buf);
}

static void *client_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;

    /* read request headers (up to blank line), then any body by Content-Length */
    char req[8192];
    size_t used = 0;
    while (used < sizeof(req) - 1) {
        ssize_t n = read(fd, req + used, sizeof(req) - 1 - used);
        if (n <= 0) { close(fd); return NULL; }
        used += (size_t)n;
        req[used] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }

    char method[8] = {0}, path[1024] = {0};
    sscanf(req, "%7s %1023s", method, path);

    /* protected endpoints require the key when a password is set */
    int is_api = !strncmp(path, "/api/", 5) || !strcmp(path, "/metrics");
    if (is_api && !authorized(req, path)) {
        respond(fd, "401 Unauthorized", "application/json", "{\"error\":\"unauthorized\"}", 23);
        close(fd);
        return NULL;
    }

    if (!strcmp(method, "GET") && !strncmp(path, "/api/status", 11)) {
        char *json = g_web.status_cb ? g_web.status_cb() : NULL;
        if (json) { respond(fd, "200 OK", "application/json", json, strlen(json)); free(json); }
        else respond(fd, "500 Error", "text/plain", "no status", 9);
    } else if (!strcmp(method, "GET") && !strncmp(path, "/metrics", 8)) {
        char *m = g_web.metrics_cb ? g_web.metrics_cb() : NULL;
        if (m) { respond(fd, "200 OK", "text/plain; version=0.0.4", m, strlen(m)); free(m); }
        else respond(fd, "404 Not Found", "text/plain", "no metrics", 10);
    } else if (!strcmp(method, "POST") && !strncmp(path, "/api/config", 11)) {
        char *body = strstr(req, "\r\n\r\n");
        body = body ? body + 4 : (char *)"";
        /* read remaining body if Content-Length exceeds what we have */
        long clen = 0;
        char *cl = strcasestr(req, "Content-Length:");
        if (cl) clen = atol(cl + 15);
        size_t have = strlen(body);
        char *full = malloc((size_t)clen + 1);
        if (full) {
            memcpy(full, body, have);
            while ((long)have < clen) {
                ssize_t n = read(fd, full + have, (size_t)clen - have);
                if (n <= 0) break;
                have += (size_t)n;
            }
            full[have] = '\0';
            int rc = g_web.config_cb ? g_web.config_cb(full) : -1;
            free(full);
            if (rc == 0) respond(fd, "200 OK", "application/json", "{\"ok\":true}", 11);
            else respond(fd, "400 Bad Request", "application/json", "{\"ok\":false}", 12);
        } else {
            respond(fd, "500 Error", "text/plain", "oom", 3);
        }
    } else if (!strcmp(method, "GET")) {
        serve_static(fd, path);
    } else {
        respond(fd, "405 Method Not Allowed", "text/plain", "no", 2);
    }

    close(fd);
    return NULL;
}

static void *listen_thread(void *arg)
{
    (void)arg;
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return NULL;
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t)g_web.port);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "dualpool: web bind :%d failed\n", g_web.port);
        close(lfd); return NULL;
    }
    listen(lfd, 64);
    fprintf(stderr, "dualpool: dashboard on :%d (webroot %s)\n", g_web.port, g_web.webroot);
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; break; }
        pthread_t t;
        if (pthread_create(&t, NULL, client_thread, (void *)(intptr_t)c) == 0)
            pthread_detach(t);
        else close(c);
    }
    close(lfd);
    return NULL;
}

int webui_start(int port, const char *webroot,
                webui_status_fn status_cb, webui_config_fn config_cb,
                webui_metrics_fn metrics_cb, const char *password)
{
    g_web.port = port;
    snprintf(g_web.webroot, sizeof(g_web.webroot), "%s", webroot);
    snprintf(g_web.password, sizeof(g_web.password), "%s", password ? password : "");
    g_web.status_cb = status_cb;
    g_web.config_cb = config_cb;
    g_web.metrics_cb = metrics_cb;

    pthread_t t;
    if (pthread_create(&t, NULL, listen_thread, NULL) != 0) return -1;
    pthread_detach(t);
    return 0;
}
