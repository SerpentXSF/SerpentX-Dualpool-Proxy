/*
 * splitmux.h — Stratum-aware multiplexer: relays one miner through one
 * upstream (M3). Groundwork for the dual-upstream swap (M4).
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#ifndef DUALPOOL_SPLITMUX_H
#define DUALPOOL_SPLITMUX_H

#include <stdbool.h>

/* Share-accounting hook. A split miner's shares are routed to whichever pool owns
   the job they were found on, so the mux — which relays every pool submit-ack back
   to the miner — is the only place that can attribute them. It calls this once per
   routed submit that the owning pool acks.

     pool     0 = A, 1 = B: the pool that ACKED (not the connection's start pool)
     accepted true on result:true, false on result:false / an error reply
     diff     the pool difficulty the share was submitted under (its weight)
     worker   the miner's authorize username, or "" if not yet known

   Runs on the mux's own thread; keep it short and lock-safe. NULL disables it. */
typedef void (*splitmux_share_cb)(void *ctx, int pool, bool accepted,
                                  double diff, const char *worker);

/* Runs one split-mode miner. down_fd = miner socket; up_fd[2] = the two ckproxy
   sockets. In M3 only up_fd[0] is used (up_fd[1] == -1). Blocks until the miner
   or the upstream disconnects, then returns. Does NOT close the fds.

   start_pool selects which pool the dual-mode session begins on:
     start_pool < 0        -> seed the active pool from ratio_a (M4 default)
     start_pool == 0 or 1  -> begin with that pool active. Used by the splitter
                              to alternate which pool a reconnecting fallback
                              (M5 reconnect-slice) miner lands on.

   M5 capability fallback: a miner that never sends mining.extranonce.subscribe
   is assumed not to honour mining.set_extranonce. For such a miner the mux does
   NOT emit a smooth set_extranonce swap; instead, at the slice deadline it
   shutdown()s down_fd and returns so the miner reconnects (and the splitter
   binds it to the next pool via start_pool). down_fd/up_fd are NOT closed.

   assume_ext (EXPERIMENTAL operator opt-in, default false at every call site):
   when true the session STARTS already marked set_extranonce-capable, so the
   deadline path takes the smooth swap even for a miner that never advertised
   the extension. Intended for ESP-Miner-derived firmware (BitAxe / Hammer /
   NerdAxe / NerdQAxe), which commonly honours mining.set_extranonce without
   sending mining.extranonce.subscribe. Nothing else changes — routing, grace,
   version-rolling, the FIX-11 fresh-job gate and share accounting are
   identical. With assume_ext == false the mux behaves exactly as before.
   Caveat: a miner that truly ignores set_extranonce will keep mining the old
   pool's extranonce1 after a swap and its shares will be rejected, which is why
   this is opt-in rather than the default.

   Asymmetric bring-up (dual mode): the PRIMARY pool is handshaked synchronously
   and the miner starts mining it immediately; the SECONDARY pool is brought up
   by a non-blocking state machine inside the poll loop and joins once ready,
   retrying with backoff if it isn't ready yet (a ckproxy in userproxy mode may
   not accept subscribes the instant the mux connects). up_addr[p] = "host:port"
   for pool p, used to RECONNECT the secondary between retry attempts. In
   single-pool mode (up_fd[1] == -1) up_addr is unused.

   Ownership: splitmux_run may close and reopen the secondary fd while retrying,
   so on return it writes the FINAL upstream fds back into up_fd[0]/up_fd[1]
   (a retired secondary that is mid-backoff is written back as -1). Callers MUST
   close up_fd[0]/up_fd[1] (the array they passed), NOT any stale copies.

   Precondition: the process must ignore SIGPIPE (the splitter does so at
   startup; the standalone test probe does the same). Writes to a peer that
   closed then surface as a normal error and unwind cleanly instead of killing
   the process. */
void splitmux_run(int down_fd, int up_fd[2], int ratio_a,
                  int target_shares, int min_s, int max_s, int start_pool,
                  const char *up_addr[2],
                  splitmux_share_cb share_cb, void *share_ctx,
                  bool assume_ext);

#endif /* DUALPOOL_SPLITMUX_H */
