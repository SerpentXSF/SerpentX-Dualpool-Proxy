#!/usr/bin/env bash
# HSPLIT-M4 integration test: the dual-upstream swap (the actual hashrate split).
#
# It first runs the M3 gate (run_hashrate_split.sh) as a regression check of the
# single-upstream passthrough path, then drives splitmux_run() via the probe
# through TWO fake upstreams (A/B) and one split-mode fake miner for ~30 s.
#
# Asserts:
#   (a) both pools received shares  (A share >= 2 AND B share >= 2)
#   (b) routing is clean            (every "A share job=" starts A-, "B" starts B-)
#   (c) >= 1 set_extranonce reached the miner (a swap actually happened)
#   (d) the probe exited cleanly (rc 0) after the miner left (crash/hang gate)
#
# Run inside the dualpool-dev image (has python3+gcc+make+jansson):
#   docker run --rm -v "$PWD":/repo -w /repo dualpool-dev \
#       bash test/integration/run_hashrate_split_m4.sh
set -u
cd "$(dirname "$0")"
ROOT=../..
PROBE=/tmp/splitmux_probe
LOG=/tmp/hsplit_m4.log
MINERLOG=/tmp/hsplit_m4_miner.log
LPORT=3334
APORT=4001
BPORT=4002
RUN_S=30

# ---------------------------------------------------------------------------
# 0. M3 regression gate first.
# ---------------------------------------------------------------------------
echo "== M3 regression gate: run_hashrate_split.sh =="
if ! bash run_hashrate_split.sh; then
  echo "HSPLIT-M4 FAILED: M3 regression gate did not pass"; exit 1
fi
echo

# ---------------------------------------------------------------------------
# 1. Compile the probe (0-warning gate).
# ---------------------------------------------------------------------------
echo "== compiling splitmux_probe (M4) =="
PROBE_OUT=$(gcc -std=c11 -Wall -Wextra -I"$ROOT/src" \
  splitmux_probe.c "$ROOT/src/splitmux.c" "$ROOT/src/stratum_msg.c" \
  "$ROOT/src/split_sched.c" -ljansson -lpthread -lm -o "$PROBE" 2>&1)
PROBE_RC=$?
echo "$PROBE_OUT"
if [ "$PROBE_RC" -ne 0 ]; then
  echo "HSPLIT-M4 FAILED: probe compile error"; exit 1
fi
if echo "$PROBE_OUT" | grep -qi "warning:"; then
  echo "HSPLIT-M4 FAILED: probe compile emitted warnings"; exit 1
fi

# ---------------------------------------------------------------------------
# 2. Two fake upstreams (A=4001, B=4002) + probe + split miner.
# ---------------------------------------------------------------------------
: > "$LOG"; : > "$MINERLOG"
echo "== starting fake upstreams A(4001) + B(4002) + probe =="
python3 fake_upstream.py --interval 1.2 --port "$APORT" --tag A --log "$LOG" & UPA=$!
python3 fake_upstream.py --interval 1.2 --port "$BPORT" --tag B --log "$LOG" & UPB=$!
sleep 0.6
"$PROBE" --listen "$LPORT" --upstream "127.0.0.1:$APORT" \
  --upstream2 "127.0.0.1:$BPORT" \
  --ratio 50 --target 3 --min 2 --max 5 & PB=$!
sleep 0.4

echo "== running split miner (${RUN_S}s) =="
python3 fake_miner_split.py 127.0.0.1 "$LPORT" "$RUN_S" wallet.w1 > "$MINERLOG" 2>&1
MINER_RC=$?

# The miner disconnects at end-of-run; splitmux_run must return and the probe
# must exit 0 on its own (reuse the M3 crash/hang gate).
sleep 0.6
if kill -0 "$PB" 2>/dev/null; then
  echo "NOTE: probe still running after miner left (splitmux_run did not return)"
  kill "$PB" 2>/dev/null; wait "$PB" 2>/dev/null; PROBE_OK=0
