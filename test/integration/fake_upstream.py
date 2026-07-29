#!/usr/bin/env python3
"""Minimal fake Stratum upstream for Dual-Pool Proxy integration tests.

Speaks just enough Stratum to let a miner subscribe/authorize/submit. Appends
one line per event to stdout (and optionally a file) so runners can count them:
    <TAG> conn               (a connection reached this pool)
    <TAG> notify job=<jobid> (this pool ISSUED that job to the mux)
    <TAG> share job=<jobid>  (a share was submitted to this pool, with its job)
    <TAG> reject-version job=<jobid> (a share's rolled version left the granted
                             ASICBoost version-rolling mask; see --vmask)

ASICBoost version rolling (--vmask <hex>, e.g. 1fffe000, default OFF): when set,
this pool GRANTS version-rolling on mining.configure — replying
{"version-rolling": true, "version-rolling.mask": "<req & vmask>"} — and then
VALIDATES every submit's 6th param (the rolled nVersion): any bit that differs
from the job's base version (0x20000000) and lies OUTSIDE the granted mask is
rejected with {"result": false, "error": "Invalid version"} and logged
`reject-version`. A share submitted with a rolled version when version-rolling was
never granted is likewise rejected. This models a real pool tightly enough to
reject a miner that rolls a mask the pool did not grant (the exact failure the mux
caused by answering mining.configure with {}). With --vmask unset, configure is
answered {} and submits are accepted regardless of version (legacy behaviour).

Each tag advertises a DISTINCT extranonce1 (A -> "aaaa0001", else "bbbb0001")
so the mux can synthesize per-pool sessions and a runner can verify routing.
After authorize the upstream sends one clean mining.notify. With --interval /
--notify-ms > 0 it ALSO keeps issuing a fresh *clean* notify with a monotonically
increasing job-id, giving the mux clean-job boundaries to swap on. That periodic
stream is OFF by default so idle held miners still time out and disconnect (which
the pre-existing robustness/eviction tests depend on).

Job-id namespace is selectable so a test can force a COLLISION:
    --jobns tag     (default) job-ids are "<tag>-<seq>" (A-0, B-0, ...)
    --jobns shared  both pools use "job-<seq>" (overlapping namespace) — this
                    exercises FIX-3 (the mux must route by the job it actually
                    SHOWED, not by job-id string alone). The runner verifies
                    zero cross-routing by matching each 'share job=' against the
                    receiving pool's own 'notify job=' lines.

The fresh-notify interval is a flag: --interval <seconds> or --notify-ms <ms>
(the latter wins). A slow interval with short mux slices makes swaps fall
BETWEEN notifies, exercising the swap-time stale-share grace (FIX-2).

SIGUSR1 makes it STOP accepting new connections while keeping existing ones
alive — simulating "pool unreachable to a health probe, but in-flight sessions
still connected", which is how the eviction path is exercised. GPLv3.
"""
import argparse, json, signal, socket, sys, threading, time

LOCK = threading.Lock()
START = time.monotonic()   # process start, for --ready-delay not-ready-yet window

def logline(path, s):
    with LOCK:
        print(s, flush=True)
        if path and path != "/dev/null":
            with open(path, "a") as f:
                f.write(s + "\n"); f.flush()

def enonce1_for(tag):
    return "aaaa0001" if tag == "A" else "bbbb0001"

BASE_VERSION = 0x20000000   # nVersion the notify carries (params[5]); miners roll it

