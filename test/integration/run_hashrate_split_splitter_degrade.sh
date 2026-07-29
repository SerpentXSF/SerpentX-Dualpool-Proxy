#!/usr/bin/env bash
# HSPLIT-SPLITTER-DEGRADE: availability hardening for the hashrate_split splitter.
#
# The capstone (run_hashrate_split_splitter.sh) proves the happy path with BOTH
# pools healthy. This test proves the M6.2 hardening: a miner must keep mining a
# HEALTHY pool when the other pool is broken, instead of being stranded or
# reconnect-looping against a dead upstream.
#
# Scenario 1 — one pool UNREACHABLE (D3 + D1a, splitter readiness gate):
#   Only pool A is up; nothing listens on pool B's port. The splitter's
#   hashrate_split accept branch must NOT try to run a dual split — the readiness
#   gate (health / warmup / crash-loop, plus the connect-failure fallback) routes
#   the miner to a single-pool passthrough on A. The miner mines A normally.
#     asserts: A shares >= 2, B shares == 0, single-pool route logged, miner 0.
#
# Scenario 2 — one pool WORKLESS (D1b, splitmux handshake degrade):
#   Both pools accept TCP (so the readiness gate lets the split proceed), but B
#   is --workless: it answers NOTHING, never returning a subscribe result. The
#   mux's dual handshake must time B out and DEGRADE to a single-pool relay on A
#   rather than dropping the miner. The miner mines A.
#     asserts: A shares >= 2, B shares == 0, "single-pool degrade" logged, miner 0.
#
# Scenario 3 — D4 knob clamp (--max-slice 0 must not churn):
#   Both pools healthy, but the splitter is started with --max-slice 0 --min-slice
#   0. Un-clamped, a 0-length max slice makes every deadline already-past ->
#   perpetual swap churn every poll tick. Clamped to [1,3600] with min<=max, the
#   miner mines normally across both pools with a bounded number of swaps.
#     asserts: A shares >= 1, B shares >= 1, swaps bounded (< 200), miner 0.
#
# Run inside the dualpool-dev image (python3+gcc+make+jansson):
#   docker run --rm -v "$PWD":/repo -w /repo dualpool-dev \
#       bash test/integration/run_hashrate_split_splitter_degrade.sh
set -u
cd "$(dirname "$0")"
ROOT=../..
BIN=/tmp/dualpool_splitter_hsplit_degrade
LPORT=3336
APORT=4021
BPORT=4022
RUN_S=12

fail=0

# ---------------------------------------------------------------------------
# Build the REAL splitter (0-warning gate).
# ---------------------------------------------------------------------------
echo "== compiling dualpool-splitter =="
BUILD_OUT=$(make -C "$ROOT" dualpool-splitter 2>&1)
BUILD_RC=$?
echo "$BUILD_OUT"
if [ "$BUILD_RC" -ne 0 ]; then echo "HSPLIT-DEGRADE FAILED: compile error"; exit 1; fi
if echo "$BUILD_OUT" | grep -qi "warning:"; then
  echo "HSPLIT-DEGRADE FAILED: build emitted warnings"; exit 1; fi
cp "$ROOT/dualpool-splitter" "$BIN"

# ===========================================================================
# Scenario 1: pool B UNREACHABLE (nothing listening on BPORT).
# ===========================================================================
echo
echo "===== [S1] one pool UNREACHABLE (readiness gate / D3+D1a) ====="
S1LOG=/tmp/hsplit_degrade_s1.log
S1SP=/tmp/hsplit_degrade_s1_sp.log
S1MINER=/tmp/hsplit_degrade_s1_miner.log
: > "$S1LOG"; : > "$S1SP"; : > "$S1MINER"

python3 fake_upstream.py --port "$APORT" --tag A --log "$S1LOG" & UPA=$!
sleep 0.6
# NOTE: no upstream on BPORT -> connect() is refused.
"$BIN" --listen "$LPORT" --poolA 127.0.0.1:$APORT --poolB 127.0.0.1:$BPORT \
  --mode hashrate_split --ratio 50 --target-shares 3 --min-slice 2 --max-slice 5 \
  > "$S1SP" 2>&1 & SP=$!
sleep 0.6

echo "== [S1] running smooth miner (${RUN_S}s) =="
python3 fake_miner_split.py 127.0.0.1 "$LPORT" "$RUN_S" wallet.s1 > "$S1MINER" 2>&1
S1_RC=$?
sleep 0.4

S1_A=$(grep -c "^A share" "$S1LOG")
S1_B=$(grep -c "^B share" "$S1LOG")
S1_SINGLE=$(grep -c "single-pool" "$S1SP")

kill "$SP" "$UPA" 2>/dev/null; wait 2>/dev/null

echo "[S1] miner exit code   : $S1_RC"
echo "[S1] A shares          : $S1_A"
echo "[S1] B shares          : $S1_B"
echo "[S1] single-pool routes: $S1_SINGLE"
[ "$S1_RC" -eq 0 ]     || { echo "FAIL: [S1] miner exit $S1_RC != 0 (stranded?)"; fail=1; }
[ "$S1_A" -ge 2 ]      || { echo "FAIL: [S1] pool A got < 2 shares (miner not mining survivor)"; fail=1; }
[ "$S1_B" -eq 0 ]      || { echo "FAIL: [S1] pool B (dead) somehow got shares"; fail=1; }
[ "$S1_SINGLE" -ge 1 ] || { echo "FAIL: [S1] splitter never logged a single-pool route"; fail=1; }

