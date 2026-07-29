#!/usr/bin/env python3
"""Fake "naive" Stratum miner for Dual-Pool Proxy M5 tests. GPLv3.

Models an ASIC firmware that does NOT support the mining.set_extranonce
extension:
  * it NEVER sends mining.extranonce.subscribe (so the mux must treat it as
    incapable and fall back to reconnect-slicing);
  * it IGNORES any mining.set_extranonce the mux might send — it keeps the
    extranonce1 it was handed in the subscribe reply for the life of the
    connection;
  * when the server drops the connection (the mux's reconnect-slice fallback
    shutdown()s it at the slice deadline) it RECONNECTS and starts a fresh
    session, exactly like a real miner recovering from a pool drop.

Across the whole run it prints, on exit, a summary line

    set_extranonce_seen=<n> conns=<n> submits=<n> acks=<n>

so a runner can assert set_extranonce_seen == 0 (the mux never tried the smooth
swap on this miner) while both pools still received shares across reconnects.

Each individual mining.set_extranonce that (wrongly) reaches this miner is also
printed verbatim as "set_extranonce <enonce1>", matching fake_miner_split.py, so
`grep -c set_extranonce` is a direct RED/GREEN signal.

    fake_miner_naive.py <host> <port> <seconds> [worker]
"""
import json, select, socket, sys, time

def main():
    host, port, secs = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
    worker = sys.argv[4] if len(sys.argv) > 4 else "wallet.w1"
    deadline = time.time() + secs

    set_extranonce_seen = 0
    conns = 0
    submits = 0
    acks = 0

    def run_one_session(time_left):
        """One connection: subscribe/authorize, submit until dropped or time up.
        Returns (saw_set_extranonce_count, submits, acks). Never sends
        mining.extranonce.subscribe; never acts on mining.set_extranonce."""
        nonlocal conns
        try:
            s = socket.create_connection((host, port), timeout=5)
        except Exception:
            return 0, 0, 0
        conns += 1

        def send(o):
            try:
                s.sendall((json.dumps(o) + "\n").encode())
            except Exception:
                pass

        # subscribe + capture the enonce1 the mux hands us. We keep THIS value
        # for every submit on this connection, ignoring any set_extranonce.
        send({"id": 1, "method": "mining.subscribe", "params": ["dualpool-naive/1.0"]})
        s.settimeout(5)
        try:
            first = s.recv(65536)
        except Exception:
            return 0, 0, 0
        if not first:
            return 0, 0, 0
        my_en = ""
        buf = first
        try:
            head, _, rest = buf.partition(b"\n")
            msg = json.loads(head.decode().strip())
            res = msg.get("result")
            if not (isinstance(res, list) and msg.get("id") == 1):
                return 0, 0, 0
            if len(res) >= 2:
                my_en = res[1]
            buf = rest
        except Exception:
            return 0, 0, 0

        # NOTE: deliberately NO mining.extranonce.subscribe here.
        send({"id": 2, "method": "mining.authorize", "params": [worker, "x"]})

        cur_job = ""
        sid = 100
        se_seen = 0
        n_sub = 0
        n_ack = 0
        pending = set()
        next_submit = time.time() + 0.4
        end = time.time() + time_left

        def handle_line(l):
            nonlocal cur_job, se_seen, n_ack
            try:
                m = json.loads(l.decode().strip())
            except Exception:
                return
            meth = m.get("method")
            p = m.get("params") or []
            if meth == "mining.notify" and p:
                cur_job = p[0]
            elif meth == "mining.set_extranonce":
                # A real naive miner ignores this; we only COUNT it so the test
                # can prove the mux never sent one. my_en is left unchanged.
                se_seen += 1
                ne = p[0] if p else ""
                print(f"set_extranonce {ne}", flush=True)
            elif meth is None and ("result" in m):
                rid = m.get("id")
                if rid in pending:
                    pending.discard(rid)
                    n_ack += 1

        s.setblocking(False)
        while time.time() < end:
            r, _, _ = select.select([s], [], [], 0.1)
            if r:
                try:
                    data = s.recv(65536)
                except Exception:
                    data = b""
                if not data:
                    break            # mux dropped us (fallback reconnect-slice)
                buf += data
                while b"\n" in buf:
                    l, buf = buf.split(b"\n", 1)
                    handle_line(l)
            now = time.time()
            if now >= next_submit and cur_job:
                send({"id": sid, "method": "mining.submit",
                      "params": [worker, cur_job, "00000000", my_en, "00000000"]})
                pending.add(sid)
                n_sub += 1
                sid += 1
                next_submit = now + 0.4

        try:
            s.close()
        except Exception:
            pass
        return se_seen, n_sub, n_ack

    # Reconnect loop: keep coming back until the total run time is spent.
    while time.time() < deadline:
        se, ns, na = run_one_session(deadline - time.time())
        set_extranonce_seen += se
        submits += ns
        acks += na
        # brief pause before reconnecting so the mux's next accept is ready
        if time.time() < deadline:
            time.sleep(0.2)

    print(f"set_extranonce_seen={set_extranonce_seen} conns={conns} "
          f"submits={submits} acks={acks}", flush=True)
    return 0

if __name__ == "__main__":
    sys.exit(main())
