#!/usr/bin/env bash
# HSPLIT-SUGGEST integration test: the mux sends mining.suggest_difficulty on the
# miner's behalf when a pool opens a session far above the miner's reach.
#
# The live failure this reproduces: Kryptex opens a session at difficulty
# 1,000,000. A miner that does NOT send its own mining.suggest_difficulty finds a
# share only every diff*2^32/hashrate seconds there (~5.5 min at 12.9 TH/s), so
# the pool's vardiff never gets samples to ramp down with, the miner sees almost
# no accepted shares, and its watchdog drops and reconnects — an ~18-minute loop
# producing nothing. The mux already MEASURES the miner (total_work / active
# time), so it can tell the pool how big this miner is.
#
# Both pools are started with --opendiff 1000000 and honour suggest_difficulty.
# The miner (fake_miner_hashrate.py) mines at a fixed simulated hashrate and never
# suggests anything itself.
#
#   RED   SPLITMUX_NO_SUGGEST=1 — the mux never suggests (pre-fix behaviour).
#         Assert: BOTH pools stay at 1,000,000 and the miner produces ~no shares.
#   GREEN suggestions on. Assert: both pools come DOWN to the suggested
#         difficulty, BOTH are mined, ~0 rejects, and at most ONE suggestion per
#         pool (no spam).
#
# Time compression: the production target is one share per SUGGEST_TARGET_S = 15 s,
# which means a >4x mismatch implies a >60 s wait for the first share — the whole
# point of the bug. SPLITMUX_SUGGEST_TARGET_S (test-only env, see splitmux.c)
# compresses that constant to 1 s so the SAME code path runs in seconds. With the
# miner at 357.9 TH/s:
#     first share at 1,000,000  = 1e6 * 2^32 / 357.9e12 ~= 12 s
#     suggested difficulty      = 357.9e12 * 1 / 2^32 = 83,333 -> 65,536 (pow2)
#     mismatch                  = 1e6 / 65,536 = 15.3x  (> the 4x trigger)
#     share interval after      = 65,536 * 2^32 / 357.9e12 ~= 0.79 s
#
# Run inside the dualpool-dev image (python3+gcc+make+jansson):
#   docker run --rm -v "$PWD":/repo -w /repo dualpool-dev \
#       bash test/integration/run_hashrate_split_suggest.sh
set -u
cd "$(dirname "$0")"
ROOT=../..
PROBE=/tmp/splitmux_probe_suggest
LPORT=3357
APORT=4061
BPORT=4062
RUN_S=45
OPENDIFF=1000000
THS=357.9            # simulated miner hashrate (TH/s)
EXPECT_DIFF=65536    # power-of-two target the mux should land on
TARGET_S=1           # compressed SUGGEST_TARGET_S for this test

# ---------------------------------------------------------------------------
# 1. Compile the probe (0-warning gate).
# ---------------------------------------------------------------------------
echo "== compiling splitmux_probe (suggest) =="
PROBE_OUT=$(gcc -std=c11 -Wall -Wextra -I"$ROOT/src" \
  splitmux_probe.c "$ROOT/src/splitmux.c" "$ROOT/src/stratum_msg.c" \
  "$ROOT/src/split_sched.c" -ljansson -lpthread -lm -o "$PROBE" 2>&1)
PROBE_RC=$?
echo "$PROBE_OUT"
if [ "$PROBE_RC" -ne 0 ]; then echo "HSPLIT-SUGGEST FAILED: probe compile error"; exit 1; fi
if echo "$PROBE_OUT" | grep -qi "warning:"; then
  echo "HSPLIT-SUGGEST FAILED: probe compile emitted warnings"; exit 1
fi

