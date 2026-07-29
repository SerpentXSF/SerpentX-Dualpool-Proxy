#!/usr/bin/env bash
# HSPLIT-M3 integration test: drives splitmux_run() via the standalone probe
# through ONE fake upstream and ONE fake miner, proving the single-upstream
# relay path end-to-end. Also gates the splitter regression build (the new
# stratum_msg/split_sched/splitmux modules must link clean into
# dualpool-splitter with 0 warnings). GPLv3.
#
# Run inside the dualpool-dev image (has python3+gcc+make+jansson):
#   docker run --rm -v "$PWD":/repo -w /repo dualpool-dev \
#       bash test/integration/run_hashrate_split.sh
set -u
cd "$(dirname "$0")"
ROOT=../..
LOG=/tmp/hsplit.log
PROBE=/tmp/splitmux_probe
LPORT=3334
UPORT=4001

: > "$LOG"

# ---------------------------------------------------------------------------
# 1. Regression gate: SPLITTER_SRC now lists the three new modules; the
#    splitter must still build clean with 0 warnings.
# ---------------------------------------------------------------------------
echo "== regression build: make dualpool-splitter (0-warning gate) =="
for m in stratum_msg.c split_sched.c splitmux.c; do
  grep -q "src/$m" "$ROOT/Makefile" || {
    echo "FAIL: src/$m not listed in SPLITTER_SRC (Makefile)"; exit 1; }
done
rm -f "$ROOT/dualpool-splitter"
BUILD_OUT=$(make -C "$ROOT" dualpool-splitter 2>&1)
BUILD_RC=$?
echo "$BUILD_OUT"
if [ "$BUILD_RC" -ne 0 ]; then
  echo "HSPLIT-M3 FAILED: splitter build error"; exit 1
fi
if echo "$BUILD_OUT" | grep -qi "warning:"; then
  echo "HSPLIT-M3 FAILED: splitter build emitted warnings"; exit 1
fi

# ---------------------------------------------------------------------------
# 2. Compile the probe (0-warning gate).
# ---------------------------------------------------------------------------
echo "== compiling splitmux_probe =="
PROBE_OUT=$(gcc -std=c11 -Wall -Wextra -I"$ROOT/src" \
  splitmux_probe.c "$ROOT/src/splitmux.c" "$ROOT/src/stratum_msg.c" \
  "$ROOT/src/split_sched.c" -ljansson -lpthread -lm -o "$PROBE" 2>&1)
PROBE_RC=$?
echo "$PROBE_OUT"
if [ "$PROBE_RC" -ne 0 ]; then
  echo "HSPLIT-M3 FAILED: probe compile error"; exit 1
fi
if echo "$PROBE_OUT" | grep -qi "warning:"; then
  echo "HSPLIT-M3 FAILED: probe compile emitted warnings"; exit 1
fi

# ---------------------------------------------------------------------------
# 3. Fake upstream + probe + fake miner.
# ---------------------------------------------------------------------------
echo "== starting fake upstream (tag A) + probe =="
python3 fake_upstream.py --port "$UPORT" --tag A --log "$LOG" & UP=$!
sleep 0.6
"$PROBE" --listen "$LPORT" --upstream "127.0.0.1:$UPORT" \
  --ratio 100 --target 10 --min 10 --max 120 & PB=$!
sleep 0.4

echo "== running fake miner (3 shares) =="
python3 fake_miner.py 127.0.0.1 "$LPORT" 3 wallet.w1
MINER_RC=$?

# When the miner disconnects, splitmux_run must return and the probe must exit
# 0 on its own. Still-running after a grace period => splitmux_run failed to
# unwind; a non-zero/signal exit (e.g. 141 = SIGPIPE, 139 = SEGV) => crash.
# Either way the relay is broken even if shares were logged, so gate on it.
sleep 0.5
if kill -0 "$PB" 2>/dev/null; then
  echo "NOTE: probe still running after miner left (splitmux_run did not return)"
  kill "$PB" 2>/dev/null; wait "$PB" 2>/dev/null; PROBE_OK=0
else
  wait "$PB"; PRC=$?
  if [ "$PRC" -eq 0 ]; then PROBE_OK=1
  else echo "NOTE: probe exited abnormally (rc=$PRC)"; PROBE_OK=0; fi
fi

kill "$UP" 2>/dev/null
wait 2>/dev/null

SHARES=$(grep -c "^A share" "$LOG")
echo "-------------------------------------------"
echo "fake_miner exit code : $MINER_RC"
echo "A share lines logged : $SHARES"
echo "-------------------------------------------"

fail=0
[ "$MINER_RC" -eq 0 ] || { echo "FAIL: fake_miner exit $MINER_RC != 0"; fail=1; }
[ "$SHARES" -ge 1 ]   || { echo "FAIL: no 'A share' lines through the relay"; fail=1; }
[ "$PROBE_OK" -eq 1 ] || { echo "FAIL: probe did not exit cleanly (crash or hang)"; fail=1; }

if [ "$fail" -eq 0 ]; then
  echo "HSPLIT-M3 PASS: single-upstream relay carried subscribe/authorize/submit."
  exit 0
else
  echo "HSPLIT-M3 FAILED"; exit 1
fi
