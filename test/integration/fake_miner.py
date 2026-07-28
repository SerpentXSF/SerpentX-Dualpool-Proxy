#!/usr/bin/env python3
"""Minimal fake Stratum miner for SerpentX T2 tests. Connects, subscribes,
authorizes, and submits N shares. GPLv3.

    fake_miner.py <host> <port> <n_shares>
"""
import json, socket, sys, time

def main():
    host, port, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    worker = sys.argv[4] if len(sys.argv) > 4 else "worker"
    hold = float(sys.argv[5]) if len(sys.argv) > 5 else 0.0   # keep conn open N s
    try:
        s = socket.create_connection((host, port), timeout=5)
    except Exception:
        return 1
    s.settimeout(5)
    f = s.makefile("rwb")

    def send(o):
        f.write((json.dumps(o) + "\n").encode()); f.flush()
    def recv():
        try:
            return f.readline()
        except Exception:
            return b""

    # subscribe: the reply comes from ckproxy (proving splitter->ckproxy plumbing).
    send({"id": 1, "method": "mining.subscribe", "params": ["serpentx-fake/1.0"]})
    line = recv()
    try:
        msg = json.loads(line.decode().strip())
        if msg.get("id") != 1 or msg.get("result") in (None, False):
            return 2   # no valid subscribe response through the chain
    except Exception:
        return 2

    send({"id": 2, "method": "mining.authorize", "params": [worker, "x"]})
    recv()
    for i in range(n):
        send({"id": 10 + i, "method": "mining.submit",
              "params": [worker, "job1", "00000000", "5e6f7788", "00000000"]})
        recv()
    if hold > 0:
        # Keep the connection open so an eviction (server shutdown) can be
        # observed: block on a read, which returns empty when we're evicted.
        s.settimeout(hold)
        try:
            f.read()
        except Exception:
            pass
    try:
        s.close()
    except Exception:
        pass
    return 0   # handshake through splitter+ckproxy succeeded

if __name__ == "__main__":
    sys.exit(main())
