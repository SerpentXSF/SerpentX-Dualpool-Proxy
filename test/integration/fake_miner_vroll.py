#!/usr/bin/env python3
"""Version-rolling (ASICBoost) fake miner for Dual-Pool Proxy tests. GPLv3.

Models a real ASIC that negotiates BIP310 version rolling before mining:

  1. sends mining.configure requesting a version-rolling mask (default 1fffe000);
  2. ADOPTS the mask the server grants in the configure reply, and thereafter
     rolls the block nVersion ONLY within that mask (so every share stays valid);
  3. honours a later mining.set_version_mask by narrowing to the new mask (this is
     how the mux keeps a share valid on BOTH pools when they grant different
     masks — it sends the AND-intersection);
  4. if the server answers mining.configure with {} / no mask (i.e. version
     rolling was NOT granted — the exact bug the mux had), it does NOT give up:
     like real ASIC firmware it rolls anyway with a WRONG hardware-default mask
     (0x1fffffff), which sets version bits outside whatever the pool actually
     granted upstream — reproducing the ~97% "Invalid version" reject.

Each submit carries the rolled nVersion as its 6th param (the version-mask param),
exactly as a real ASICBoost miner does, so a version-rolling-aware upstream
(fake_upstream --vmask) can validate it.

On exit prints a summary line so a runner can assert accept/reject counts:

    acks=<n> accepted=<n> rejected=<n> submits=<n>

and prints "set_version_mask <hex>" whenever it adopts a narrowed mask, and
"set_extranonce <hex>" on each swap (matching fake_miner_split.py).

    fake_miner_vroll.py <host> <port> <seconds> [worker] [req_mask_hex]
"""
import json, select, socket, sys, time

BASE_VERSION = 0x20000000     # nVersion the notify carries; we roll within a mask
DEFAULT_WRONG_MASK = 0x1fffffff  # used when NOT granted a mask (broader than a
                                 # pool's standard 1fffe000 grant -> out-of-range bits)


def main():
    host, port, secs = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
    worker = sys.argv[4] if len(sys.argv) > 4 else "wallet.w1"
    req_mask = sys.argv[5] if len(sys.argv) > 5 else "1fffe000"
    try:
        s = socket.create_connection((host, port), timeout=5)
    except Exception:
        return 1

    def send(o):
        try:
            s.sendall((json.dumps(o) + "\n").encode())
        except Exception:
            pass

    buf = b""

    def recv_line(timeout):
        """Block up to `timeout` for one complete JSON line; return dict or None."""
        nonlocal buf
        end = time.time() + timeout
        while True:
            if b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.decode(errors="ignore").strip()
                if not line:
                    continue
                try:
                    return json.loads(line)
                except Exception:
                    continue
            remaining = end - time.time()
            if remaining <= 0:
                return None
            r, _, _ = select.select([s], [], [], remaining)
            if not r:
                return None
            try:
                data = s.recv(65536)
            except Exception:
                return None
            if not data:
                return None
            buf += data

    # ---- 1. mining.configure: request version rolling and adopt the grant ----
    adopted_mask = None          # None => not granted => roll with WRONG default
    send({"id": 0, "method": "mining.configure",
          "params": [["version-rolling"],
                     {"version-rolling.mask": req_mask,
                      "version-rolling.min-bit-count": 2}]})
    s.settimeout(10)
    while True:
        msg = recv_line(10)
        if msg is None:
            return 2
        if msg.get("id") == 0 and "result" in msg:
            res = msg.get("result")
            if isinstance(res, dict) and res.get("version-rolling") is True:
                mv = res.get("version-rolling.mask")
                if isinstance(mv, str):
                    try:
                        adopted_mask = int(mv, 16)
                    except ValueError:
                        adopted_mask = None
            # else: {} / not granted -> stays None (wrong-default rolling)
            break

    # ---- 2. subscribe ----
    send({"id": 1, "method": "mining.subscribe", "params": ["dualpool-vroll/1.0"]})
    cur_en, cur_n2 = "", 0
    while True:
        msg = recv_line(10)
        if msg is None:
            return 2
        if msg.get("id") == 1 and isinstance(msg.get("result"), list):
            res = msg["result"]
            if len(res) >= 3:
                cur_en, cur_n2 = res[1], res[2]
            break

    # ---- 3. advertise set_extranonce support (smooth swaps) + authorize ----
    send({"id": 3, "method": "mining.extranonce.subscribe", "params": []})
    send({"id": 2, "method": "mining.authorize", "params": [worker, "x"]})

    cur_diff, cur_job = 1, ""
    sid = 100
    roll_ctr = 1
    end = time.time() + secs
    next_submit = time.time() + 0.4

    pending = set()
    acks = 0
    accepted = 0
    rejected = 0
    submits = 0

    def cur_mask():
        return adopted_mask if adopted_mask is not None else DEFAULT_WRONG_MASK

    def rolled_version():
        nonlocal roll_ctr
        mk = cur_mask()
        if mk:
            low = mk & (-mk)                 # lowest set bit of the mask
            roll = (roll_ctr * low) & mk     # vary strictly within the mask
        else:
            roll = 0                         # mask 0 => told to stop rolling
        roll_ctr += 1
        return ((BASE_VERSION & ~mk) | roll) & 0xffffffff

    def handle_line(m):
        nonlocal cur_diff, cur_en, cur_n2, cur_job, acks, accepted, rejected
        nonlocal adopted_mask
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
        elif meth == "mining.set_version_mask" and p:
            # The mux narrows us so a rolled share is valid on BOTH pools.
            try:
                adopted_mask = int(p[0], 16)
                print(f"set_version_mask {p[0]}", flush=True)
            except (ValueError, TypeError):
                pass
        elif meth == "mining.notify" and p:
            cur_job = p[0]
        elif meth is None and ("result" in m):
            rid = m.get("id")
            if rid in pending:
                pending.discard(rid)
                acks += 1
                if m.get("result") is True:
                    accepted += 1
                else:
                    rejected += 1

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
                l, buf2 = buf.split(b"\n", 1)
                buf = buf2
                try:
                    handle_line(json.loads(l.decode().strip()))
                except Exception:
                    pass
        now = time.time()
        if now >= next_submit and cur_job:
            send({"id": sid, "method": "mining.submit",
                  "params": [worker, cur_job, "00000000", cur_en, "00000000",
                             f"{rolled_version():08x}"]})
            pending.add(sid)
            submits += 1
            sid += 1
            next_submit = now + 0.4

    # Drain trailing acks so accepted/rejected reflect the whole run.
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
            l, buf2 = buf.split(b"\n", 1)
            buf = buf2
            try:
                handle_line(json.loads(l.decode().strip()))
            except Exception:
                pass

    print(f"acks={acks} accepted={accepted} rejected={rejected} submits={submits}",
          flush=True)
    try:
        s.close()
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
