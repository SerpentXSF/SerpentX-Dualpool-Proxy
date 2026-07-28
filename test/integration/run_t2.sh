#!/usr/bin/env bash
# T2 integration test: real splitter binary + two fake Stratum upstreams + N
# fake miners. Asserts (1) the realized A/B split matches --ratio, and (2) every
# miner's shares land at the pool it was routed to (correct routing). No ckproxy
# needed — the splitter's upstreams point straight at the fakes. GPLv3.
set -u
cd "$(dirname "$0")"
ROOT=../..
LOG=/tmp/dualpool_t2.log
BIN=/tmp/dualpool_splitter
RATIO=70
N=40
SHARES=3
TOL=4          # +/- connections tolerance around the target

: > "$LOG"

echo "== compiling splitter =="
make -C "$ROOT" dualpool-splitter >/dev/null 2>&1 || { echo "FAIL: compile"; exit 1; }
cp "$ROOT/dualpool-splitter" "$BIN"

SPLOG=/tmp/dualpool_t2_sp.log
python3 fake_upstream.py --port 4001 --tag A --log "$LOG" & UPA=$!
python3 fake_upstream.py --port 4002 --tag B --log "$LOG" & UPB=$!
sleep 0.6
"$BIN" --listen 3333 --poolA 127.0.0.1:4001 --poolB 127.0.0.1:4002 --ratio $RATIO >"$SPLOG" 2>&1 & SP=$!
sleep 0.5

echo "== running $N miners x $SHARES shares =="
for i in $(seq 1 $N); do
  python3 fake_miner.py 127.0.0.1 3333 $SHARES
done
sleep 0.5

kill $SP $UPA $UPB 2>/dev/null
wait 2>/dev/null

# Split from the splitter's own authoritative routing log (the health probe adds
# extra upstream connections at each pool, so pool-side conn counts are not the
# split). Share counts are clean — the probe never submits shares.
RA=$(grep -c "route -> A" "$SPLOG"); RB=$(grep -c "route -> B" "$SPLOG")
SA=$(grep -c "^A share" "$LOG");     SB=$(grep -c "^B share" "$LOG")
TOTAL=$((RA + RB))
EXP_A=$(( N * RATIO / 100 ))

echo "-------------------------------------------"
echo "splitter routed:  A=$RA  B=$RB  (total=$TOTAL, expected=$N)"
echo "target A=${RATIO}%  ->  expected A~=$EXP_A (tol +/-$TOL)"
echo "shares at pools:  A=$SA  B=$SB"
echo "-------------------------------------------"

fail=0
[ "$TOTAL" -eq "$N" ] || { echo "FAIL: routed $TOTAL != $N"; fail=1; }
if [ "$RA" -lt $((EXP_A - TOL)) ] || [ "$RA" -gt $((EXP_A + TOL)) ]; then
  echo "FAIL: realized split A=$RA outside [$((EXP_A-TOL)),$((EXP_A+TOL))]"; fail=1
fi
# routing correctness: shares at each pool == that pool's routed miners * SHARES
[ "$SA" -eq $((RA * SHARES)) ] || { echo "FAIL: A shares $SA != $((RA*SHARES)) (misrouted)"; fail=1; }
[ "$SB" -eq $((RB * SHARES)) ] || { echo "FAIL: B shares $SB != $((RB*SHARES)) (misrouted)"; fail=1; }

if [ "$fail" -eq 0 ]; then
  echo "T2 PASS: split within tolerance and all shares routed to the owning pool."
  exit 0
else
  echo "T2 FAILED"; exit 1
fi
