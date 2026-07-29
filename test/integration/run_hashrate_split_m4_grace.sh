#!/usr/bin/env bash
# HSPLIT-M4-GRACE: slow-notify regression for FIX-2 (and the immediate-swap
# path of FIX-5).
#
# Real pools can go tens of seconds between notifies. Here both fakes notify
# only every 5 s (--notify-ms 5000) while the mux runs SHORT slices (min 1 /
# max 2 s), so nearly every swap falls BETWEEN the target's notifies:
#   * FIX-5: the mux must swap immediately onto the target's CURRENT (stale-by-
#     wall-clock but still valid) job forced clean, not stall waiting for a
#     fresh clean notify that is up to 5 s away.
#   * FIX-2: a share the miner found just before a swap must still be routed —
#     the stale grace runs from the SWAP moment, not from the pool's last
#     (seconds-old) notify. If grace were anchored to notify time, many valid
#     shares would be silently dropped and accepted << submitted.
#
# Asserts: both pools still receive shares, swaps still reach the miner, and the
# large majority of submits are acked/accepted (nothing silently dropped).
#
#   docker run --rm -v "$PWD":/repo -w /repo dualpool-dev \
#       bash test/integration/run_hashrate_split_m4_grace.sh
set -u
cd "$(dirname "$0")"
ROOT=../..
PROBE=/tmp/splitmux_probe_grace
LOG=/tmp/hsplit_m4_grace.log
MINERLOG=/tmp/hsplit_m4_grace_miner.log
LPORT=3336
APORT=4021
BPORT=4022
RUN_S=26

echo "== compiling splitmux_probe (grace) =="
PROBE_OUT=$(gcc -std=c11 -Wall -Wextra -I"$ROOT/src" \
  splitmux_probe.c "$ROOT/src/splitmux.c" "$ROOT/src/stratum_msg.c" \
  "$ROOT/src/split_sched.c" -ljansson -lpthread -lm -o "$PROBE" 2>&1)
PROBE_RC=$?
echo "$PROBE_OUT"
if [ "$PROBE_RC" -ne 0 ]; then echo "GRACE FAILED: probe compile error"; exit 1; fi
if echo "$PROBE_OUT" | grep -qi "warning:"; then
  echo "GRACE FAILED: probe compile emitted warnings"; exit 1; fi

: > "$LOG"; : > "$MINERLOG"
echo "== starting fake upstreams with SLOW notifies (5000 ms) =="
python3 fake_upstream.py --port "$APORT" --tag A --log "$LOG" --notify-ms 5000 & UPA=$!
python3 fake_upstream.py --port "$BPORT" --tag B --log "$LOG" --notify-ms 5000 & UPB=$!
sleep 0.6
# Short slices (min 1 / max 2) so swaps fall between the 5 s notifies.
"$PROBE" --listen "$LPORT" --upstream "127.0.0.1:$APORT" \
  --upstream2 "127.0.0.1:$BPORT" \
  --ratio 50 --target 2 --min 1 --max 2 & PB=$!
sleep 0.4

echo "== running split miner (${RUN_S}s) =="
python3 fake_miner_split.py 127.0.0.1 "$LPORT" "$RUN_S" wallet.w1 > "$MINERLOG" 2>&1
MINER_RC=$?

sleep 0.6
if kill -0 "$PB" 2>/dev/null; then
  echo "NOTE: probe still running after miner left"
  kill "$PB" 2>/dev/null; wait "$PB" 2>/dev/null; PROBE_OK=0
else
  wait "$PB"; PRC=$?
  if [ "$PRC" -eq 0 ]; then PROBE_OK=1
  else echo "NOTE: probe exited abnormally (rc=$PRC)"; PROBE_OK=0; fi
fi
kill "$UPA" "$UPB" 2>/dev/null
wait 2>/dev/null

A_SHARES=$(grep -c "^A share" "$LOG")
B_SHARES=$(grep -c "^B share" "$LOG")
A_BAD=$(grep "^A share" "$LOG" | grep -vc "job=A-")
B_BAD=$(grep "^B share" "$LOG" | grep -vc "job=B-")
SWAPS=$(grep -c "set_extranonce" "$MINERLOG")
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
echo "submits/acks/accepted  : $SUBMITS / $ACKS / $ACCEPTED"
echo "-------------------------------------------"

fail=0
[ "$MINER_RC" -eq 0 ] || { echo "FAIL: fake_miner exit $MINER_RC != 0"; fail=1; }
[ "$A_SHARES" -ge 2 ]  || { echo "FAIL: pool A got < 2 shares (grace dropped them?)"; fail=1; }
[ "$B_SHARES" -ge 2 ]  || { echo "FAIL: pool B got < 2 shares (grace dropped them?)"; fail=1; }
[ "$A_BAD" -eq 0 ]     || { echo "FAIL: $A_BAD A-shares cross-routed"; fail=1; }
[ "$B_BAD" -eq 0 ]     || { echo "FAIL: $B_BAD B-shares cross-routed"; fail=1; }
[ "$SWAPS" -ge 1 ]     || { echo "FAIL: no swap reached the miner despite short slices (FIX-5?)"; fail=1; }
[ "$ACKS" -gt 0 ]      || { echo "FAIL: miner received ZERO submit acks"; fail=1; }
# FIX-2: with grace anchored to swap time, virtually every submit still routes
# and is accepted. Allow a small tail for in-flight submits at swaps/teardown.
[ "$ACCEPTED" -ge $((SUBMITS - 5)) ] || {
  echo "FAIL: only $ACCEPTED/$SUBMITS accepted (> 5 dropped -> grace anchored wrong)"; fail=1; }

if [ "$fail" -eq 0 ]; then
  echo "HSPLIT-M4-GRACE PASS: slow notifies, swaps immediate, shares not dropped."
  exit 0
else
  echo "HSPLIT-M4-GRACE FAILED"; exit 1
fi
