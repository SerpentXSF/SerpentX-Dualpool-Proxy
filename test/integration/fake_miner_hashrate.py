#!/usr/bin/env python3
"""Fixed-hashrate fake miner for Dual-Pool Proxy tests. GPLv3.

Unlike the other fake miners (which submit on a fixed wall-clock cadence), this
one models the physics that the proactive-difficulty fix exists for: a miner of a
GIVEN hashrate finds a share only every

    diff * 2^32 / hashrate   seconds

so raising the difficulty makes shares proportionally rarer, and a pool that opens
a session at 1,000,000 can starve a miner into producing (almost) nothing. Search
progress is accumulated as a fraction of the current difficulty's expected work,
so a mid-flight mining.set_difficulty takes effect immediately and smoothly
instead of restarting the clock.

By default it does NOT send mining.suggest_difficulty — it is the firmware that
never tells a pool how big it is, which is the case the mux's proactive
suggestion has to cover. It DOES send mining.extranonce.subscribe, so the mux
takes the smooth-swap path.

--suggest <D> models the OPPOSITE firmware: one that ships a hardcoded
mining.suggest_difficulty and repeats it every --suggest-every seconds
(--suggest-count times, 0 = for the whole run). A hardcoded small-miner default
of 4000 is correct for a 1.5 TH/s miner and a share-per-1.3s flood for a
12.8 TH/s one; the repeats matter because each one re-arms whichever pool is idle
at that moment.

Prints "diff <d>" whenever the difficulty it was told changes, "set_extranonce
<hex>" on each swap, "suggest <d>" on each hint it sends, and on exit a summary
line for the runner to assert on:

    acks=<n> accepted=<n> rejected=<n> submits=<n>

    fake_miner_hashrate.py <host> <port> <seconds> [worker] [hashrate_THs]
                           [--suggest D] [--suggest-every S] [--suggest-count N]
"""
import json, select, socket, sys, time

TWO32 = 4294967296.0


def split_args(args):
    """Split a tail of argv into positionals and `--flag value` options."""
    pos, opts, i = [], {}, 0
    while i < len(args):
        if args[i].startswith("--"):
            opts[args[i]] = args[i + 1] if i + 1 < len(args) else ""
            i += 2
        else:
            pos.append(args[i])
            i += 1
    return pos, opts


def main():
    host, port, secs = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
    # Positionals 4/5 (worker, hashrate) are unchanged for the existing callers;
    # the suggest knobs are flags, so adding them cannot disturb them.
    pos, opts = split_args(sys.argv[4:])
    worker = pos[0] if len(pos) > 0 else "wallet.hr1"
    ths = float(pos[1]) if len(pos) > 1 else 12.9
    hashrate = ths * 1e12                       # H/s
    sugg_diff = float(opts.get("--suggest", 0))
    sugg_every = float(opts.get("--suggest-every", 2.0))
    sugg_count = int(opts.get("--suggest-count", 0))  # 0 = repeat for the whole run

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

    # ---- subscribe ----
    send({"id": 1, "method": "mining.subscribe", "params": ["dualpool-hr/1.0"]})
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

    # ---- advertise set_extranonce support (smooth swaps) + authorize. NO
    # mining.suggest_difficulty: this firmware never tells a pool its size. ----
    send({"id": 3, "method": "mining.extranonce.subscribe", "params": []})
    send({"id": 2, "method": "mining.authorize", "params": [worker, "x"]})

    # Hardcoded-hint firmware: send the suggestion straight after authorize and
    # keep repeating it. Its own id range (10+) keeps it clear of the submit ids.
    sugg_sent = [0]
    sugg_next = [time.time()]

    def maybe_suggest(now):
        if sugg_diff <= 0 or now < sugg_next[0]:
            return
        if sugg_count and sugg_sent[0] >= sugg_count:
            return
        send({"id": 10 + sugg_sent[0], "method": "mining.suggest_difficulty",
              "params": [sugg_diff]})
        sugg_sent[0] += 1
        sugg_next[0] = now + sugg_every
        print(f"suggest {sugg_diff:.0f}", flush=True)

    maybe_suggest(time.time())

    cur_diff, cur_job = 0.0, ""
    sid = 100
    n2 = 0
    pending = set()
    acks = accepted = rejected = submits = 0
    progress = 0.0          # fraction of the CURRENT difficulty's work searched

    def handle_line(m):
        nonlocal cur_diff, cur_en, cur_n2, cur_job, acks, accepted, rejected
        meth = m.get("method")
        p = m.get("params") or []
        if meth == "mining.set_difficulty" and p:
            try:
                d = float(p[0])
            except (TypeError, ValueError):
                return
            if d != cur_diff:
                cur_diff = d
                print(f"diff {d:.0f}", flush=True)
        elif meth == "mining.set_extranonce" and p:
            ne = p[0]
            if ne != cur_en:
                cur_en = ne
                cur_n2 = p[1] if len(p) > 1 else cur_n2
                print(f"set_extranonce {cur_en}", flush=True)
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
    end = time.time() + secs
    last = time.time()
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 0.05)
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
        maybe_suggest(now)
        dt, last = now - last, now
        if cur_job and cur_diff > 0.0 and dt > 0.0:
            # Hashes searched in dt, as a fraction of this difficulty's expected
            # work. One whole unit of progress == one share found.
            progress += (hashrate * dt) / (cur_diff * TWO32)
            while progress >= 1.0:
                progress -= 1.0
                n2 += 1
                send({"id": sid, "method": "mining.submit",
                      "params": [worker, cur_job, f"{n2:08x}", cur_en,
                                 f"{n2:08x}"]})
                pending.add(sid)
                submits += 1
                sid += 1

    # Drain trailing acks so accepted/rejected reflect the whole run.
    drain_end = time.time() + 0.6
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
