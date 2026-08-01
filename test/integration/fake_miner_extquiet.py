#!/usr/bin/env python3
"""Fake "ext-quiet" Stratum miner for the assume_extranonce opt-in. GPLv3.
Copyright (C) 2025-2026 The SerpentX authors.

Models the user's real fleet profile — ESP-Miner-derived firmware (BitAxe /
Hammer / NerdAxe / NerdQAxe) — which sits in the gap the mux's M5 capability
detection cannot see:

  * it NEVER sends mining.extranonce.subscribe (so the mux, by default, must
    classify it as incapable and reconnect-slice it), yet
  * it DOES honour mining.set_extranonce: on receipt it adopts the new
    extranonce1 (and nonce2 size), DISCARDS in-flight work, and resumes mining
    only once the next mining.notify arrives — which is what real firmware does,
    since a new extranonce1 invalidates the current nonce2 space.

Every submit carries the extranonce1 the miner is CURRENTLY using, in params[3],
the field fake_upstream.py logs as `en=`. That makes the pools the oracle: a
share reaching pool A must carry A's own extranonce1, so any en/pool mismatch is
a cross-routed share, i.e. proof the miner did NOT really follow the swap.

Like fake_miner_naive.py it RECONNECTS when the server drops it, so the M5
reconnect-slice fallback is observable as a connection count > 1.

Per-event lines (greppable):
    set_extranonce <enonce1>       each set_extranonce actually honoured
    reconnect <n>                  each reconnect after the first connection

Final summary line:
    extquiet set_extranonce_seen=<n> conns=<n> reconnects=<n> submits=<n>
             acks=<n> accepted=<n> rejected=<n>

RED (flag off)  => set_extranonce_seen == 0 and reconnects >= 1
GREEN (flag on) => set_extranonce_seen >= 1 and reconnects == 0

    fake_miner_extquiet.py <host> <port> <seconds> [worker]
"""
import json
import select
import socket
import sys
import time

SUBMIT_EVERY = 0.4     # seconds between submits while we hold live work
RECONNECT_PAUSE = 0.2  # settle time before coming back after a drop


def main():
    host, port, secs = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
    worker = sys.argv[4] if len(sys.argv) > 4 else "wallet.extquiet"
    deadline = time.time() + secs

    totals = {"se": 0, "conns": 0, "submits": 0,
              "acks": 0, "accepted": 0, "rejected": 0}

    def run_one_session(time_left):
        """One connection: subscribe (WITHOUT extranonce.subscribe), authorize,
        then submit against the current job with the current extranonce1 until
        dropped or out of time. Honours mining.set_extranonce."""
        try:
            s = socket.create_connection((host, port), timeout=5)
        except Exception:
            return
        totals["conns"] += 1
        if totals["conns"] > 1:
            print(f"reconnect {totals['conns'] - 1}", flush=True)

        def send(o):
            try:
                s.sendall((json.dumps(o) + "\n").encode())
            except Exception:
                pass

        # Subscribe and capture the extranonce1/nonce2-size the mux hands us.
        # Generous wait: on the single-pool degrade path the mux only answers
        # after its dead-pool handshake timeout (~5 s) elapses.
        send({"id": 1, "method": "mining.subscribe",
              "params": ["dualpool-extquiet/1.0"]})
        s.settimeout(10)
        try:
            first = s.recv(65536)
        except Exception:
            return
        if not first:
            return
        buf = first
        cur_en, cur_n2 = "", 0
        try:
            head, _, rest = buf.partition(b"\n")
            msg = json.loads(head.decode().strip())
            res = msg.get("result")
            if not (isinstance(res, list) and msg.get("id") == 1):
                return
            if len(res) >= 3:
                cur_en, cur_n2 = res[1], res[2]
            elif len(res) >= 2:
                cur_en = res[1]
            buf = rest
        except Exception:
            return

        # NOTE: deliberately NO mining.extranonce.subscribe — that omission is
        # the whole point of this miner. It still honours set_extranonce below.
        send({"id": 2, "method": "mining.authorize", "params": [worker, "x"]})

        state = {"job": "", "en": cur_en, "n2": cur_n2,
                 "awaiting_job": False, "acks": 0,
                 "accepted": 0, "rejected": 0, "se": 0}
        pending = set()
        sid = 100
        n_sub = 0
        next_submit = time.time() + SUBMIT_EVERY
        end = time.time() + time_left

        def handle_line(line):
            try:
                m = json.loads(line.decode().strip())
            except Exception:
                return
            meth = m.get("method")
            p = m.get("params") or []
            if meth == "mining.set_extranonce" and p:
                # HONOUR it: adopt the new extranonce1 (+ nonce2 size), drop any
                # in-flight work and wait for the next job before submitting
                # again. Real firmware must do this — the old nonce2 space is no
                # longer valid under a new extranonce1.
                state["en"] = p[0]
                if len(p) > 1:
                    state["n2"] = p[1]
                state["se"] += 1
                state["job"] = ""
                state["awaiting_job"] = True
                print(f"set_extranonce {state['en']}", flush=True)
            elif meth == "mining.notify" and p:
                state["job"] = p[0]
                state["awaiting_job"] = False
            elif meth is None and ("result" in m):
                rid = m.get("id")
                if rid in pending:
                    pending.discard(rid)
                    state["acks"] += 1
                    if m.get("result") is True and not m.get("error"):
                        state["accepted"] += 1
                    else:
                        state["rejected"] += 1

        s.setblocking(False)
        while time.time() < end:
            r, _, _ = select.select([s], [], [], 0.1)
            if r:
                try:
                    data = s.recv(65536)
                except Exception:
                    data = b""
                if not data:
                    break          # mux dropped us (reconnect-slice fallback)
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    handle_line(line)
            now = time.time()
            if now >= next_submit and state["job"] and not state["awaiting_job"]:
                send({"id": sid, "method": "mining.submit",
                      "params": [worker, state["job"], "00000000",
                                 state["en"], "00000000"]})
                pending.add(sid)
                n_sub += 1
                sid += 1
                next_submit = now + SUBMIT_EVERY

        # Drain acks still in flight before tearing the socket down.
        drain_end = time.time() + 0.4
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
                line, buf = buf.split(b"\n", 1)
                handle_line(line)

        totals["se"] += state["se"]
        totals["submits"] += n_sub
        totals["acks"] += state["acks"]
        totals["accepted"] += state["accepted"]
        totals["rejected"] += state["rejected"]
        try:
            s.close()
        except Exception:
            pass

    # Reconnect loop: keep coming back until the total run time is spent, so a
    # reconnect-sliced session shows up as conns > 1.
    while time.time() < deadline:
        run_one_session(deadline - time.time())
        if time.time() < deadline:
            time.sleep(RECONNECT_PAUSE)

    reconnects = max(0, totals["conns"] - 1)
    print(f"extquiet set_extranonce_seen={totals['se']} "
          f"conns={totals['conns']} reconnects={reconnects} "
          f"submits={totals['submits']} acks={totals['acks']} "
          f"accepted={totals['accepted']} rejected={totals['rejected']}",
          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
