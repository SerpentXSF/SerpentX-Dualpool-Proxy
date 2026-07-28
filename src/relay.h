/*
 * relay.h — transparent bidirectional byte relay between a miner (downstream)
 * and its assigned upstream (a ckproxy, or a pool directly in tests).
 *
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3 (derivative of ckpool).
 * Copyright (C) 2025-2026 The SerpentX authors.
 *
 * Farm-split pins a whole miner session to one upstream, so the relay is a
 * verbatim byte pump — ckproxy handles all Stratum semantics. Connection counts
 * are small (miners, not shares), so a poll()-based pump per connection is
 * simple and correct; the heavy multiplexing lives in ckproxy upstream.
 */
#ifndef DUALPOOL_RELAY_H
#define DUALPOOL_RELAY_H

#include <stddef.h>

/* Optional passive line-sniff callback. Called with each full '\n'-terminated
 * line seen in a given direction (a COPY — the bytes are still forwarded
 * verbatim regardless). May be NULL. `up` is 1 for downstream->upstream
 * (miner->pool), 0 for upstream->downstream. */
typedef void (*relay_sniff_fn)(void *ctx, int up, const char *line, size_t len);

/* Pump bytes both ways until either side closes or errors. Blocks the calling
 * thread. Does not close the fds. */
void relay_pump(int down_fd, int up_fd, relay_sniff_fn sniff, void *ctx);

#endif /* DUALPOOL_RELAY_H */
