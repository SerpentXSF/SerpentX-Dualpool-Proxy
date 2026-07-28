#!/usr/bin/env python3
"""Minimal fake Stratum upstream for Dual-Pool Proxy integration tests.

Speaks just enough Stratum to let a miner subscribe/authorize/submit. Appends
one line per event to stdout (and optionally a file) so runners can count them:
    <TAG> conn      (a connection reached this pool)
    <TAG> share     (a share was submitted to this pool)

SIGUSR1 makes it STOP accepting new connections while keeping existing ones
alive — simulating "pool unreachable to a health probe, but in-flight sessions
still connected", which is how the eviction path is exercised. GPLv3.
"""
import argparse, json, signal, socket, sys, threading, time

LOCK = threading.Lock()

def logline(path, s):
    with LOCK:
        print(s, flush=True)
        if path and path != "/dev/null":
            with open(path, "a") as f:
                f.write(s + "\n"); f.flush()

def handle(conn, tag, logpath):
    logline(logpath, f"{tag} conn")
    try:
        f = conn.makefile("rwb")
        def send(obj):
            f.write((json.dumps(obj) + "\n").encode()); f.flush()
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
                      "result": [[["mining.notify", "ae6812eb"]], "81000000", 4],
                      "error": None})
            elif m == "mining.authorize":
                send({"id": mid, "result": True, "error": None})
                send({"id": None, "method": "mining.set_difficulty", "params": [1024]})
                send({"id": None, "method": "mining.notify",
                      "params": ["job1", "0" * 64, "01", "02", [],
                                 "20000000", "1a2b3c4d", "5e6f7788", True]})
            elif m == "mining.submit":
                logline(logpath, f"{tag} share")
                send({"id": mid, "result": True, "error": None})
            elif mid is not None:
                send({"id": mid, "result": True, "error": None})
    except (ConnectionResetError, BrokenPipeError, OSError):
        pass

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--log", required=True)
    a = ap.parse_args()

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
        threading.Thread(target=handle, args=(conn, a.tag, a.log), daemon=True).start()

    # keep the process (and its live handler threads) alive after we stop listening
    while True:
        time.sleep(3600)

if __name__ == "__main__":
    main()
