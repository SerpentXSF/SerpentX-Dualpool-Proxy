#!/usr/bin/env python3
"""Fake split-mode Stratum miner for Dual-Pool Proxy M4 tests. GPLv3.

Connects, subscribes, authorizes, then for <seconds> follows the mux's
miner-facing session: it tracks the current difficulty, extranonce1 and job as
the mux forwards them, and submits a share against the *current* job every
~0.4 s using the *current* extranonce1. Because the mux swaps which pool it
presents, the miner naturally submits each pool's job while that pool is active
— which is exactly what makes per-pool routing observable.

Prints a line each time set_extranonce changes ("set_extranonce <enonce1>") so a
runner can confirm a swap actually reached the miner.

It also matches each mining.submit id to the pool's {"id",result} reply that the
mux relays back, and on exit prints a summary line

    acks=<n> accepted=<n> submits=<n>

so a runner can assert the miner actually saw its accepts (FIX-1): a real ASIC
disconnects a pool that never acknowledges shares.

    fake_miner_split.py <host> <port> <seconds>
"""
import json, select, socket, sys, time

def main():
    host, port, secs = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
    worker = sys.argv[4] if len(sys.argv) > 4 else "wallet.w1"
    try:
        s = socket.create_connection((host, port), timeout=5)
    except Exception:
        return 1

    def send(o):
        try:
            s.sendall((json.dumps(o) + "\n").encode())
        except Exception:
            pass

    # subscribe + capture the mux's synthesized extranonce1/n2size. The wait is
    # generous: on the single-pool DEGRADE path the mux only answers the miner
    # after its per-pool handshake timeout (~5s) elapses on the dead pool.
    send({"id": 1, "method": "mining.subscribe", "params": ["dualpool-split/1.0"]})
    s.settimeout(10)
    try:
        first = s.recv(65536)
    except Exception:
        return 2
    if not first:
        return 2
    cur_en, cur_n2 = "", 0
    buf = first
    # parse just the subscribe result line
    try:
        head, _, rest = buf.partition(b"\n")
        msg = json.loads(head.decode().strip())
        res = msg.get("result")
        if not (isinstance(res, list) and msg.get("id") == 1):
            return 2
        if len(res) >= 3:
            cur_en, cur_n2 = res[1], res[2]
        buf = rest
    except Exception:
        return 2

    # Advertise mining.set_extranonce support so the mux keeps the smooth-swap
    # path (M5 capability detection). A naive miner omits this and is instead
    # reconnect-sliced; see fake_miner_naive.py.
    send({"id": 3, "method": "mining.extranonce.subscribe", "params": []})
    send({"id": 2, "method": "mining.authorize", "params": [worker, "x"]})

    cur_diff, cur_job = 1, ""
    sid = 100
    end = time.time() + secs
    next_submit = time.time() + 0.4

    pending = set()      # submit ids sent, awaiting a reply
    acks = 0             # submit ids that got any {id,result} reply back
    accepted = 0         # of those, replies whose result was truthy
    submits = 0          # total submits sent

    def handle_line(l):
        nonlocal cur_diff, cur_en, cur_n2, cur_job, acks, accepted
        try:
            m = json.loads(l.decode().strip())
        except Exception:
            return
        meth = m.get("method")
        p = m.get("params") or []
        if meth == "mining.set_difficulty" and p:
            cur_diff = p[0]
        elif meth == "mining.set_extranonce" and p:
            ne = p[0]
            if ne != cur_en:
                cur_en = ne
                cur_n2 = p[1] if len(p) > 1 else cur_n2
                print(f"set_extranonce {cur_en}", flush=True)
        elif meth == "mining.notify" and p:
            cur_job = p[0]
        elif meth is None and ("result" in m):
            # A submit ack (or other result) relayed back by the mux (FIX-1).
            rid = m.get("id")
            if rid in pending:
                pending.discard(rid)
                acks += 1
                if m.get("result") is True:
                    accepted += 1

    s.setblocking(False)
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            try:
                data = s.recv(65536)
            except Exception:
                data = b""
            if not data:
                break
            buf += data
            while b"\n" in buf:
                l, buf = buf.split(b"\n", 1)
                handle_line(l)
        now = time.time()
        if now >= next_submit and cur_job:
            send({"id": sid, "method": "mining.submit",
                  "params": [worker, cur_job, "00000000", cur_en, "00000000"]})
            pending.add(sid)
            submits += 1
            sid += 1
            next_submit = now + 0.4

    # Drain any acks still in flight before we tear the socket down.
    drain_end = time.time() + 0.5
    while time.time() < drain_end and pending:
        r, _, _ = select.select([s], [], [], 0.1)
        if not r:
            continue
        try:
            data = s.recv(65536)
        except Exception:
            break
        if not data:
            break
        buf += data
        while b"\n" in buf:
            l, buf = buf.split(b"\n", 1)
            handle_line(l)

    print(f"acks={acks} accepted={accepted} submits={submits}", flush=True)

    try:
        s.close()
    except Exception:
        pass
    return 0

if __name__ == "__main__":
    sys.exit(main())
