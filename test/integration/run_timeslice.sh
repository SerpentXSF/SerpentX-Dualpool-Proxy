#!/usr/bin/env bash
# M6 time-slice test: splitter in time_slice mode with a short interval. A single
# miner that reconnects each time it's recycled at a slice boundary should land
# on alternating pools so the long-run split tracks --ratio. (Boundary churn is
# the documented cost; interval is minute-scale in production.) Run in
# dualpool-dev. GPLv3.
set -u
cd "$(dirname "$0")/../.."
BIN=/tmp/sx_ts_splitter
PORT=3336
INTERVAL=700
RATIO=50
BOUNDARIES=14

echo "== compiling =="
make dualpool-splitter >/dev/null 2>&1 || { echo "FAIL: compile"; exit 1; }
cp dualpool-splitter "$BIN"

cd test/integration
python3 fake_upstream.py --port 4201 --tag A --log /dev/null >/dev/null 2>&1 & UPA=$!
python3 fake_upstream.py --port 4202 --tag B --log /dev/null >/dev/null 2>&1 & UPB=$!
sleep 0.5
"$BIN" --listen $PORT --mode time_slice --interval $INTERVAL --ratio $RATIO \
       --poolA 127.0.0.1:4201 --poolB 127.0.0.1:4202 >/tmp/sx_ts.log 2>&1 & SP=$!
sleep 0.6

cleanup() { kill $SP $UPA $UPB 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "== one miner reconnecting across ~$BOUNDARIES slice boundaries =="
# each fake_miner holds until evicted at the next boundary, then the loop reconnects
for k in $(seq 1 $BOUNDARIES); do
  python3 fake_miner.py 127.0.0.1 $PORT 1 "tsminer" 5 >/dev/null 2>&1
done
sleep 0.3

RA=$(grep -c "route -> A" /tmp/sx_ts.log)
RB=$(grep -c "route -> B" /tmp/sx_ts.log)
BND=$(grep -c "time-slice boundary" /tmp/sx_ts.log)
TOTAL=$((RA + RB))

echo "-------------------------------------------"
echo "single miner routed over time:  A=$RA  B=$RB  (total=$TOTAL)"
echo "slice boundaries crossed:       $BND"
echo "target ratio A=${RATIO}% -> expect A~=B"
echo "-------------------------------------------"

fail=0
[ "$BND" -ge 8 ]  || { echo "FAIL: too few slice boundaries ($BND)"; fail=1; }
[ "$TOTAL" -ge 10 ] || { echo "FAIL: miner did not recycle across slices ($TOTAL routes)"; fail=1; }
[ "$RA" -ge 3 ]   || { echo "FAIL: pool A got too few slices ($RA)"; fail=1; }
[ "$RB" -ge 3 ]   || { echo "FAIL: pool B got too few slices ($RB)"; fail=1; }
# at 50/50 the two should be within a reasonable band of each other
d=$((RA - RB)); [ $d -lt 0 ] && d=$((-d))
[ "$d" -le 4 ] || { echo "FAIL: split too skewed for ratio 50 (|A-B|=$d)"; fail=1; }

if [ "$fail" -eq 0 ]; then
  echo "TIME-SLICE PASS: single miner alternates pools by ratio across boundaries."
  exit 0
else
  echo "TIME-SLICE FAILED"; grep -E "route|boundary|TIME-SLICE" /tmp/sx_ts.log | tail -20; exit 1
fi
