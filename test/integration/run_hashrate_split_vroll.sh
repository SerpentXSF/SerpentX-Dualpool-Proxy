#!/usr/bin/env bash
# HSPLIT-VROLL integration test: ASICBoost version-rolling negotiation across the
# dual-pool mux. Proves the mux relays the pool's REAL granted version mask to the
# miner (instead of {}), so a version-rolling ASIC's rolled shares are ACCEPTED on
# both pools — and that the harness now CATCHES the regression it previously missed.
#
# Three cases, all driving splitmux_run() via the probe with two --vmask upstreams
# and the version-rolling fake miner:
#
#   GREEN     both pools grant 1fffe000. The mux relays it; the miner rolls within
#             the mask. Assert: both pools ACCEPT, ZERO version rejects.
#   RED       identical, but SPLITMUX_VROLL_OFF=1 reverts the mux to the old {}
#             reply (negative control). The miner rolls a wrong default; both pools
#             REJECT ("Invalid version"). Assert: rejects appear, ~0 accepted.
#             This is the proof the harness catches the bug.
#   MISMATCH  pool A grants 1fffe000, pool B grants a NARROWER 1c000000. The mux
#             must send the miner mining.set_version_mask with the AND-intersection
#             (1c000000) so shares stay valid on BOTH. Assert: miner adopts the
#             intersection, both pools ACCEPT, ZERO rejects.
#
# Run inside the dualpool-dev image (python3+gcc+make+jansson):
#   docker run --rm -v "$PWD":/repo -w /repo dualpool-dev \
#       bash test/integration/run_hashrate_split_vroll.sh
set -u
cd "$(dirname "$0")"
ROOT=../..
PROBE=/tmp/splitmux_probe_vroll
LPORT=3355
APORT=4051
BPORT=4052
RUN_S=18

# ---------------------------------------------------------------------------
# 1. Compile the probe (0-warning gate).
# ---------------------------------------------------------------------------
echo "== compiling splitmux_probe (vroll) =="
PROBE_OUT=$(gcc -std=c11 -Wall -Wextra -I"$ROOT/src" \
  splitmux_probe.c "$ROOT/src/splitmux.c" "$ROOT/src/stratum_msg.c" \
  "$ROOT/src/split_sched.c" -ljansson -lpthread -lm -o "$PROBE" 2>&1)
PROBE_RC=$?
echo "$PROBE_OUT"
if [ "$PROBE_RC" -ne 0 ]; then echo "HSPLIT-VROLL FAILED: probe compile error"; exit 1; fi
if echo "$PROBE_OUT" | grep -qi "warning:"; then
  echo "HSPLIT-VROLL FAILED: probe compile emitted warnings"; exit 1
fi

# ---------------------------------------------------------------------------
# One case: start A/B upstreams (with given vmasks), the probe (optionally with
# SPLITMUX_VROLL_OFF), and the vroll miner. Echoes the tallies into globals.
# args: <name> <a_vmask> <b_vmask> <vroll_off 0|1>
# ---------------------------------------------------------------------------
run_case() {
  local name="$1" avmask="$2" bvmask="$3" vroff="$4"
  local LOG="/tmp/vroll_${name}.log"
  local MLOG="/tmp/vroll_${name}_miner.log"
  : > "$LOG"; : > "$MLOG"

  echo "== case $name: A(--vmask $avmask) B(--vmask $bvmask) vroll_off=$vroff =="
  python3 fake_upstream.py --interval 1.0 --port "$APORT" --tag A --vmask "$avmask" --log "$LOG" & UPA=$!
  python3 fake_upstream.py --interval 1.0 --port "$BPORT" --tag B --vmask "$bvmask" --log "$LOG" & UPB=$!
  sleep 0.6

  local voff_env=""
  [ "$vroff" = "1" ] && voff_env="SPLITMUX_VROLL_OFF=1"
  env $voff_env SPLITMUX_DEBUG=1 "$PROBE" --listen "$LPORT" \
    --upstream "127.0.0.1:$APORT" --upstream2 "127.0.0.1:$BPORT" \
    --ratio 50 --target 3 --min 2 --max 5 > "/tmp/vroll_${name}_probe.log" 2>&1 & PB=$!
  sleep 0.4

  python3 fake_miner_vroll.py 127.0.0.1 "$LPORT" "$RUN_S" wallet.vroll 1fffe000 > "$MLOG" 2>&1
  MINER_RC=$?

  sleep 0.6
  kill "$PB" 2>/dev/null; wait "$PB" 2>/dev/null
  kill "$UPA" "$UPB" 2>/dev/null; wait 2>/dev/null

  A_SHARES=$(grep -c "^A share" "$LOG")
  B_SHARES=$(grep -c "^B share" "$LOG")
  A_REJECT=$(grep -c "^A reject-version" "$LOG")
  B_REJECT=$(grep -c "^B reject-version" "$LOG")
  SVM=$(grep -o "set_version_mask [0-9a-f]*" "$MLOG" | tail -1 | awk '{print $2}')
  ACK_LINE=$(grep -o "acks=[0-9]* accepted=[0-9]* rejected=[0-9]* submits=[0-9]*" "$MLOG" | tail -1)
  ACCEPTED=$(echo "$ACK_LINE" | sed -n 's/.*accepted=\([0-9]*\).*/\1/p'); ACCEPTED=${ACCEPTED:-0}
  REJECTED=$(echo "$ACK_LINE" | sed -n 's/.*rejected=\([0-9]*\).*/\1/p'); REJECTED=${REJECTED:-0}
  SUBMITS=$(echo "$ACK_LINE" | sed -n 's/.*submits=\([0-9]*\).*/\1/p'); SUBMITS=${SUBMITS:-0}

  echo "   miner: $ACK_LINE  set_version_mask=${SVM:-<none>}"
  echo "   pool A: shares=$A_SHARES reject-version=$A_REJECT | pool B: shares=$B_SHARES reject-version=$B_REJECT"
}

