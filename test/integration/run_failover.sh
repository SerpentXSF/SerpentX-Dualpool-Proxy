#!/usr/bin/env bash
# Failover integration test (M5): real splitter (CLI mode) + two fake upstreams.
# Verifies pool-DOWN donation (new miners go to the survivor), eviction (a dead
# pool's pinned miners are disconnected), and recovery (traffic returns when the
# pool comes back). Run inside the serpentx-dev image. GPLv3.
set -u
cd "$(dirname "$0")/../.."
ROOT=.
BIN=/tmp/sx_fo_splitter
LOG=/tmp/sx_fo.log
PORT=3334

echo "== compiling splitter =="
make serpentx-splitter >/dev/null 2>&1 || { echo "FAIL: compile"; exit 1; }
cp serpentx-splitter "$BIN"

cd test/integration
python3 fake_upstream.py --port 4001 --tag A --log /dev/null >/dev/null 2>&1 & UPA=$!
python3 fake_upstream.py --port 4002 --tag B --log /dev/null >/dev/null 2>&1 & UPB=$!
sleep 0.5
: > "$LOG"
"$BIN" --listen $PORT --poolA 127.0.0.1:4001 --poolB 127.0.0.1:4002 --ratio 50 >"$LOG" 2>&1 & SP=$!
sleep 1

cleanup() { kill $SP $UPA $UPB 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

run_batch() { for i in $(seq 1 "$1"); do python3 fake_miner.py 127.0.0.1 $PORT 1 "m.$RANDOM" >/dev/null 2>&1; done; }
rA() { grep -c "route -> A" "$LOG"; }
rB() { grep -c "route -> B" "$LOG"; }

echo "== batch 1: both pools alive =="
# long-lived miners so several are still connected (and some pinned to B) when B
# dies, to exercise eviction. They hold the connection until evicted/timeout.
for i in $(seq 1 8); do python3 fake_miner.py 127.0.0.1 $PORT 1 "hold.$i" 40 >/dev/null 2>&1 & done
sleep 2
run_batch 4
A1=$(rA); B1=$(rB)
echo "   routed so far: A=$A1 B=$B1"
[ "$B1" -ge 1 ] || { echo "FAIL: pool B got no miners while alive"; exit 1; }

echo "== pool B stops accepting (held sessions stay; probe will see it down) =="
kill -USR1 $UPB 2>/dev/null      # refuse new connects, keep held B sessions alive
sleep 9                           # probe: ~2 missed rounds -> DOWN + evict held B miners

echo "== batch 2: B is down (expect donation to A) =="
run_batch 12
A2=$(rA); B2=$(rB)
echo "   routed so far: A=$A2 B=$B2"
evicted=$(grep -c "evicted" "$LOG")

echo "== restarting pool B upstream =="
kill $UPB 2>/dev/null
python3 fake_upstream.py --port 4002 --tag B --log /dev/null >/dev/null 2>&1 & UPB=$!
sleep 8   # probe detects recovery

echo "== batch 3: B recovered (expect some B again) =="
run_batch 16
A3=$(rA); B3=$(rB)
echo "   routed so far: A=$A3 B=$B3"
recovered=$(grep -c "recovered" "$LOG")

echo "-------------------------------------------"
echo "batch1 A=$A1 B=$B1 | after-kill A=$A2 B=$B2 | after-recover A=$A3 B=$B3"
echo "evicted log lines=$evicted  recovered log lines=$recovered"
echo "-------------------------------------------"

fail=0
# donation: no NEW B routes while B was down
[ "$B2" -eq "$B1" ] || { echo "FAIL: pool B received miners while down ($B1 -> $B2)"; fail=1; }
# and batch2 all went to A
[ $((A2 - A1)) -ge 11 ] || { echo "FAIL: batch2 not donated to A (A $A1 -> $A2)"; fail=1; }
# eviction happened
[ "$evicted" -ge 1 ] || { echo "FAIL: no eviction logged when B went down"; fail=1; }
# recovery: B gets traffic again in batch3
[ $((B3 - B2)) -ge 1 ] || { echo "FAIL: pool B got no miners after recovery"; fail=1; }
[ "$recovered" -ge 1 ] || { echo "FAIL: recovery not logged"; fail=1; }

if [ "$fail" -eq 0 ]; then
  echo "FAILOVER PASS: donation + eviction + recovery all verified."
  exit 0
else
  echo "FAILOVER FAILED"; echo "--- splitter log tail ---"; grep -E "route|evict|recover|down" "$LOG" | tail -20; exit 1
fi
