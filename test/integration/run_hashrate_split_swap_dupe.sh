#!/usr/bin/env bash
# HSPLIT-SWAP-DUPE: swap-back duplicate-share regression (FIX-11).
#
# Models the live asymmetry that made a solo pool reject ~60% on pool swaps while
# Kryptex stayed at 0:
#   pool A = solo-like  : LOW job churn (--notify-ms 6000, one fresh job / 6 s)
#   pool B = kryptex-like: HIGH job churn (--interval 1.0, a fresh job / second)
# Both pools run --reject-dupes: a real solo pool rejects the IDENTICAL shares an
# ASIC re-emits when the mux force-clean re-presents a job it already mined. The
# miner (fake_miner_swap) models an ASIC: on clean_jobs=true it flushes and
# re-mines the job from scratch, so a re-presented unchanged job => duplicate
# submits.
#
# Short mux slices (min 1 / max 2) make swaps far more frequent than pool A's
# 6 s notify, so nearly every swap-back to A lands on the SAME job.
#
# Two runs, one binary:
#   RED  (SPLITMUX_SWAP_REPRESENT=1): old behaviour re-presents the unchanged job
#        force-clean  -> pool A logs reject-duplicate  (proves the harness bites).
#   GREEN (default, FIX-11)         : the mux only presents genuinely fresh jobs
#        -> ZERO duplicates, while both pools still receive shares (swap stays
#        responsive).
#
#   docker run --rm -v "$PWD":/repo -w /repo dualpool-dev \
#       bash test/integration/run_hashrate_split_swap_dupe.sh
set -u
cd "$(dirname "$0")"
ROOT=../..
PROBE=/tmp/splitmux_probe_dupe
LPORT=3338
APORT=4031
BPORT=4032
RUN_S=22

echo "== compiling splitmux_probe (swap-dupe) =="
PROBE_OUT=$(gcc -std=c11 -Wall -Wextra -I"$ROOT/src" \
  splitmux_probe.c "$ROOT/src/splitmux.c" "$ROOT/src/stratum_msg.c" \
  "$ROOT/src/split_sched.c" -ljansson -lpthread -lm -o "$PROBE" 2>&1)
PROBE_RC=$?
echo "$PROBE_OUT"
if [ "$PROBE_RC" -ne 0 ]; then echo "SWAP-DUPE FAILED: probe compile error"; exit 1; fi
if echo "$PROBE_OUT" | grep -qi "warning:"; then
  echo "SWAP-DUPE FAILED: probe compile emitted warnings"; exit 1; fi

# $1=label  $2=represent(0/1) -> echoes "A_dupe B_dupe A_share B_share swaps rej minerrc"
run_phase() {
  local label="$1" represent="$2"
  local LOG="/tmp/hsplit_dupe_${label}.log"
  local MINERLOG="/tmp/hsplit_dupe_${label}_miner.log"
  : > "$LOG"; : > "$MINERLOG"
  python3 fake_upstream.py --port "$APORT" --tag A --log "$LOG" \
    --notify-ms 6000 --reject-dupes & local UPA=$!
  python3 fake_upstream.py --port "$BPORT" --tag B --log "$LOG" \
    --interval 1.0 --reject-dupes & local UPB=$!
  sleep 0.6
  SPLITMUX_SWAP_REPRESENT="$represent" \
    "$PROBE" --listen "$LPORT" --upstream "127.0.0.1:$APORT" \
    --upstream2 "127.0.0.1:$BPORT" \
    --ratio 50 --target 2 --min 1 --max 2 & local PB=$!
  sleep 0.4
  python3 fake_miner_swap.py 127.0.0.1 "$LPORT" "$RUN_S" wallet.dupe off \
    > "$MINERLOG" 2>&1
  local MRC=$?
  sleep 0.5
  kill "$PB" "$UPA" "$UPB" 2>/dev/null
  wait "$PB" "$UPA" "$UPB" 2>/dev/null

  local ADUP BDUP ASH BSH SWAPS REJ
  ADUP=$(grep -c "^A reject-duplicate" "$LOG")
  BDUP=$(grep -c "^B reject-duplicate" "$LOG")
  ASH=$(grep -c "^A share" "$LOG")
  BSH=$(grep -c "^B share" "$LOG")
  SWAPS=$(grep -c "set_extranonce" "$MINERLOG")
  REJ=$(grep -o "rejected=[0-9]*" "$MINERLOG" | tail -1 | sed 's/rejected=//'); REJ=${REJ:-0}
  echo "  [$label] A dupes=$ADUP B dupes=$BDUP | A shares=$ASH B shares=$BSH | swaps=$SWAPS miner_rejected=$REJ rc=$MRC"
  echo "$ADUP $BDUP $ASH $BSH $SWAPS $REJ $MRC" > "/tmp/hsplit_dupe_${label}.res"
}

echo "== RED phase (SPLITMUX_SWAP_REPRESENT=1: old re-present behaviour) =="
run_phase red 1
echo "== GREEN phase (FIX-11: present only fresh jobs) =="
run_phase green 0

read RADUP RBDUP RASH RBSH RSWAPS RREJ RRC < /tmp/hsplit_dupe_red.res
read GADUP GBDUP GASH GBSH GSWAPS GREJ GRC < /tmp/hsplit_dupe_green.res

echo "-------------------------------------------"
echo "RED  : A dupes=$RADUP  B dupes=$RBDUP  A/B shares=$RASH/$RBSH  swaps=$RSWAPS"
echo "GREEN: A dupes=$GADUP  B dupes=$GBDUP  A/B shares=$GASH/$GBSH  swaps=$GSWAPS"
echo "-------------------------------------------"

fail=0
# Negative control: the OLD behaviour MUST produce duplicate rejects at solo pool A.
[ "$RADUP" -ge 1 ] || { echo "FAIL: RED produced no A duplicates — control invalid"; fail=1; }
# The fix: NO duplicate rejects at either pool.
[ "$GADUP" -eq 0 ] || { echo "FAIL: GREEN still produced $GADUP A duplicate(s) — FIX-11 regressed"; fail=1; }
[ "$GBDUP" -eq 0 ] || { echo "FAIL: GREEN produced $GBDUP B duplicate(s)"; fail=1; }
# Swap responsiveness preserved: both pools still mined, swaps still reached miner.
[ "$GASH" -ge 2 ] || { echo "FAIL: GREEN pool A got < 2 shares (swap stalled?)"; fail=1; }
[ "$GBSH" -ge 2 ] || { echo "FAIL: GREEN pool B got < 2 shares"; fail=1; }
[ "$GSWAPS" -ge 1 ] || { echo "FAIL: GREEN no swap reached the miner"; fail=1; }
[ "$GRC" -eq 0 ] || { echo "FAIL: GREEN miner exit $GRC != 0"; fail=1; }

if [ "$fail" -eq 0 ]; then
  echo "HSPLIT-SWAP-DUPE PASS: RED reproduces solo dupes; FIX-11 eliminates them, swaps stay responsive."
  exit 0
else
  echo "HSPLIT-SWAP-DUPE FAILED"; exit 1
fi