def handle(conn, tag, logpath, interval, jobns, workless=False, ready_delay=0.0,
           vmask=None):
    logline(logpath, f"{tag} conn")
    if ready_delay > 0 and (time.monotonic() - START) < ready_delay:
        # "not ready yet": for the first ready_delay seconds of PROCESS life,
        # reject a subscribe with an error and close — simulating a ckproxy in
        # userproxy mode that isn't ready ("Temporarily insufficient proxies" /
        # "Failed to provide subscription due to no sdata"). The mux's secondary
        # bring-up must retry (with backoff) until this pool becomes ready.
        try:
            f = conn.makefile("rwb")
            for raw in f:
                line = raw.decode(errors="ignore").strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except Exception:
                    continue
                if msg.get("method") == "mining.subscribe":
                    err = {"id": msg.get("id"), "result": None,
                           "error": [20, "Temporarily insufficient proxies to "
                                         "accept more clients", None]}
                    try:
                        f.write((json.dumps(err) + "\n").encode()); f.flush()
                    except Exception:
                        pass
                    return                       # close: not ready yet
        except (ConnectionResetError, BrokenPipeError, OSError):
            return
        finally:
            try:
                conn.close()
            except Exception:
                pass
        return
    if workless:
        # "reachable but workless": accept the TCP connection and NEVER answer
        # anything — no subscribe result, no notify. Simulates a ckproxy whose
        # upstream has no work yet. The mux must time its handshake out and
        # degrade to the healthy pool (D1b) instead of stranding the miner.
        try:
            while True:
                if not conn.recv(65536):
                    return                       # peer closed
        except (ConnectionResetError, BrokenPipeError, OSError):
            return
    en1 = enonce1_for(tag)
    slock = threading.Lock()
    seq = [0]
    granted = [None]   # per-connection granted version-rolling mask (int), None = not granted
    stop = threading.Event()
    try:
        f = conn.makefile("rwb")
        def send(obj):
            with slock:
                f.write((json.dumps(obj) + "\n").encode()); f.flush()
        def job_id(n):
            # 'shared' makes both pools use the SAME namespace ("job-<seq>"),
            # forcing job-id collisions so FIX-3 routing is actually tested.
            return f"job-{n}" if jobns == "shared" else f"{tag}-{n}"
        def make_notify(clean=True):
            n = seq[0]; seq[0] += 1
            jid = job_id(n)
            logline(logpath, f"{tag} notify job={jid}")
            return {"id": None, "method": "mining.notify",
                    "params": [jid, "0" * 64, "01", "02", [],
                               "20000000", "1a2b3c4d", "5e6f7788", clean]}
        def notifier():
            # After authorize, keep feeding fresh clean jobs so the mux has
            # clean-job boundaries to swap on. Opt-in (interval > 0): a periodic
            # notify stream keeps a held miner's read from ever timing out, which
            # would wedge tests that rely on idle miners disconnecting on their
            # own (run_robustness et al.), so it defaults OFF.
            while not stop.wait(interval):
                try:
                    send(make_notify(True))
                except Exception:
                    return
        for raw in f:
            line = raw.decode(errors="ignore").strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except Exception:
                continue
            m, mid = msg.get("method"), msg.get("id")
            if m == "mining.subscribe":
                send({"id": mid,
                      "result": [[["mining.notify", "ae6812eb"]], en1, 4],
                      "error": None})
            elif m == "mining.configure":
                # ASICBoost version-rolling negotiation. Grant only what BOTH the
                # miner requested AND this pool's --vmask allows (their AND).
                if vmask is None:
                    send({"id": mid, "result": {}, "error": None})
                else:
                    params = msg.get("params") or []
                    args = params[1] if len(params) > 1 and isinstance(params[1], dict) else {}
                    req = 0xffffffff
                    rm = args.get("version-rolling.mask")
                    if isinstance(rm, str):
                        try:
                            req = int(rm, 16)
                        except ValueError:
                            req = 0xffffffff
                    g = req & vmask
                    granted[0] = g
                    send({"id": mid, "result": {"version-rolling": True,
                          "version-rolling.mask": f"{g:08x}"}, "error": None})
            elif m == "mining.authorize":
                # Real ckproxy (userproxy) keys the upstream user on the authorize
                # username and REJECTS an empty one ("Empty workername parameter").
                # Mirror that so the harness catches a mux that forgets to send the
                # miner's worker (regression that shipped past the old fake).
                params = msg.get("params") or []
                worker = params[0] if params else ""
                if not worker:
                    send({"id": mid, "result": False,
                          "error": [24, "Empty workername parameter", None]})
                    continue
                send({"id": mid, "result": True, "error": None})
                send({"id": None, "method": "mining.set_difficulty",
                      "params": [1024]})
                # A real version-rolling pool PUSHES the negotiated mask to its
                # client via mining.set_version_mask. A ckpool proxy sitting in
                # front of us relies on this to learn the upstream version_mask
                # (else it logs "Json did not find entry version_mask" and rejects
                # every rolled downstream share with "Invalid version mask"). Send
                # it before the first job so the proxy is primed. In the pure mux
                # harness the mux simply ignores this upstream notification.
                if vmask is not None:
                    if granted[0] is None:
                        granted[0] = vmask
                    send({"id": None, "method": "mining.set_version_mask",
                          "params": [f"{vmask:08x}"]})
                send(make_notify(True))              # first clean job
                if interval > 0:                     # opt-in periodic fresh jobs
                    threading.Thread(target=notifier, daemon=True).start()
            elif m == "mining.submit":
                params = msg.get("params") or []
                job = params[1] if len(params) > 1 else "?"
                # params[3] is the extranonce1 the miner mined with. Under a
                # shared job-id namespace the job prefix no longer reveals the
                # origin pool, but this extranonce1 does: it MUST equal this
                # pool's own en1, else the share was cross-routed (FIX-3).
                en = params[3] if len(params) > 3 else "?"
                # ASICBoost version check (only when this pool enforces --vmask):
                # params[5], if present, is the rolled nVersion. Any bit that
                # differs from the base version and falls outside the granted mask
                # (or ANY roll when version-rolling was never granted) is invalid.
                if vmask is not None and len(params) > 5:
                    try:
                        ver = int(params[5], 16)
                    except (ValueError, TypeError):
                        ver = BASE_VERSION
                    diff = (ver ^ BASE_VERSION) & 0xffffffff
                    allowed = granted[0] if granted[0] is not None else 0
                    if diff & ~allowed & 0xffffffff:
                        logline(logpath, f"{tag} reject-version job={job}")
                        send({"id": mid, "result": False,
                              "error": [23, "Invalid version", None]})
                        continue
                logline(logpath, f"{tag} share job={job} en={en}")
                send({"id": mid, "result": True, "error": None})
            elif mid is not None:
                send({"id": mid, "result": True, "error": None})
    except (ConnectionResetError, BrokenPipeError, OSError):
        pass
    finally:
        stop.set()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--interval", type=float, default=0.0,
                    help="seconds between fresh clean notifies (0 = off, default)")
    ap.add_argument("--notify-ms", type=int, default=0,
                    help="ms between fresh clean notifies (overrides --interval)")
    ap.add_argument("--jobns", choices=("tag", "shared"), default="tag",
                    help="job-id namespace: 'tag' (A-/B-) or 'shared' (job-)")
    ap.add_argument("--workless", action="store_true",
                    help="accept TCP but answer NOTHING (never subscribe) — "
                         "simulates a reachable-but-workless upstream")
    ap.add_argument("--ready-delay", type=float, default=0.0, dest="ready_delay",
                    help="for the first N seconds of process life, reject "
                         "mining.subscribe with an error and close (simulates a "
                         "ckproxy that isn't ready yet); afterwards behave normally")
    ap.add_argument("--vmask", default=None,
                    help="hex ASICBoost version-rolling mask to GRANT on "
                         "mining.configure and enforce on submits (e.g. 1fffe000); "
                         "default OFF (configure answered {}, submits unchecked)")
    a = ap.parse_args()
    interval = a.notify_ms / 1000.0 if a.notify_ms > 0 else a.interval
    vmask = int(a.vmask, 16) if a.vmask else None

    ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", a.port))
    ls.listen(128)

    state = {"accepting": True}
    def stop_listening(_sig, _frm):
        state["accepting"] = False
        try:
            ls.close()          # refuse new connects; keep existing sessions
        except Exception:
            pass
    signal.signal(signal.SIGUSR1, stop_listening)

    while state["accepting"]:
        try:
            conn, _ = ls.accept()
        except OSError:
            break               # listener closed via SIGUSR1
        threading.Thread(target=handle,
                         args=(conn, a.tag, a.log, interval, a.jobns, a.workless,
                               a.ready_delay, vmask),
                         daemon=True).start()

    # keep the process (and its live handler threads) alive after we stop listening
    while True:
        time.sleep(3600)

if __name__ == "__main__":
    main()
