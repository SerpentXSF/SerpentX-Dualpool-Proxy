#!/usr/bin/env bash
# HSPLIT-BADHINT integration test: the mux must not relay a miner's IMPLAUSIBLE
# mining.suggest_difficulty to the pools.
#
# The live failure this reproduces. A 12.8 TH/s miner ships a hardcoded
# small-miner firmware default, mining.suggest_difficulty [4000], and repeats it
# fourteen times a session. At that hashrate 4000 is one share every 1.3 s — a
# flood. The mux relayed the hint verbatim to BOTH pools (including the one
# currently IDLE, on every repeat) while its own correctly-sized suggestion was
# vetoed. The pool believed the hint, its vardiff ramped 8192 -> ... -> 1,000,000
# chasing the flood back down, and EVERY upward step stranded the in-flight
# low-difficulty shares as "Above target": 131 rejects out of 387 (34%) on that
# pool. The identical hint is CORRECT for the 1.5 TH/s miner alongside it
# (11.5 s/share), which ran at 0.02% — so the hint itself is not the bug; relaying
# it without checking it against the rate we can measure is.
#
# Model: fake_upstream.py --vardiff — raises difficulty when shares arrive fast,
# lowers it when slow, and rejects any submit whose job it issued BELOW its
# current difficulty ("Above target"). A mining.suggest_difficulty still wins
# outright and re-seeds the ramp, which is what makes the repeats expensive.
#
#   RED   SPLITMUX_NO_CLAMP=1 — the hint is relayed verbatim (pre-fix). Assert the
#         ramp happens (difficulty climbs over many steps, and keeps climbing to
#         the last line of the run) and a substantial fraction of submits come
#         back rejected.
#   GREEN clamp on. Assert the pools are told the mux's own target instead of
#         4000 — on EVERY repeat, not just the first — that the ramp collapses to
#         the warm-up only, and that the settled half of the run strands nothing.
#         Pool B is a LATE secondary (--ready-delay), so the value it receives
#         comes from the miner_sugg replay path; asserting B never sees 4000
#         proves the CORRECTED line is what gets stored for the replay.
#         The estimate the clamp acted on is asserted against the miner's true
#         rate too — that is the share-weighting fix, read off the decision that
#         depends on it.
#   SMALL passthrough control: the same [4000] hint from a miner it is PLAUSIBLE
#         for must reach the pools UNTOUCHED (and log no clamp).
#
# The first hint of a session always goes out untouched: at connect the mux has no
# rate estimate, and it does not contradict a miner it cannot yet measure. So every
# GREEN run has a warm-up in which the pool ramps exactly as RED does. The fix is
# that it STOPS — which is what the second-half window measures.
#
# Sizing (SPLITMUX_SUGGEST_TARGET_S compresses the 15 s production constant to 3 s
# so the whole path runs in a 60 s test; the 2 s plausibility floor is NOT scaled):
#     miner            100 TH/s
#     asked 4000    -> 4000 * 2^32 / 100e12  = 0.17 s/share  (< 2 s: implausible)
#     mux target    -> 100e12 * 3 / 2^32     = 69,849 -> 65,536 (power of two)
#     at 65,536     -> 2.8 s/share           (>= 2 s: the clamp is idempotent)
#   small control     5 TH/s
#     asked 4000    -> 4000 * 2^32 / 5e12    = 3.4 s/share    (plausible: untouched)
#
# Run inside the dualpool-dev image (python3+gcc+make+jansson):
#   docker run --rm -v "$PWD":/repo -w /repo dualpool-dev \
#       bash test/integration/run_hashrate_split_badhint.sh
set -u
cd "$(dirname "$0")"
ROOT=../..
PROBE=/tmp/splitmux_probe_badhint
LPORT=3358
APORT=4081
BPORT=4082
RUN_S=60
HINT=4000            # the hardcoded firmware default the miner ships
HINT_EVERY=5         # seconds between repeats (live: 14 over a session)
THS=100              # simulated miner hashrate (TH/s) — 4000 is absurd for it
SMALL_THS=5          # control miner — 4000 is correct for it (3.4 s/share)
TARGET_S=3           # compressed SUGGEST_TARGET_S for this test
OPENDIFF=8192        # where the live pool's ramp started
VD_TARGET=4          # pool's vardiff aim (s/share)
VD_WINDOW=2
VD_MAX=1000000       # where the live pool's ramp ended
READY_DELAY=6        # pool B joins late => it is fed via the miner_sugg replay
BAND_HI=262144       # GREEN: difficulty must never climb past this