# ===========================================================================
# Scenario 2: pool B WORKLESS (accepts TCP, answers nothing).
# ===========================================================================
echo
echo "===== [S2] one pool WORKLESS (splitmux handshake degrade / D1b) ====="
S2LOG=/tmp/hsplit_degrade_s2.log
S2SP=/tmp/hsplit_degrade_s2_sp.log
S2MINER=/tmp/hsplit_degrade_s2_miner.log
: > "$S2LOG"; : > "$S2SP"; : > "$S2MINER"

python3 fake_upstream.py --port "$APORT" --tag A --log "$S2LOG" & UPA=$!
python3 fake_upstream.py --port "$BPORT" --tag B --log "$S2LOG" --workless & UPB=$!
sleep 0.6
"$BIN" --listen "$LPORT" --poolA 127.0.0.1:$APORT --poolB 127.0.0.1:$BPORT \
  --mode hashrate_split --ratio 50 --target-shares 3 --min-slice 2 --max-slice 5 \
  > "$S2SP" 2>&1 & SP=$!
sleep 0.6

echo "== [S2] running smooth miner (${RUN_S}s; ~5s handshake degrade first) =="
python3 fake_miner_split.py 127.0.0.1 "$LPORT" "$RUN_S" wallet.s2 > "$S2MINER" 2>&1
S2_RC=$?
sleep 0.4

S2_A=$(grep -c "^A share" "$S2LOG")
S2_B=$(grep -c "^B share" "$S2LOG")
S2_DEGRADE=$(grep -c "single-pool degrade" "$S2SP")

kill "$SP" "$UPA" "$UPB" 2>/dev/null; wait 2>/dev/null

echo "[S2] miner exit code   : $S2_RC"
echo "[S2] A shares          : $S2_A"
echo "[S2] B shares          : $S2_B"
echo "[S2] degrade log lines : $S2_DEGRADE"
[ "$S2_RC" -eq 0 ]      || { echo "FAIL: [S2] miner exit $S2_RC != 0 (dropped instead of degraded)"; fail=1; }
[ "$S2_A" -ge 2 ]       || { echo "FAIL: [S2] pool A got < 2 shares (degrade path broken)"; fail=1; }
[ "$S2_B" -eq 0 ]       || { echo "FAIL: [S2] workless pool B somehow got shares"; fail=1; }
[ "$S2_DEGRADE" -ge 1 ] || { echo "FAIL: [S2] mux never logged the single-pool degrade"; fail=1; }

# ===========================================================================
# Scenario 3: D4 clamp — --max-slice 0 must clamp, not churn.
# ===========================================================================
echo
echo "===== [S3] D4 knob clamp (--max-slice 0 must not churn) ====="
S3LOG=/tmp/hsplit_degrade_s3.log
S3SP=/tmp/hsplit_degrade_s3_sp.log
S3MINER=/tmp/hsplit_degrade_s3_miner.log
: > "$S3LOG"; : > "$S3SP"; : > "$S3MINER"

python3 fake_upstream.py --port "$APORT" --tag A --log "$S3LOG" & UPA=$!
python3 fake_upstream.py --port "$BPORT" --tag B --log "$S3LOG" & UPB=$!
sleep 0.6
# --max-slice 0 --min-slice 0: un-clamped this churns; clamped it is [1,1].
"$BIN" --listen "$LPORT" --poolA 127.0.0.1:$APORT --poolB 127.0.0.1:$BPORT \
  --mode hashrate_split --ratio 50 --target-shares 0 --min-slice 0 --max-slice 0 \
  > "$S3SP" 2>&1 & SP=$!
sleep 0.6

echo "== [S3] running smooth miner (${RUN_S}s) =="
python3 fake_miner_split.py 127.0.0.1 "$LPORT" "$RUN_S" wallet.s3 > "$S3MINER" 2>&1
S3_RC=$?
sleep 0.4

S3_A=$(grep -c "^A share" "$S3LOG")
S3_B=$(grep -c "^B share" "$S3LOG")
S3_SWAPS=$(grep -c "set_extranonce" "$S3MINER")

kill "$SP" "$UPA" "$UPB" 2>/dev/null; wait 2>/dev/null

echo "[S3] miner exit code   : $S3_RC"
echo "[S3] A shares          : $S3_A"
echo "[S3] B shares          : $S3_B"
echo "[S3] set_extranonce sw : $S3_SWAPS"
[ "$S3_RC" -eq 0 ]     || { echo "FAIL: [S3] miner exit $S3_RC != 0"; fail=1; }
[ "$S3_A" -ge 1 ]      || { echo "FAIL: [S3] pool A got no shares (clamp broke mining)"; fail=1; }
[ "$S3_B" -ge 1 ]      || { echo "FAIL: [S3] pool B got no shares (clamp broke mining)"; fail=1; }
# With a 1s clamped slice the mux swaps ~once/second; un-clamped it would churn
# every 200ms poll tick (tens of swaps/second). A generous bound catches churn.
[ "$S3_SWAPS" -lt 200 ] || { echo "FAIL: [S3] $S3_SWAPS swaps -> perpetual churn (clamp not applied)"; fail=1; }

# ---------------------------------------------------------------------------
echo
echo "==========================================="
if [ "$fail" -eq 0 ]; then
  echo "HSPLIT-DEGRADE PASS: unreachable pool -> single-pool passthrough on the"
  echo "  survivor (D3/D1a); workless pool -> splitmux handshake degrade (D1b);"
  echo "  --max-slice 0 clamped, no swap churn (D4). Miner kept mining throughout."
  exit 0
else
  echo "HSPLIT-DEGRADE FAILED"; exit 1
fi
