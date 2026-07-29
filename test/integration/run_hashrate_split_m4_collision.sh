#!/usr/bin/env bash
# HSPLIT-M4-COLLISION: shared job-id namespace regression for FIX-3.
#
# Both fake upstreams are told (--jobns shared) to number their jobs in the SAME
# namespace ("job-0", "job-1", ...), so a job-id string alone no longer reveals
# which pool issued it — exactly the ckpool-style overlap that made the old
# "newest ring entry by job-id" logic cross-route.
#
# The mux must route each submit to the pool whose work the miner was ACTUALLY
# shown. We detect a cross-route independently of the job-id: every submit
# carries the extranonce1 the miner mined with (params[3]); pool A only ever
# hands out "aaaa0001" and pool B only "bbbb0001", so a share reaching pool A
# with en=bbbb0001 (or B with aaaa0001) is a cross-route.
#
# Asserts:
#   (a) both pools received shares
#   (b) both pools actually issued colliding "job-" ids (namespace really shared)
#   (c) ZERO cross-routed shares (en never mismatches the receiving pool)
#   (d) the miner saw its submit acks relayed back (FIX-1 sanity)
#   (e) the probe exited cleanly
#
#   docker run --rm -v "$PWD":/repo -w /repo dualpool-dev \
#       bash test/integration/run_hashrate_split_m4_collision.sh
set -u
cd "$(dirname "$0")"
ROOT=../..
PROBE=/tmp/splitmux_probe_coll
LOG=/tmp/hsplit_m4_coll.log
MINERLOG=/tmp/hsplit_m4_coll_miner.log
LPORT=3335
APORT=4011
BPORT=4012
RUN_S=25

echo "== compiling splitmux_probe (collision) =="
PROBE_OUT=$(gcc -std=c11 -Wall -Wextra -I"$ROOT/src" \
  splitmux_probe.c "$ROOT/src/splitmux.c" "$ROOT/src/stratum_msg.c" \
  "$ROOT/src/split_sched.c" -ljansson -lpthread -lm -o "$PROBE" 2>&1)
PROBE_RC=$?
echo "$PROBE_OUT"
if [ "$PROBE_RC" -ne 0 ]; then echo "COLLISION FAILED: probe compile error"; exit 1; fi
if echo "$PROBE_OUT" | grep -qi "warning:"; then
  echo "COLLISION FAILED: probe compile emitted warnings"; exit 1; fi

: > "$LOG"; : > "$MINERLOG"
echo "== starting fake upstreams A($APORT) + B($BPORT) with SHARED job namespace =="
python3 fake_upstream.py --port "$APORT" --tag A --log "$LOG" --jobns shared & UPA=$!
python3 fake_upstream.py --port "$BPORT" --tag B --log "$LOG" --jobns shared & UPB=$!
sleep 0.6
"$PROBE" --listen "$LPORT" --upstream "127.0.0.1:$APORT" \
  --upstream2 "127.0.0.1:$BPORT" \
  --ratio 50 --target 3 --min 2 --max 5 & PB=$!
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
# Both pools must have issued colliding "job-" ids for the test to mean anything.
A_JOBNS=$(grep -c "^A notify job=job-" "$LOG")
B_JOBNS=$(grep -c "^B notify job=job-" "$LOG")
# Cross-route = a share reaching a pool with the OTHER pool's extranonce1.
A_CROSS=$(grep "^A share" "$LOG" | grep -c "en=bbbb0001")
B_CROSS=$(grep "^B share" "$LOG" | grep -c "en=aaaa0001")
ACK_LINE=$(grep -o "acks=[0-9]* accepted=[0-9]* submits=[0-9]*" "$MINERLOG" | tail -1)
ACKS=$(echo "$ACK_LINE" | sed -n 's/.*acks=\([0-9]*\).*/\1/p'); ACKS=${ACKS:-0}
SUBMITS=$(echo "$ACK_LINE" | sed -n 's/.*submits=\([0-9]*\).*/\1/p'); SUBMITS=${SUBMITS:-0}

echo "-------------------------------------------"
echo "fake_miner exit code   : $MINER_RC"
echo "A share lines          : $A_SHARES"
echo "B share lines          : $B_SHARES"
echo "A 'job-' notifies      : $A_JOBNS"
echo "B 'job-' notifies      : $B_JOBNS"
echo "A cross-routed (bbbb)  : $A_CROSS"
echo "B cross-routed (aaaa)  : $B_CROSS"
echo "miner submits / acks   : $SUBMITS / $ACKS"
echo "-------------------------------------------"

fail=0
[ "$MINER_RC" -eq 0 ] || { echo "FAIL: fake_miner exit $MINER_RC != 0"; fail=1; }
[ "$A_SHARES" -ge 2 ]  || { echo "FAIL: pool A got < 2 shares"; fail=1; }
[ "$B_SHARES" -ge 2 ]  || { echo "FAIL: pool B got < 2 shares"; fail=1; }
[ "$A_JOBNS" -ge 1 ]   || { echo "FAIL: pool A never issued shared 'job-' ids"; fail=1; }
[ "$B_JOBNS" -ge 1 ]   || { echo "FAIL: pool B never issued shared 'job-' ids"; fail=1; }
[ "$A_CROSS" -eq 0 ]   || { echo "FAIL: $A_CROSS shares cross-routed to A (FIX-3 regressed)"; fail=1; }
[ "$B_CROSS" -eq 0 ]   || { echo "FAIL: $B_CROSS shares cross-routed to B (FIX-3 regressed)"; fail=1; }
[ "$ACKS" -gt 0 ]      || { echo "FAIL: miner received ZERO submit acks (FIX-1 regressed)"; fail=1; }
[ "$PROBE_OK" -eq 1 ]  || { echo "FAIL: probe did not exit cleanly (crash or hang)"; fail=1; }

if [ "$fail" -eq 0 ]; then
  echo "HSPLIT-M4-COLLISION PASS: shared job namespace, zero cross-routed shares."
  exit 0
else
  echo "HSPLIT-M4-COLLISION FAILED"; exit 1
fi