# ---------------------------------------------------------------------------
# 1. Compile the probe (0-warning gate).
# ---------------------------------------------------------------------------
echo "== compiling splitmux_probe (badhint) =="
PROBE_OUT=$(gcc -std=c11 -Wall -Wextra -I"$ROOT/src" \
  splitmux_probe.c "$ROOT/src/splitmux.c" "$ROOT/src/stratum_msg.c" \
  "$ROOT/src/split_sched.c" -ljansson -lpthread -lm -o "$PROBE" 2>&1)
PROBE_RC=$?
echo "$PROBE_OUT"
if [ "$PROBE_RC" -ne 0 ]; then echo "HSPLIT-BADHINT FAILED: probe compile error"; exit 1; fi
if echo "$PROBE_OUT" | grep -qi "warning:"; then
  echo "HSPLIT-BADHINT FAILED: probe compile emitted warnings"; exit 1
fi

# ---------------------------------------------------------------------------
# One case. args: <name> <no_clamp 0|1> <vardiff 0|1> <miner TH/s> <ready_delay>
# Fills the globals read by the assertions below.
# ---------------------------------------------------------------------------
run_case() {
  local name="$1" noclamp="$2" vd="$3" ths="$4" rdelay="$5"
  local LOG="/tmp/badhint_${name}.log"
  local MLOG="/tmp/badhint_${name}_miner.log"
  local PLOG="/tmp/badhint_${name}_probe.log"
  : > "$LOG"; : > "$MLOG"

  local vdargs=""
  [ "$vd" = "1" ] && vdargs="--vardiff --vardiff-target $VD_TARGET \
--vardiff-window $VD_WINDOW --vardiff-max $VD_MAX"

  echo "== case $name: miner ${ths} TH/s hint $HINT, vardiff=$vd, no_clamp=$noclamp =="
  # shellcheck disable=SC2086
  python3 fake_upstream.py --interval 1.5 --port "$APORT" --tag A \
      --opendiff "$OPENDIFF" $vdargs --log "$LOG" & UPA=$!
  # shellcheck disable=SC2086
  python3 fake_upstream.py --interval 1.5 --port "$BPORT" --tag B \
      --opendiff "$OPENDIFF" --ready-delay "$rdelay" $vdargs --log "$LOG" & UPB=$!
  sleep 0.6

  local nc_env=""
  [ "$noclamp" = "1" ] && nc_env="SPLITMUX_NO_CLAMP=1"
  env $nc_env SPLITMUX_SUGGEST_TARGET_S="$TARGET_S" SPLITMUX_DEBUG=1 \
    "$PROBE" --listen "$LPORT" \
    --upstream "127.0.0.1:$APORT" --upstream2 "127.0.0.1:$BPORT" \
    --start-pool 0 --ratio 50 --target 3 --min 3 --max 6 \
    > "$PLOG" 2>&1 & PB=$!
  sleep 0.4

  python3 fake_miner_hashrate.py 127.0.0.1 "$LPORT" "$RUN_S" wallet.bh "$ths" \
    --suggest "$HINT" --suggest-every "$HINT_EVERY" > "$MLOG" 2>&1
  MINER_RC=$?

  sleep 0.8
  kill "$PB" 2>/dev/null; wait "$PB" 2>/dev/null
  kill "$UPA" "$UPB" 2>/dev/null; wait 2>/dev/null

  # --- what the pools were TOLD -------------------------------------------
  A_SUGG_VALS=$(grep "^A suggest" "$LOG" | sed -n 's/.*diff=\([0-9.]*\).*/\1/p')
  B_SUGG_VALS=$(grep "^B suggest" "$LOG" | sed -n 's/.*diff=\([0-9.]*\).*/\1/p')
  A_SUGG_RAW=$(echo "$A_SUGG_VALS" | grep -c "^${HINT}$")
  B_SUGG_RAW=$(echo "$B_SUGG_VALS" | grep -c "^${HINT}$")
  # The value the mux says it substituted (0 = it never clamped).
  CLAMPED=$(grep -o "sending [0-9]* instead" "$PLOG" | tail -1 \
            | sed -n 's/sending \([0-9]*\) instead/\1/p'); CLAMPED=${CLAMPED:-0}
  CLAMP_LOGS=$(grep -c "implausible; sending" "$PLOG")
  # The hashrate estimate the clamp decided on (whole TH/s). This is the direct
  # read-out of the share-weighting fix: weighting a submit by the pool's CURRENT
  # difficulty instead of the difficulty its job was SHOWN at over-credits every
  # share a ramp overtakes, so a contaminated estimate reads HIGH here — exactly
  # when the clamp needs to trust it — and picks a correspondingly too-hard target.
  MEASURED=$(grep -o "measured [0-9]*\.[0-9]* TH/s" "$PLOG" | tail -1 \
             | sed -n 's/measured \([0-9]*\)\..* TH\/s/\1/p'); MEASURED=${MEASURED:-0}
  A_SUGG_FIX=0; B_SUGG_FIX=0
  if [ "$CLAMPED" -gt 0 ]; then
    A_SUGG_FIX=$(echo "$A_SUGG_VALS" | grep -c "^${CLAMPED}$")
    B_SUGG_FIX=$(echo "$B_SUGG_VALS" | grep -c "^${CLAMPED}$")
  fi

  # --- the ramp ------------------------------------------------------------
  # Every difficulty each pool pushed, and the highest it reached.
  MAX_DIFF=$(grep "setdiff" "$LOG" | sed -n 's/.*diff=\([0-9]*\).*/\1/p' \
             | sort -n | tail -1); MAX_DIFF=${MAX_DIFF:-0}
  # SECOND HALF of the run, as a settled-state window. The first hint always goes
  # out untouched — at connect the mux has no rate estimate, and by design it
  # does not contradict a miner it cannot yet measure — so every run has a
  # warm-up during which the pool ramps exactly as it does in RED, plus the one
  # upward step the correction itself causes. What the fix has to deliver is that
  # this ENDS. The same window is taken for every case, so the columns compare
  # directly: RED keeps ramping and stranding to the last line, GREEN goes quiet.
  POST_FROM=$(( $(wc -l < "$LOG") / 2 ))
  [ "$POST_FROM" -lt 1 ] && POST_FROM=1
  tail -n "+${POST_FROM}" "$LOG" > "${LOG}.post"
  # Upward steps = the ramp itself. Counted per pool, in order, over both the
  # whole run and the in-force window.
  count_steps() {   # <file> -> echoes the number of upward setdiff steps
    local f="$1" total=0 prev d
    for tag in A B; do
      prev=0
      while read -r d; do
        [ -z "$d" ] && continue
        if [ "$prev" -gt 0 ] && [ "$d" -gt "$prev" ]; then
          total=$((total + 1))
        fi
        prev=$d
      done <<EOF
$(grep "^$tag setdiff" "$f" | sed -n 's/.*diff=\([0-9]*\).*/\1/p')
EOF
    done
    echo "$total"
  }
  RAMP_STEPS=$(count_steps "$LOG")
  POST_STEPS=$(count_steps "${LOG}.post")

  # --- rejects -------------------------------------------------------------
  ABOVE=$(grep -c "reject-abovetarget" "$LOG")
  POST_ABOVE=$(grep -c "reject-abovetarget" "${LOG}.post")
  A_SHARES=$(grep -c "^A share" "$LOG")
  B_SHARES=$(grep -c "^B share" "$LOG")
  ACK_LINE=$(grep -o "acks=[0-9]* accepted=[0-9]* rejected=[0-9]* submits=[0-9]*" "$MLOG" | tail -1)
  ACCEPTED=$(echo "$ACK_LINE" | sed -n 's/.*accepted=\([0-9]*\).*/\1/p'); ACCEPTED=${ACCEPTED:-0}
  REJECTED=$(echo "$ACK_LINE" | sed -n 's/.*rejected=\([0-9]*\).*/\1/p'); REJECTED=${REJECTED:-0}
  SUBMITS=$(echo "$ACK_LINE" | sed -n 's/.*submits=\([0-9]*\).*/\1/p'); SUBMITS=${SUBMITS:-0}
  ACKS=$((ACCEPTED + REJECTED))
  REJ_PCT=0
  [ "$ACKS" -gt 0 ] && REJ_PCT=$((REJECTED * 100 / ACKS))

  echo "   miner: $ACK_LINE  (reject rate ${REJ_PCT}%)"
  echo "   pools: A shares=$A_SHARES  B shares=$B_SHARES  above-target=$ABOVE"
  echo "   told:  A [$(echo "$A_SUGG_VALS" | tr '\n' ' ')]"
  echo "          B [$(echo "$B_SUGG_VALS" | tr '\n' ' ')]"
  echo "   ramp:  max difficulty=$MAX_DIFF over $RAMP_STEPS upward step(s); clamped-to=$CLAMPED"
  [ "$CLAMPED" -gt 0 ] && \
    echo "   estimate feeding the clamp: ${MEASURED} TH/s (true ${ths} TH/s)"
  echo "   second half:     $POST_STEPS upward step(s), $POST_ABOVE above-target reject(s)"
}

