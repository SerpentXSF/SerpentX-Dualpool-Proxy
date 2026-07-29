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

   Precondition: the process must ignore SIGPIPE (the splitter does so at
   startup; the standalone test probe does the same). Writes to a peer that
   closed then surface as a normal error and unwind cleanly instead of killing
   the process. */
void splitmux_run(int down_fd, int up_fd[2], int ratio_a,
                  int target_shares, int min_s, int max_s);

#endif /* DUALPOOL_SPLITMUX_H */
