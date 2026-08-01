#!/usr/bin/env python3
"""Well-formed swap miner that models an ASIC's flush-on-clean, for the
swap-duplicate regression (FIX-11). GPLv3.

Unlike fake_miner_split (constant nonce2/nonce, enonce1 in the ntime slot), this
miner submits well-formed, per-submit-UNIQUE shares: nonce2 and nonce are a
counter of the pool's exact enonce2 length, ntime echoes the job's own ntime, and
(optionally) the block version is rolled within the granted mask.

The key behaviour: on a mining.notify with clean_jobs=true a real ASIC FLUSHES its
current work and RE-MINES the job from scratch, so this miner RESETS its nonce2 /
nonce / version-roll counters to their start. Therefore, if the mux force-clean
re-presents a job the miner ALREADY mined on that pool, the miner re-emits the
IDENTICAL (job,nonce2,ntime,nonce[,version]) tuples it sent before — exact
duplicate shares. A low-churn (solo) pool rejects those; a high-churn pool never
sees them because each swap-back lands on a new job. FIX-11 stops the mux from
re-presenting an unchanged job, so no duplicate is ever produced.

Logs one line per event and a final summary:
    acks=<n> accepted=<n> rejected=<n> submits=<n> dup_submits=<n> repeat_jobs=<n>
`dup_submits` = tuples this miner sent more than once (the duplicate-generating
behaviour); `repeat_jobs` = notify job-ids shown more than once.

    fake_miner_swap.py <host> <port> <seconds> [worker] [mask_hex|off]
"""
import json, select, socket, sys, time

BASE_DEFAULT = 0x20000000


def main():
    host, port, secs = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
    worker = sys.argv[4] if len(sys.argv) > 4 else "wallet.swap"
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

    adopted_mask = None
    noroll = req_mask.lower() in ("off", "0", "none", "no")
    if not noroll:
        send({"id": 0, "method": "mining.configure",
              "params": [["version-rolling"],
                         {"version-rolling.mask": req_mask,
                          "version-rolling.min-bit-count": 2}]})
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
                break

    send({"id": 1, "method": "mining.subscribe", "params": ["dualpool-swap/1.0"]})
    cur_en, cur_n2len = "", 4
    while True:
        msg = recv_line(10)
        if msg is None:
            return 2
        if msg.get("id") == 1 and isinstance(msg.get("result"), list):
            res = msg["result"]
            if len(res) >= 3:
                cur_en, cur_n2len = res[1], int(res[2])
            break

    send({"id": 3, "method": "mining.extranonce.subscribe", "params": []})
    send({"id": 2, "method": "mining.authorize", "params": [worker, "x"]})

    cur_job = ""
    cur_ntime = "5e6f7788"
    cur_base = BASE_DEFAULT
    n2ctr = 0
    roll_ctr = 1
    sid = 100
    seen_jobs = set()
    repeat_jobs = 0
    submitted = set()
    dup_submits = 0
    pending = {}
    acks = accepted = rejected = submits = 0
    end = time.time() + secs
    next_submit = time.time() + 0.4

    def cur_mask():
        return adopted_mask if adopted_mask is not None else 0

    def rolled_version():
        nonlocal roll_ctr
        mk = cur_mask()
        if mk:
            low = mk & (-mk)
            roll = (roll_ctr * low) & mk
        else:
            roll = 0
        roll_ctr += 1
        return ((cur_base & ~mk) | roll) & 0xffffffff

    def handle(m):
        nonlocal cur_job, cur_ntime, cur_base, cur_en, cur_n2len, n2ctr, roll_ctr
        nonlocal adopted_mask, acks, accepted, rejected, repeat_jobs
        meth = m.get("method")
        p = m.get("params") or []
        if meth == "mining.set_difficulty":
            pass
        elif meth == "mining.set_extranonce" and p:
            if p[0] != cur_en:
                cur_en = p[0]
                if len(p) > 1:
                    cur_n2len = int(p[1])
                print(f"SWAP  set_extranonce {cur_en}", flush=True)
        elif meth == "mining.set_version_mask" and p:
            try:
                adopted_mask = int(p[0], 16)
                print(f"MASK  set_version_mask {p[0]}", flush=True)
            except (ValueError, TypeError):
                pass
        elif meth == "mining.notify" and p:
            jid = p[0]
            if len(p) > 7:
                cur_ntime = p[7]
            if len(p) > 5:
                try:
                    cur_base = int(p[5], 16)
                except (ValueError, TypeError):
                    pass
            clean = bool(p[8]) if len(p) > 8 else False
            key = (cur_en, jid)          # pool-aware: distinct enonce1 = distinct pool
            repeat = key in seen_jobs
            if repeat:
                repeat_jobs += 1
            seen_jobs.add(key)
            cur_job = jid
            if clean:
                # ASIC flush + restart: re-mine this job from the start.
                n2ctr = 0
                roll_ctr = 1
            print(f"NOTIFY job={jid} clean={'t' if clean else 'f'} "
                  f"repeat={'t' if repeat else 'f'}", flush=True)
        elif meth is None and ("result" in m):
            rid = m.get("id")
            if rid in pending:
                j = pending.pop(rid)
                acks += 1
                if m.get("result") is True:
                    accepted += 1
                    print(f"ACCEPT id={rid} job={j}", flush=True)
                else:
                    rejected += 1
                    print(f"REJECT id={rid} job={j} err={m.get('error')}", flush=True)

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
                try:
                    handle(json.loads(l.decode().strip()))
                except Exception:
                    pass
        now = time.time()
        if now >= next_submit and cur_job:
            n2 = f"{n2ctr:0{cur_n2len*2}x}"
            nonce = f"{n2ctr:08x}"
            if noroll:
                params = [worker, cur_job, n2, cur_ntime, nonce]
                ver = "-"
            else:
                ver = f"{rolled_version():08x}"
                params = [worker, cur_job, n2, cur_ntime, nonce, ver]
            # Dedup key includes the current extranonce1: a share is only a
            # duplicate WITHIN one pool's enonce1 space. Two independent pools may
            # reuse the same job-id integer, so keying on job alone would flag a
            # false duplicate across a pool swap.
            tup = (cur_en, cur_job, n2, cur_ntime, nonce, ver)
            is_dup = tup in submitted
            if is_dup:
                dup_submits += 1
            submitted.add(tup)
            send({"id": sid, "method": "mining.submit", "params": params})
            print(f"SUBMIT job={cur_job} n2={n2} ntime={cur_ntime} "
                  f"dup={'t' if is_dup else 'f'}", flush=True)
            pending[sid] = cur_job
            submits += 1
            sid += 1
            n2ctr += 1
            next_submit = now + 0.4

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
            l, buf = buf.split(b"\n", 1)
            try:
                handle(json.loads(l.decode().strip()))
            except Exception:
                pass

    print(f"acks={acks} accepted={accepted} rejected={rejected} submits={submits} "
          f"dup_submits={dup_submits} repeat_jobs={repeat_jobs}", flush=True)
    try:
        s.close()
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