fail=0

# ---------------------------------------------------------------------------
# RED — negative control: the hint is relayed verbatim. The pools believe it, and
# their vardiff ramps away from it, stranding shares on every step up. This is
# the state the live 12.8 TH/s miner was in.
# ---------------------------------------------------------------------------
run_case red 1 1 "$THS" "$READY_DELAY"
RED_MAX=$MAX_DIFF; RED_STEPS=$RAMP_STEPS; RED_REJ=$REJECTED; RED_PCT=$REJ_PCT
RED_ABOVE=$ABOVE; RED_ACKS=$ACKS; RED_POST_STEPS=$POST_STEPS; RED_POST_ABOVE=$POST_ABOVE
[ "$MINER_RC" -eq 0 ]        || { echo "FAIL[red]: miner exit $MINER_RC"; fail=1; }
[ "$CLAMPED" -eq 0 ]         || { echo "FAIL[red]: the mux clamped ($CLAMPED) despite SPLITMUX_NO_CLAMP=1"; fail=1; }
[ "$A_SUGG_RAW" -ge 2 ]      || { echo "FAIL[red]: pool A saw $A_SUGG_RAW raw $HINT hint(s) — the relay is not being exercised"; fail=1; }
[ "$B_SUGG_RAW" -ge 1 ]      || { echo "FAIL[red]: pool B (late secondary) was never armed with the raw $HINT"; fail=1; }
[ "$RED_STEPS" -ge 10 ]      || { echo "FAIL[red]: only $RED_STEPS upward vardiff step(s) — the ramp is NOT reproducing"; fail=1; }
[ "$RED_MAX" -ge $((HINT * 4)) ] || { echo "FAIL[red]: difficulty only reached $RED_MAX (from the $HINT the pools were told) — the ramp is NOT reproducing"; fail=1; }
[ "$RED_ACKS" -ge 30 ]       || { echo "FAIL[red]: only $RED_ACKS acked submits — too few to measure a reject rate"; fail=1; }
[ "$RED_PCT" -ge 15 ]        || { echo "FAIL[red]: reject rate ${RED_PCT}% — the 'Above target' stranding is NOT reproducing"; fail=1; }
[ "$RED_POST_ABOVE" -ge 3 ]  || { echo "FAIL[red]: only $RED_POST_ABOVE late 'Above target' reject(s) — the failure is not sustained"; fail=1; }
[ "$RED_POST_STEPS" -ge 4 ]  || { echo "FAIL[red]: only $RED_POST_STEPS late upward step(s) — the ramp is not sustained"; fail=1; }

