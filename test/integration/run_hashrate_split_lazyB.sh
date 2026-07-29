#!/usr/bin/env bash
# HSPLIT-LAZYB: the live-failure regression — a secondary pool that isn't ready
# the instant the mux subscribes must NOT be abandoned. The miner must mine the
# READY primary immediately, and the secondary must JOIN once it becomes ready.
#
# Reproduces the field failure: real ckproxy in userproxy mode logged
# "Temporarily insufficient proxies to accept more clients" and the mux (which
# handshook BOTH pools synchronously) degraded to single-pool on A and NEVER
# retried B. B received ZERO shares forever.
#
# Setup: upstream A normal; upstream B with --ready-delay 8 (rejects subscribe
# for its first 8 s, then behaves normally). Probe drives splitmux (dual, ratio
# 50, short slices) + a swap-following miner for ~26 s.
#
# Asserts (this FAILS before the async-secondary fix, PASSES after):
#   (a) A is mined from the START — A shares within the first ~6 s, BEFORE B is
#       ready (proves the miner was NOT blocked waiting on B).
#   (b) B is NOT yet mined in that early window (B early shares == 0) — proves
#       the lazy/not-ready simulation is real, not an instant handshake.
#   (c) after B becomes ready (~9 s), B ALSO receives shares (B total >= 2) —
#       proves the async bring-up + swap engaged. THIS is what fails pre-fix.
#   (d) A total >= 2 and the probe exits cleanly.
#
# Run inside the dualpool-dev image (python3+gcc+make+jansson):
#   docker run --rm -v "$PWD":/repo -w /repo dualpool-dev \
#       bash test/integration/run_hashrate_split_lazyB.sh
set -u
cd "$(dirname "$0")"
ROOT=../..
PROBE=/tmp/splitmux_probe_lazyb
LOG=/tmp/hsplit_lazyb.log
MINERLOG=/tmp/hsplit_lazyb_miner.log
LPORT=3339
APORT=4041
BPORT=4042
RUN_S=26
READY_DELAY=8
EARLY_S=6            # snapshot point: after A is mining, before B is ready

# ---------------------------------------------------------------------------
# 1. Compile the probe (0-warning gate).
# ---------------------------------------------------------------------------
echo "== compiling splitmux_probe (lazyB) =="
PROBE_OUT=$(gcc -std=c11 -Wall -Wextra -I"$ROOT/src" \
  splitmux_probe.c "$ROOT/src/splitmux.c" "$ROOT/src/stratum_msg.c" \
  "$ROOT/src/split_sched.c" -ljansson -lpthread -lm -o "$PROBE" 2>&1)
PROBE_RC=$?
echo "$PROBE_OUT"
if [ "$PROBE_RC" -ne 0 ]; then echo "HSPLIT-LAZYB FAILED: probe compile error"; exit 1; fi
if echo "$PROBE_OUT" | grep -qi "warning:"; then
  echo "HSPLIT-LAZYB FAILED: probe compile emitted warnings"; exit 1; fi

# ---------------------------------------------------------------------------
# 2. Upstream A (ready) + B (--ready-delay 8) + probe + swap-following miner.
# ---------------------------------------------------------------------------
: > "$LOG"; : > "$MINERLOG"
echo "== starting upstream A($APORT, ready) + B($BPORT, ready-delay ${READY_DELAY}s) + probe =="
python3 fake_upstream.py --interval 1.2 --port "$APORT" --tag A --log "$LOG" & UPA=$!
python3 fake_upstream.py --interval 1.2 --ready-delay "$READY_DELAY" \
  --port "$BPORT" --tag B --log "$LOG" & UPB=$!
sleep 0.6
"$PROBE" --listen "$LPORT" --upstream "127.0.0.1:$APORT" \
  --upstream2 "127.0.0.1:$BPORT" \
  --ratio 50 --target 3 --min 2 --max 5 & PB=$!
sleep 0.4

echo "== running split miner (${RUN_S}s) =="
python3 fake_miner_split.py 127.0.0.1 "$LPORT" "$RUN_S" wallet.lazyb > "$MINERLOG" 2>&1 & MINER=$!

# Early snapshot: A should already be mining; B should not be ready yet.
sleep "$EARLY_S"
A_EARLY=$(grep -c "^A share" "$LOG")
B_EARLY=$(grep -c "^B share" "$LOG")

wait "$MINER"; MINER_RC=$?

# The miner disconnects at end-of-run; splitmux_run must return and the probe
# must exit 0 on its own.
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
A_TOTAL=$(grep -c "^A share" "$LOG")
B_TOTAL=$(grep -c "^B share" "$LOG")

echo "-------------------------------------------"
echo "miner exit code           : $MINER_RC"
echo "A shares @ ${EARLY_S}s (early): $A_EARLY"
echo "B shares @ ${EARLY_S}s (early): $B_EARLY"
echo "A shares total            : $A_TOTAL"
echo "B shares total            : $B_TOTAL"
echo "-------------------------------------------"

fail=0
[ "$MINER_RC" -eq 0 ]  || { echo "FAIL: miner exit $MINER_RC != 0"; fail=1; }
[ "$A_EARLY" -ge 1 ]   || { echo "FAIL: pool A got no early shares (miner blocked on the not-ready secondary)"; fail=1; }
[ "$B_EARLY" -eq 0 ]   || { echo "FAIL: pool B mined before its ready-delay elapsed (harness not simulating not-ready)"; fail=1; }
[ "$A_TOTAL" -ge 2 ]   || { echo "FAIL: pool A got < 2 shares total"; fail=1; }
[ "$B_TOTAL" -ge 2 ]   || { echo "FAIL: pool B got < 2 shares (secondary never joined — the live failure)"; fail=1; }
[ "$PROBE_OK" -eq 1 ]  || { echo "FAIL: probe did not exit cleanly (crash or hang)"; fail=1; }

if [ "$fail" -eq 0 ]; then
  echo "HSPLIT-LAZYB PASS: A mined immediately; B joined after its ready-delay."
  exit 0
else
  echo "HSPLIT-LAZYB FAILED"; exit 1
fi
