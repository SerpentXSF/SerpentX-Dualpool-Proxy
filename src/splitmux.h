/*
 * splitmux.h — Stratum-aware multiplexer: relays one miner through one
 * upstream (M3). Groundwork for the dual-upstream swap (M4).
 * Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
 * Copyright (C) 2025-2026 The SerpentX authors.
 */
#ifndef DUALPOOL_SPLITMUX_H
#define DUALPOOL_SPLITMUX_H

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
                  const char *up_addr[2]);

#endif /* DUALPOOL_SPLITMUX_H */