# ---------------------------------------------------------------------------
# GREEN — the fix: the implausible hint is rewritten with the mux's own target
# before it leaves, on every repeat, and the stored copy (replayed to the late
# secondary B) carries the corrected value too.
# ---------------------------------------------------------------------------
run_case green 0 1 "$THS" "$READY_DELAY"
GRN_MAX=$MAX_DIFF; GRN_STEPS=$RAMP_STEPS; GRN_REJ=$REJECTED; GRN_PCT=$REJ_PCT
GRN_ABOVE=$ABOVE; GRN_ACKS=$ACKS; GRN_CLAMPED=$CLAMPED
GRN_A_SHARES=$A_SHARES; GRN_B_SHARES=$B_SHARES
GRN_POST_STEPS=$POST_STEPS; GRN_POST_ABOVE=$POST_ABOVE
[ "$MINER_RC" -eq 0 ]         || { echo "FAIL[green]: miner exit $MINER_RC"; fail=1; }
[ "$GRN_CLAMPED" -gt 0 ]      || { echo "FAIL[green]: the mux never clamped the hint"; fail=1; }
[ "$GRN_CLAMPED" -ge 16384 ]  || { echo "FAIL[green]: clamped to $GRN_CLAMPED — implausibly low for ${THS} TH/s"; fail=1; }
[ "$CLAMP_LOGS" -eq 1 ]       || { echo "FAIL[green]: $CLAMP_LOGS clamp log line(s) — expected exactly 1 (once per changed value)"; fail=1; }
# Share-weighting fix, read directly off the decision that depends on it: the
# estimate is taken DURING the warm-up ramp, the window in which crediting a
# share at the pool's current (already-raised) difficulty rather than the one its
# job was shown at inflates the figure several-fold.
[ "$MEASURED" -ge 50 ]        || { echo "FAIL[green]: the clamp acted on a ${MEASURED} TH/s estimate for a ${THS} TH/s miner — far too low"; fail=1; }
[ "$MEASURED" -le 130 ]       || { echo "FAIL[green]: the clamp acted on a ${MEASURED} TH/s estimate for a ${THS} TH/s miner — the ramp is still inflating it (share weighting)"; fail=1; }
[ "$A_SUGG_FIX" -ge 2 ]       || { echo "FAIL[green]: pool A received the corrected value only $A_SUGG_FIX time(s) — repeats are not being clamped"; fail=1; }
[ "$B_SUGG_FIX" -ge 1 ]       || { echo "FAIL[green]: pool B (late secondary) never received the corrected value $GRN_CLAMPED"; fail=1; }
[ "$B_SUGG_RAW" -eq 0 ]       || { echo "FAIL[green]: pool B was armed with the raw $HINT $B_SUGG_RAW time(s) — the replay path still carries it"; fail=1; }
[ "$GRN_MAX" -le "$BAND_HI" ] || { echo "FAIL[green]: difficulty still climbed to $GRN_MAX (band ceiling $BAND_HI) — the ramp was not prevented"; fail=1; }
[ "$GRN_MAX" -le $((GRN_CLAMPED * 2)) ] || { echo "FAIL[green]: difficulty reached $GRN_MAX, more than 2x the $GRN_CLAMPED the pools were told"; fail=1; }
# A settled GREEN run can still show an isolated upward step per pool: the pool's
# own vardiff drifts DOWN while that pool is idle for a slice, and the next
# repeat of the (corrected) hint puts it back. That is the fix asserting itself,
# not a ramp — note it costs no shares, which is what the reject count below
# pins down. The ramp proper is asserted on the whole-run step count vs RED.
[ "$GRN_POST_STEPS" -le 2 ]   || { echo "FAIL[green]: $GRN_POST_STEPS upward vardiff step(s) in the settled half — still ramping"; fail=1; }
[ "$GRN_POST_ABOVE" -eq 0 ]   || { echo "FAIL[green]: $GRN_POST_ABOVE 'Above target' reject(s) in the settled half (expected 0)"; fail=1; }
[ "$GRN_A_SHARES" -ge 3 ]     || { echo "FAIL[green]: pool A only got $GRN_A_SHARES shares"; fail=1; }
[ "$GRN_B_SHARES" -ge 2 ]     || { echo "FAIL[green]: pool B only got $GRN_B_SHARES shares"; fail=1; }
[ "$GRN_PCT" -le 10 ]         || { echo "FAIL[green]: whole-run reject rate ${GRN_PCT}% (expected <= 10%, warm-up included)"; fail=1; }
[ $((GRN_REJ * 4)) -le "$RED_REJ" ] || { echo "FAIL[green]: $GRN_REJ rejects is not decisively fewer than RED's $RED_REJ"; fail=1; }
[ $((GRN_STEPS * 4)) -le "$RED_STEPS" ] || { echo "FAIL[green]: $GRN_STEPS upward step(s) is not decisively fewer than RED's $RED_STEPS"; fail=1; }