fail=0

# ---------------------------------------------------------------------------
# GREEN — the fix: both pools grant 1fffe000, relayed to the miner -> accepted.
# ---------------------------------------------------------------------------
run_case green 1fffe000 1fffe000 0
GREEN_A_SHARES=$A_SHARES; GREEN_B_SHARES=$B_SHARES
GREEN_A_REJECT=$A_REJECT; GREEN_B_REJECT=$B_REJECT
GREEN_ACCEPTED=$ACCEPTED; GREEN_REJECTED=$REJECTED; GREEN_SUBMITS=$SUBMITS
[ "$MINER_RC" -eq 0 ]        || { echo "FAIL[green]: miner exit $MINER_RC"; fail=1; }
[ "$GREEN_SUBMITS" -ge 5 ]   || { echo "FAIL[green]: miner sent < 5 submits (degenerate)"; fail=1; }
[ "$GREEN_A_SHARES" -ge 1 ]  || { echo "FAIL[green]: pool A accepted 0 shares"; fail=1; }
[ "$GREEN_B_SHARES" -ge 1 ]  || { echo "FAIL[green]: pool B accepted 0 shares (swap never delivered a valid roll)"; fail=1; }
[ "$GREEN_A_REJECT" -eq 0 ]  || { echo "FAIL[green]: pool A version-rejected $GREEN_A_REJECT shares"; fail=1; }
[ "$GREEN_B_REJECT" -eq 0 ]  || { echo "FAIL[green]: pool B version-rejected $GREEN_B_REJECT shares"; fail=1; }
[ "$GREEN_REJECTED" -eq 0 ]  || { echo "FAIL[green]: miner saw $GREEN_REJECTED rejects (expected 0)"; fail=1; }
[ "$GREEN_ACCEPTED" -ge 3 ]  || { echo "FAIL[green]: only $GREEN_ACCEPTED accepted"; fail=1; }

# ---------------------------------------------------------------------------
# RED — negative control: revert the relay -> miner rolls wrong default -> reject.
# ---------------------------------------------------------------------------
run_case red 1fffe000 1fffe000 1
RED_REJECT=$((A_REJECT + B_REJECT))
RED_ACCEPTED=$ACCEPTED; RED_REJECTED=$REJECTED
[ "$RED_REJECT" -ge 3 ]      || { echo "FAIL[red]: pools version-rejected only $RED_REJECT shares (harness did not catch the bug)"; fail=1; }
[ "$RED_REJECTED" -ge 3 ]    || { echo "FAIL[red]: miner saw only $RED_REJECTED rejects"; fail=1; }
[ "$RED_ACCEPTED" -eq 0 ]    || { echo "FAIL[red]: $RED_ACCEPTED shares accepted despite dropped negotiation (expected 0)"; fail=1; }

# ---------------------------------------------------------------------------
# MISMATCH — A 1fffe000, B narrower 1c000000: mux must send the intersection.
# ---------------------------------------------------------------------------
run_case mismatch 1fffe000 1c000000 0
MM_A_SHARES=$A_SHARES; MM_B_SHARES=$B_SHARES
MM_A_REJECT=$A_REJECT; MM_B_REJECT=$B_REJECT
MM_SVM=$SVM; MM_REJECTED=$REJECTED
[ "$MM_A_REJECT" -eq 0 ]     || { echo "FAIL[mismatch]: pool A version-rejected $MM_A_REJECT shares"; fail=1; }
[ "$MM_B_REJECT" -eq 0 ]     || { echo "FAIL[mismatch]: pool B version-rejected $MM_B_REJECT shares"; fail=1; }
[ "$MM_A_SHARES" -ge 1 ]     || { echo "FAIL[mismatch]: pool A accepted 0 shares"; fail=1; }
[ "$MM_B_SHARES" -ge 1 ]     || { echo "FAIL[mismatch]: pool B accepted 0 shares"; fail=1; }
[ "$MM_SVM" = "1c000000" ]   || { echo "FAIL[mismatch]: miner adopted mask '${MM_SVM:-<none>}', expected intersection 1c000000"; fail=1; }
[ "$MM_REJECTED" -eq 0 ]     || { echo "FAIL[mismatch]: miner saw $MM_REJECTED rejects (expected 0)"; fail=1; }

echo "-------------------------------------------"
if [ "$fail" -eq 0 ]; then
  echo "HSPLIT-VROLL PASS: relay grants ACCEPT (green), dropped negotiation REJECTS"
  echo "                   (red control), narrower pool -> set_version_mask intersection."
  exit 0
else
  echo "HSPLIT-VROLL FAILED"; exit 1
fi