# ---------------------------------------------------------------------------
# One case: two pools opening at $OPENDIFF, the probe (optionally with
# SPLITMUX_NO_SUGGEST), and the fixed-hashrate miner. Fills the globals below.
# args: <name> <no_suggest 0|1>
# ---------------------------------------------------------------------------
run_case() {
  local name="$1" nosug="$2"
  local LOG="/tmp/suggest_${name}.log"
  local MLOG="/tmp/suggest_${name}_miner.log"
  : > "$LOG"; : > "$MLOG"

  echo "== case $name: A/B --opendiff $OPENDIFF, miner ${THS} TH/s, no_suggest=$nosug =="
  python3 fake_upstream.py --interval 1.0 --port "$APORT" --tag A \
      --opendiff "$OPENDIFF" --log "$LOG" & UPA=$!
  python3 fake_upstream.py --interval 1.0 --port "$BPORT" --tag B \
      --opendiff "$OPENDIFF" --log "$LOG" & UPB=$!
  sleep 0.6

  local nosug_env=""
  [ "$nosug" = "1" ] && nosug_env="SPLITMUX_NO_SUGGEST=1"
  env $nosug_env SPLITMUX_SUGGEST_TARGET_S="$TARGET_S" SPLITMUX_DEBUG=1 \
    "$PROBE" --listen "$LPORT" \
    --upstream "127.0.0.1:$APORT" --upstream2 "127.0.0.1:$BPORT" \
    --ratio 50 --target 3 --min 3 --max 6 \
    > "/tmp/suggest_${name}_probe.log" 2>&1 & PB=$!
  sleep 0.4

  python3 fake_miner_hashrate.py 127.0.0.1 "$LPORT" "$RUN_S" wallet.hr "$THS" \
    > "$MLOG" 2>&1
  MINER_RC=$?

  sleep 0.6
  kill "$PB" 2>/dev/null; wait "$PB" 2>/dev/null
  kill "$UPA" "$UPB" 2>/dev/null; wait 2>/dev/null

  A_SHARES=$(grep -c "^A share" "$LOG")
  B_SHARES=$(grep -c "^B share" "$LOG")
  TOTAL_SHARES=$((A_SHARES + B_SHARES))
  A_SUGG=$(grep -c "^A suggest" "$LOG")
  B_SUGG=$(grep -c "^B suggest" "$LOG")
  # Last difficulty each pool actually pushed: RED leaves it at the opening value.
  A_DIFF=$(grep "^A setdiff" "$LOG" | tail -1 | sed -n 's/.*diff=\([0-9]*\).*/\1/p'); A_DIFF=${A_DIFF:-0}
  B_DIFF=$(grep "^B setdiff" "$LOG" | tail -1 | sed -n 's/.*diff=\([0-9]*\).*/\1/p'); B_DIFF=${B_DIFF:-0}
  ACK_LINE=$(grep -o "acks=[0-9]* accepted=[0-9]* rejected=[0-9]* submits=[0-9]*" "$MLOG" | tail -1)
  ACCEPTED=$(echo "$ACK_LINE" | sed -n 's/.*accepted=\([0-9]*\).*/\1/p'); ACCEPTED=${ACCEPTED:-0}
  REJECTED=$(echo "$ACK_LINE" | sed -n 's/.*rejected=\([0-9]*\).*/\1/p'); REJECTED=${REJECTED:-0}
  SUBMITS=$(echo "$ACK_LINE" | sed -n 's/.*submits=\([0-9]*\).*/\1/p'); SUBMITS=${SUBMITS:-0}

  echo "   miner: $ACK_LINE"
  echo "   pool A: shares=$A_SHARES suggest=$A_SUGG final_diff=$A_DIFF"
  echo "   pool B: shares=$B_SHARES suggest=$B_SUGG final_diff=$B_DIFF"
}

fail=0

# ---------------------------------------------------------------------------
# RED — negative control: no proactive suggestion. Pools sit at 1,000,000 and
# the miner is starved. This is the state the live 12.9 TH/s miner was stuck in.
# ---------------------------------------------------------------------------
run_case red 1
RED_TOTAL=$TOTAL_SHARES
RED_A_DIFF=$A_DIFF; RED_B_DIFF=$B_DIFF
RED_SUGG=$((A_SUGG + B_SUGG))
[ "$MINER_RC" -eq 0 ]              || { echo "FAIL[red]: miner exit $MINER_RC"; fail=1; }
[ "$RED_SUGG" -eq 0 ]              || { echo "FAIL[red]: $RED_SUGG suggestion(s) reached the pools despite SPLITMUX_NO_SUGGEST=1"; fail=1; }
[ "$RED_A_DIFF" -eq "$OPENDIFF" ]  || { echo "FAIL[red]: pool A left its opening difficulty (now $RED_A_DIFF)"; fail=1; }
[ "$RED_B_DIFF" -eq "$OPENDIFF" ]  || { echo "FAIL[red]: pool B left its opening difficulty (now $RED_B_DIFF)"; fail=1; }
[ "$RED_TOTAL" -le 6 ]             || { echo "FAIL[red]: $RED_TOTAL shares in ${RUN_S}s — the harness is NOT reproducing the starvation"; fail=1; }

