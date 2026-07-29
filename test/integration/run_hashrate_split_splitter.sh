#!/usr/bin/env bash
# HSPLIT-SPLITTER: the capstone — hashrate_split through the REAL splitter binary.
#
# Every earlier HSPLIT test drove splitmux_run() via the standalone probe. This
# one wires the mux where it actually lives: the production splitter, in
# --mode hashrate_split, multiplexing ONE miner across two tagged fake upstreams.
# It proves the M6.2 accept-loop branch + the ratio-weighted start_pool rotation.
#
# Scenario 1 (smooth miner): fake_miner_split.py advertises
# mining.extranonce.subscribe, so the mux performs in-connection smooth swaps.
# One connection should therefore mine BOTH pools with clean per-pool routing,
# and the splitter must log the HASHRATE-SPLIT startup line.
#   asserts: A shares >=2, B shares >=2, 0 cross-routed, startup line present,
#            miner exited 0.
#
# Scenario 2 (naive miner): fake_miner_naive.py never subscribes extranonce, so
# the mux reconnect-slices it (drops at the slice deadline, never sends
# set_extranonce). The splitter rebinds each reconnect to the next pool via the
# ratio-weighted start_pool rotation, so BOTH pools receive shares across the
# reconnects with ZERO set_extranonce ever delivered. This is what the probe M5
# test could only *simulate* with --alt-start; here the real splitter does it.
#   asserts: A shares >=1, B shares >=1, set_extranonce == 0, conns >=2, miner 0.
#
# Run inside the dualpool-dev image (python3+gcc+make+jansson):
#   docker run --rm -v "$PWD":/repo -w /repo dualpool-dev \
#       bash test/integration/run_hashrate_split_splitter.sh
set -u
cd "$(dirname "$0")"
ROOT=../..
BIN=/tmp/dualpool_splitter_hsplit
LPORT=3334
APORT=4001
BPORT=4002
LOG=/tmp/hsplit_splitter.log            # combined tagged upstream log
SPLOG=/tmp/hsplit_splitter_sp.log       # splitter stderr
SMOOTHLOG=/tmp/hsplit_splitter_smooth.log
NAIVELOG=/tmp/hsplit_splitter_naive.log
RUN_S=20

# ---------------------------------------------------------------------------
# 1. Build the REAL splitter (0-warning gate).
# ---------------------------------------------------------------------------
echo "== compiling dualpool-splitter =="
BUILD_OUT=$(make -C "$ROOT" dualpool-splitter 2>&1)
BUILD_RC=$?
echo "$BUILD_OUT"
if [ "$BUILD_RC" -ne 0 ]; then echo "HSPLIT-SPLITTER FAILED: compile error"; exit 1; fi
if echo "$BUILD_OUT" | grep -qi "warning:"; then
  echo "HSPLIT-SPLITTER FAILED: build emitted warnings"; exit 1; fi
cp "$ROOT/dualpool-splitter" "$BIN"

# ---------------------------------------------------------------------------
# 2. Two tagged fake upstreams (A=4001, B=4002) + the real splitter.
# ---------------------------------------------------------------------------
: > "$LOG"; : > "$SPLOG"; : > "$SMOOTHLOG"; : > "$NAIVELOG"
echo "== starting fake upstreams A($APORT) + B($BPORT) + splitter (hashrate_split) =="
python3 fake_upstream.py --port "$APORT" --tag A --log "$LOG" & UPA=$!
python3 fake_upstream.py --port "$BPORT" --tag B --log "$LOG" & UPB=$!
sleep 0.6
"$BIN" --listen "$LPORT" --poolA 127.0.0.1:$APORT --poolB 127.0.0.1:$BPORT \
  --mode hashrate_split --ratio 50 --target-shares 3 --min-slice 2 --max-slice 5 \
  > "$SPLOG" 2>&1 & SP=$!
sleep 0.6

# ---------------------------------------------------------------------------
# 3. Scenario 1: smooth miner (one connection mines both pools via swaps).
# ---------------------------------------------------------------------------
echo "== [smooth] running fake_miner_split (${RUN_S}s) =="
python3 fake_miner_split.py 127.0.0.1 "$LPORT" "$RUN_S" wallet.smooth > "$SMOOTHLOG" 2>&1
SMOOTH_RC=$?
sleep 0.6

# marker so scenario-2 share counting is isolated from scenario-1
SMOOTH_A=$(grep -c "^A share" "$LOG")
SMOOTH_B=$(grep -c "^B share" "$LOG")
SMOOTH_A_BAD=$(grep "^A share" "$LOG" | grep -vc "job=A-")
SMOOTH_B_BAD=$(grep "^B share" "$LOG" | grep -vc "job=B-")
SWAPS=$(grep -c "set_extranonce" "$SMOOTHLOG")

# ---------------------------------------------------------------------------
# 4. Scenario 2: naive miner (reconnect-sliced by the splitter, 0 swaps).
# ---------------------------------------------------------------------------
echo "== [naive] running fake_miner_naive (${RUN_S}s, reconnects on drop) =="
python3 fake_miner_naive.py 127.0.0.1 "$LPORT" "$RUN_S" wallet.naive > "$NAIVELOG" 2>&1
NAIVE_RC=$?
sleep 0.6