# ---------------------------------------------------------------------------
# SMALL — passthrough control: the SAME [4000] hint from a miner it is plausible
# for (5 TH/s => 3.4 s/share) must reach the pools untouched. Plain (non-vardiff)
# pools, so the only thing under test is what the mux forwards.
# ---------------------------------------------------------------------------
run_case small 0 0 "$SMALL_THS" 0
SML_CLAMPED=$CLAMPED; SML_A_RAW=$A_SUGG_RAW; SML_B_RAW=$B_SUGG_RAW
SML_OTHER=$(echo "$A_SUGG_VALS" "$B_SUGG_VALS" | tr ' ' '\n' | grep -v "^$" | grep -cv "^${HINT}$")
[ "$MINER_RC" -eq 0 ]  || { echo "FAIL[small]: miner exit $MINER_RC"; fail=1; }
[ "$SML_CLAMPED" -eq 0 ] || { echo "FAIL[small]: a plausible $HINT hint was clamped to $SML_CLAMPED"; fail=1; }
[ "$SML_A_RAW" -ge 2 ] || { echo "FAIL[small]: pool A got $SML_A_RAW verbatim $HINT hint(s), expected >= 2"; fail=1; }
[ "$SML_B_RAW" -ge 2 ] || { echo "FAIL[small]: pool B got $SML_B_RAW verbatim $HINT hint(s), expected >= 2"; fail=1; }
[ "$SML_OTHER" -eq 0 ] || { echo "FAIL[small]: $SML_OTHER suggestion(s) other than $HINT reached the pools"; fail=1; }