# ---------------------------------------------------------------------------
# GREEN — the fix: the mux suggests from its own hashrate estimate; both pools
# come down and both get mined.
# ---------------------------------------------------------------------------
run_case green 0
GRN_A_SHARES=$A_SHARES; GRN_B_SHARES=$B_SHARES; GRN_TOTAL=$TOTAL_SHARES
GRN_A_DIFF=$A_DIFF; GRN_B_DIFF=$B_DIFF
GRN_A_SUGG=$A_SUGG; GRN_B_SUGG=$B_SUGG
GRN_ACCEPTED=$ACCEPTED; GRN_REJECTED=$REJECTED; GRN_SUBMITS=$SUBMITS
[ "$MINER_RC" -eq 0 ]                 || { echo "FAIL[green]: miner exit $MINER_RC"; fail=1; }
[ "$GRN_A_SUGG" -ge 1 ]               || { echo "FAIL[green]: pool A never received a suggestion"; fail=1; }
[ "$GRN_B_SUGG" -ge 1 ]               || { echo "FAIL[green]: pool B never received a suggestion"; fail=1; }
[ "$GRN_A_SUGG" -le 1 ]               || { echo "FAIL[green]: pool A got $GRN_A_SUGG suggestions in ${RUN_S}s (spam; limit is 1 per 120s)"; fail=1; }
[ "$GRN_B_SUGG" -le 1 ]               || { echo "FAIL[green]: pool B got $GRN_B_SUGG suggestions in ${RUN_S}s (spam; limit is 1 per 120s)"; fail=1; }
[ "$GRN_A_DIFF" -eq "$EXPECT_DIFF" ]  || { echo "FAIL[green]: pool A ended at difficulty $GRN_A_DIFF, expected $EXPECT_DIFF"; fail=1; }
[ "$GRN_B_DIFF" -eq "$EXPECT_DIFF" ]  || { echo "FAIL[green]: pool B ended at difficulty $GRN_B_DIFF, expected $EXPECT_DIFF"; fail=1; }
[ "$GRN_A_SHARES" -ge 3 ]             || { echo "FAIL[green]: pool A only got $GRN_A_SHARES shares"; fail=1; }
[ "$GRN_B_SHARES" -ge 3 ]             || { echo "FAIL[green]: pool B only got $GRN_B_SHARES shares"; fail=1; }
[ "$GRN_REJECTED" -eq 0 ]             || { echo "FAIL[green]: miner saw $GRN_REJECTED rejects (expected 0)"; fail=1; }
[ "$GRN_TOTAL" -ge 15 ]               || { echo "FAIL[green]: only $GRN_TOTAL shares total in ${RUN_S}s"; fail=1; }
[ "$GRN_TOTAL" -gt $((RED_TOTAL * 3)) ] || { echo "FAIL[green]: $GRN_TOTAL shares is not decisively more than RED's $RED_TOTAL"; fail=1; }

echo "-------------------------------------------"
echo "RED   pools A=$RED_A_DIFF B=$RED_B_DIFF, $RED_TOTAL shares in ${RUN_S}s (starved)"
echo "GREEN pools A=$GRN_A_DIFF B=$GRN_B_DIFF, $GRN_TOTAL shares (A=$GRN_A_SHARES B=$GRN_B_SHARES),"
echo "      rejects=$GRN_REJECTED, suggestions A=$GRN_A_SUGG B=$GRN_B_SUGG"
if [ "$fail" -eq 0 ]; then
  echo "HSPLIT-SUGGEST PASS: a pool opening at $OPENDIFF starves the miner without the"
  echo "                     mux's suggestion (red control) and is talked down to"
  echo "                     $EXPECT_DIFF with it — one suggestion per pool, no spam."
  exit 0
else
  echo "HSPLIT-SUGGEST FAILED"; exit 1
fi