else
  wait "$PB"; PRC=$?
  if [ "$PRC" -eq 0 ]; then PROBE_OK=1
  else echo "NOTE: probe exited abnormally (rc=$PRC)"; PROBE_OK=0; fi
fi

kill "$UPA" "$UPB" 2>/dev/null
wait 2>/dev/null

# ---------------------------------------------------------------------------
# 3. Assertions.
# ---------------------------------------------------------------------------
A_SHARES=$(grep -c "^A share" "$LOG")
B_SHARES=$(grep -c "^B share" "$LOG")
# routing: an "A share" whose job-id does NOT start with A- is cross-routed.
A_BAD=$(grep "^A share" "$LOG" | grep -vc "job=A-")
B_BAD=$(grep "^B share" "$LOG" | grep -vc "job=B-")
SWAPS=$(grep -c "set_extranonce" "$MINERLOG")
# FIX-1: the miner reports how many of its submits it saw acked/accepted.
ACK_LINE=$(grep -o "acks=[0-9]* accepted=[0-9]* submits=[0-9]*" "$MINERLOG" | tail -1)
ACKS=$(echo "$ACK_LINE" | sed -n 's/.*acks=\([0-9]*\).*/\1/p'); ACKS=${ACKS:-0}
ACCEPTED=$(echo "$ACK_LINE" | sed -n 's/.*accepted=\([0-9]*\).*/\1/p'); ACCEPTED=${ACCEPTED:-0}
SUBMITS=$(echo "$ACK_LINE" | sed -n 's/.*submits=\([0-9]*\).*/\1/p'); SUBMITS=${SUBMITS:-0}

echo "-------------------------------------------"
echo "fake_miner exit code   : $MINER_RC"
echo "A share lines          : $A_SHARES"
echo "B share lines          : $B_SHARES"
echo "A cross-routed shares  : $A_BAD"
echo "B cross-routed shares  : $B_BAD"
echo "set_extranonce (miner) : $SWAPS"
echo "miner submits          : $SUBMITS"
echo "miner acks             : $ACKS"
echo "miner accepted         : $ACCEPTED"
echo "-------------------------------------------"

fail=0
[ "$MINER_RC" -eq 0 ] || { echo "FAIL: fake_miner exit $MINER_RC != 0"; fail=1; }
[ "$A_SHARES" -ge 2 ]  || { echo "FAIL: pool A got < 2 shares"; fail=1; }
[ "$B_SHARES" -ge 2 ]  || { echo "FAIL: pool B got < 2 shares"; fail=1; }
[ "$A_BAD" -eq 0 ]     || { echo "FAIL: $A_BAD A-shares cross-routed (job-id not A-)"; fail=1; }
[ "$B_BAD" -eq 0 ]     || { echo "FAIL: $B_BAD B-shares cross-routed (job-id not B-)"; fail=1; }
[ "$SWAPS" -ge 1 ]     || { echo "FAIL: miner never saw a set_extranonce (no swap reached it)"; fail=1; }
[ "$PROBE_OK" -eq 1 ]  || { echo "FAIL: probe did not exit cleanly (crash or hang)"; fail=1; }
# FIX-1: a real ASIC would disconnect a pool that never acks. The mux MUST relay
# the pools' submit acks back to the miner: zero acks is a hard failure, and all
# but a small tail (in flight at teardown) should be acked.
[ "$SUBMITS" -ge 2 ]   || { echo "FAIL: miner sent < 2 submits (test degenerate)"; fail=1; }
[ "$ACKS" -gt 0 ]      || { echo "FAIL: miner received ZERO submit acks (FIX-1 regressed)"; fail=1; }
[ "$ACKS" -ge $((SUBMITS - 3)) ] || { echo "FAIL: only $ACKS/$SUBMITS submits acked (< submits-3)"; fail=1; }

if [ "$fail" -eq 0 ]; then
  echo "HSPLIT-M4 PASS: both pools mined, routing clean, swap + acks reached the miner."
  exit 0
else
  echo "HSPLIT-M4 FAILED"; exit 1
fi