echo "-------------------------------------------"
echo "RED   hint $HINT relayed verbatim -> difficulty ramped to $RED_MAX over"
echo "      $RED_STEPS upward step(s); $RED_ABOVE 'Above target' rejects, ${RED_PCT}% of acks"
echo "GREEN hint rewritten to $GRN_CLAMPED before leaving (A and B, incl. the late"
echo "      secondary's replay) -> max difficulty $GRN_MAX, $GRN_STEPS step(s),"
echo "      $GRN_ABOVE 'Above target' rejects, ${GRN_PCT}% of acks"
echo "      settled half: $GRN_POST_STEPS step(s), $GRN_POST_ABOVE reject(s)"
echo "      (RED over the same second-half window: $RED_POST_STEPS step(s), $RED_POST_ABOVE reject(s))"
echo "SMALL plausible $HINT passed through untouched to both pools"
if [ "$fail" -eq 0 ]; then
  echo "HSPLIT-BADHINT PASS: an implausible miner hint drives the vardiff ramp and"
  echo "                     its 'Above target' rejects (red control); clamping it to"
  echo "                     the measured rate removes both, while a plausible hint"
  echo "                     from a small miner is left alone."
  exit 0
else
  echo "HSPLIT-BADHINT FAILED"; exit 1
fi