# shares that landed AFTER the smooth run (i.e. attributable to the naive miner)
TOTAL_A=$(grep -c "^A share" "$LOG")
TOTAL_B=$(grep -c "^B share" "$LOG")
NAIVE_A=$((TOTAL_A - SMOOTH_A))
NAIVE_B=$((TOTAL_B - SMOOTH_B))
NAIVE_SE_LINES=$(grep -c "^set_extranonce " "$NAIVELOG")
NAIVE_SUM=$(grep -o "set_extranonce_seen=[0-9]* conns=[0-9]* submits=[0-9]* acks=[0-9]*" "$NAIVELOG" | tail -1)
NAIVE_SE_SEEN=$(echo "$NAIVE_SUM" | sed -n 's/.*set_extranonce_seen=\([0-9]*\).*/\1/p'); NAIVE_SE_SEEN=${NAIVE_SE_SEEN:-0}
NAIVE_CONNS=$(echo "$NAIVE_SUM"   | sed -n 's/.*conns=\([0-9]*\).*/\1/p'); NAIVE_CONNS=${NAIVE_CONNS:-0}
NAIVE_SUBMITS=$(echo "$NAIVE_SUM" | sed -n 's/.*submits=\([0-9]*\).*/\1/p'); NAIVE_SUBMITS=${NAIVE_SUBMITS:-0}

# startup line + rotation evidence from the splitter's own stderr
STARTUP=$(grep -c "HASHRATE-SPLIT" "$SPLOG")
HS_ROUTES=$(grep -c "hsplit route" "$SPLOG")

# ---------------------------------------------------------------------------
# 5. Teardown.
# ---------------------------------------------------------------------------
kill "$SP" "$UPA" "$UPB" 2>/dev/null
wait 2>/dev/null

echo "==========================================="
echo "[smooth] miner exit code   : $SMOOTH_RC"
echo "[smooth] A shares          : $SMOOTH_A"
echo "[smooth] B shares          : $SMOOTH_B"
echo "[smooth] A cross-routed    : $SMOOTH_A_BAD"
echo "[smooth] B cross-routed    : $SMOOTH_B_BAD"
echo "[smooth] set_extranonce    : $SWAPS"
echo "-------------------------------------------"
echo "[naive]  miner exit code   : $NAIVE_RC"
echo "[naive]  A shares (delta)  : $NAIVE_A"
echo "[naive]  B shares (delta)  : $NAIVE_B"
echo "[naive]  set_extranonce    : $NAIVE_SE_LINES (self-report $NAIVE_SE_SEEN)"
echo "[naive]  connections       : $NAIVE_CONNS"
echo "[naive]  submits           : $NAIVE_SUBMITS"
echo "-------------------------------------------"
echo "splitter HASHRATE-SPLIT ln : $STARTUP"
echo "splitter hsplit routes     : $HS_ROUTES"
echo "==========================================="

fail=0
# --- scenario 1: smooth miner mines BOTH pools, clean routing ---
[ "$SMOOTH_RC" -eq 0 ]    || { echo "FAIL: [smooth] miner exit $SMOOTH_RC != 0"; fail=1; }
[ "$SMOOTH_A" -ge 2 ]     || { echo "FAIL: [smooth] pool A got < 2 shares (split broken)"; fail=1; }
[ "$SMOOTH_B" -ge 2 ]     || { echo "FAIL: [smooth] pool B got < 2 shares (split broken)"; fail=1; }
[ "$SMOOTH_A_BAD" -eq 0 ] || { echo "FAIL: [smooth] $SMOOTH_A_BAD A-shares cross-routed"; fail=1; }
[ "$SMOOTH_B_BAD" -eq 0 ] || { echo "FAIL: [smooth] $SMOOTH_B_BAD B-shares cross-routed"; fail=1; }
[ "$SWAPS" -ge 1 ]        || { echo "FAIL: [smooth] miner never saw a set_extranonce swap"; fail=1; }
[ "$STARTUP" -ge 1 ]      || { echo "FAIL: splitter never logged the HASHRATE-SPLIT startup line"; fail=1; }
# --- scenario 2: naive miner reconnect-sliced across BOTH pools, 0 swaps ---
[ "$NAIVE_RC" -eq 0 ]     || { echo "FAIL: [naive] miner exit $NAIVE_RC != 0"; fail=1; }
[ "$NAIVE_SE_LINES" -eq 0 ] || { echo "FAIL: [naive] splitter sent $NAIVE_SE_LINES set_extranonce to a naive miner"; fail=1; }
[ "$NAIVE_SE_SEEN" -eq 0 ]  || { echo "FAIL: [naive] miner self-reported $NAIVE_SE_SEEN set_extranonce"; fail=1; }
[ "$NAIVE_A" -ge 1 ]      || { echo "FAIL: [naive] pool A got no shares (start_pool rotation broken)"; fail=1; }
[ "$NAIVE_B" -ge 1 ]      || { echo "FAIL: [naive] pool B got no shares (start_pool rotation broken)"; fail=1; }
[ "$NAIVE_CONNS" -ge 2 ]  || { echo "FAIL: [naive] miner never reconnected (conns=$NAIVE_CONNS)"; fail=1; }
[ "$NAIVE_SUBMITS" -ge 2 ] || { echo "FAIL: [naive] miner sent < 2 submits (test degenerate)"; fail=1; }

if [ "$fail" -eq 0 ]; then
  echo "HSPLIT-SPLITTER PASS: real splitter split a smooth miner across both pools"
  echo "  (clean routing + swap) and reconnect-sliced a naive miner across both"
  echo "  pools via the ratio-weighted start_pool rotation (0 set_extranonce)."
  exit 0
else
  echo "HSPLIT-SPLITTER FAILED"; exit 1
fi
