#!/usr/bin/env bash
# HSPLIT-ASSUMEEXT: the EXPERIMENTAL assume_extranonce opt-in, proved RED/GREEN.
#
# Motivation. The user's fleet is ESP-Miner-derived firmware (BitAxe / Hammer /
# NerdAxe / NerdQAxe): it HONOURS mining.set_extranonce but never sends
# mining.extranonce.subscribe. The mux's M5 capability detection can only see the
# advertisement, so every such miner falls to the reconnect-slice fallback — a
# full disconnect+reconnect per slice, which wasted hashrate on a live run. The
# opt-in (`assume_extranonce` in config.json, --assume-extranonce on the CLI)
# tells the mux to trust the fleet and take the SMOOTH in-place swap instead.
#
# This runs the REAL splitter twice against two tagged fake upstreams, with the
# new fake_miner_extquiet.py (honours set_extranonce, never advertises it):
#
#   RED   (flag OFF, i.e. today's default behaviour)
#         asserts: `fallback reconnect-slice` IS logged, the miner reconnects
#                  >= 1 time, and it receives ZERO set_extranonce.
#
#   GREEN (flag ON, --assume-extranonce)
#         asserts: NO `fallback reconnect-slice` in the log, ZERO miner
#                  reconnects, >= 1 set_extranonce honoured, BOTH pools receive
#                  shares, ZERO cross-routed shares (every share reaching pool A
#                  carries A's own en=aaaa0001 and vice versa — proof the miner
#                  really followed the swap instead of mining the wrong pool's
#                  work), and /api/status shows per-pool accepted > 0 with
#                  rejected == 0.
#
# The startup log line must also report the flag state in both phases
# (assume_extranonce=off / =on), so production logs show what is in effect.
#
# Run inside the dualpool-dev image (python3+gcc+make+jansson+curl):
#   docker run --rm -v "$PWD":/repo -w /repo dualpool-dev \
#       bash test/integration/run_hashrate_split_assume_ext.sh
#
# Part of Dual-Pool Proxy (Dual-Pool Stratum Proxy). GPLv3.
# Copyright (C) 2025-2026 The SerpentX authors.
set -u
cd "$(dirname "$0")"
ROOT=../..
BIN=/tmp/dualpool_splitter_assumeext

# RED and GREEN get their own ports + logs so nothing has to be disentangled.
R_LPORT=3341; R_APORT=4021; R_BPORT=4022
G_LPORT=3342; G_APORT=4023; G_BPORT=4024; G_WEB=8093

RLOG=/tmp/hsplit_assumeext_red_pools.log      # tagged upstream events (RED)
GLOG=/tmp/hsplit_assumeext_green_pools.log    # tagged upstream events (GREEN)
RSPLOG=/tmp/hsplit_assumeext_red_sp.log       # splitter stderr (RED)
GSPLOG=/tmp/hsplit_assumeext_green_sp.log     # splitter stderr (GREEN)
RMLOG=/tmp/hsplit_assumeext_red_miner.log     # miner stdout (RED)
GMLOG=/tmp/hsplit_assumeext_green_miner.log   # miner stdout (GREEN)
STATUSJ=/tmp/hsplit_assumeext_status.json     # /api/status snapshot (GREEN)

RUN_S=14            # per phase; ~30 s of mining across the two phases

# ---------------------------------------------------------------------------
# 1. Build the REAL splitter (0-warning gate).
# ---------------------------------------------------------------------------
echo "== compiling dualpool-splitter =="
BUILD_OUT=$(make -C "$ROOT" dualpool-splitter 2>&1)
BUILD_RC=$?
echo "$BUILD_OUT"
if [ "$BUILD_RC" -ne 0 ]; then echo "HSPLIT-ASSUMEEXT FAILED: compile error"; exit 1; fi
if echo "$BUILD_OUT" | grep -qi "warning:"; then
  echo "HSPLIT-ASSUMEEXT FAILED: build emitted warnings"; exit 1; fi
cp "$ROOT/dualpool-splitter" "$BIN"

# The probe shares splitmux.c and gained the same flag — keep it in the gate so
# the new trailing splitmux_run() parameter can't rot the standalone harness.
echo "== compiling splitmux_probe (assume-ext) =="
PROBE_OUT=$(gcc -std=c11 -Wall -Wextra -I"$ROOT/src" \
  splitmux_probe.c "$ROOT/src/splitmux.c" "$ROOT/src/stratum_msg.c" \
  "$ROOT/src/split_sched.c" -ljansson -lpthread -lm \
  -o /tmp/splitmux_probe_assumeext 2>&1)
PROBE_RC=$?
echo "$PROBE_OUT"
if [ "$PROBE_RC" -ne 0 ]; then echo "HSPLIT-ASSUMEEXT FAILED: probe compile error"; exit 1; fi
if echo "$PROBE_OUT" | grep -qi "warning:"; then
  echo "HSPLIT-ASSUMEEXT FAILED: probe compile emitted warnings"; exit 1; fi

# ---------------------------------------------------------------------------
# 2. RED phase — flag OFF: the ext-quiet miner must be reconnect-sliced.
# ---------------------------------------------------------------------------
: > "$RLOG"; : > "$RSPLOG"; : > "$RMLOG"
echo "== [RED] upstreams A($R_APORT)+B($R_BPORT) + splitter (no --assume-extranonce) =="
python3 fake_upstream.py --interval 1.2 --port "$R_APORT" --tag A --log "$RLOG" & R_UPA=$!
python3 fake_upstream.py --interval 1.2 --port "$R_BPORT" --tag B --log "$RLOG" & R_UPB=$!
sleep 0.6
"$BIN" --listen "$R_LPORT" --poolA 127.0.0.1:$R_APORT --poolB 127.0.0.1:$R_BPORT \
  --mode hashrate_split --ratio 50 --target-shares 3 --min-slice 2 --max-slice 5 \
  > "$RSPLOG" 2>&1 & R_SP=$!
sleep 0.6

echo "== [RED] running fake_miner_extquiet (${RUN_S}s) =="
python3 fake_miner_extquiet.py 127.0.0.1 "$R_LPORT" "$RUN_S" wallet.red > "$RMLOG" 2>&1
RED_RC=$?
sleep 0.4
kill "$R_SP" "$R_UPA" "$R_UPB" 2>/dev/null
sleep 0.3

RED_SLICE=$(grep -c "fallback reconnect-slice" "$RSPLOG")
RED_SE=$(grep -c "^set_extranonce " "$RMLOG")
RED_SUM=$(grep "^extquiet " "$RMLOG" | tail -1)
red_field() { echo "$RED_SUM" | sed -n "s/.*$1=\([0-9]*\).*/\1/p"; }
RED_SE_SEEN=$(red_field set_extranonce_seen); RED_SE_SEEN=${RED_SE_SEEN:-0}
RED_RECON=$(red_field reconnects);           RED_RECON=${RED_RECON:-0}
RED_SUBMITS=$(red_field submits);            RED_SUBMITS=${RED_SUBMITS:-0}
RED_STARTUP_OFF=$(grep -c "HASHRATE-SPLIT.*assume_extranonce=off" "$RSPLOG")
RED_A=$(grep -c "^A share" "$RLOG")
RED_B=$(grep -c "^B share" "$RLOG")

# ---------------------------------------------------------------------------
# 3. GREEN phase — flag ON: smooth set_extranonce swap, no reconnects.
# ---------------------------------------------------------------------------
: > "$GLOG"; : > "$GSPLOG"; : > "$GMLOG"
echo "== [GREEN] upstreams A($G_APORT)+B($G_BPORT) + splitter (--assume-extranonce) =="
python3 fake_upstream.py --interval 1.2 --port "$G_APORT" --tag A --log "$GLOG" & G_UPA=$!
python3 fake_upstream.py --interval 1.2 --port "$G_BPORT" --tag B --log "$GLOG" & G_UPB=$!
sleep 0.6
"$BIN" --listen "$G_LPORT" --poolA 127.0.0.1:$G_APORT --poolB 127.0.0.1:$G_BPORT \
  --mode hashrate_split --ratio 50 --target-shares 3 --min-slice 2 --max-slice 5 \
  --web "$G_WEB" --assume-extranonce \
  > "$GSPLOG" 2>&1 & G_SP=$!
sleep 0.6

echo "== [GREEN] running fake_miner_extquiet (${RUN_S}s) =="
python3 fake_miner_extquiet.py 127.0.0.1 "$G_LPORT" "$RUN_S" wallet.green > "$GMLOG" 2>&1
GREEN_RC=$?
sleep 0.4

# /api/status while the splitter is still up (per-pool accounting is in-process).
curl -s "http://127.0.0.1:$G_WEB/api/status" > "$STATUSJ" 2>/dev/null
read -r ST_A_ACC ST_B_ACC ST_A_REJ ST_B_REJ ST_MODE <<<"$(python3 -c '
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print("-1 -1 -1 -1 none"); sys.exit(0)
p = d.get("pools") or []
def g(i, k):
    return int(p[i].get(k, -1)) if i < len(p) else -1
print(g(0, "accepted"), g(1, "accepted"),
      g(0, "rejected"), g(1, "rejected"), d.get("mode", "none"))
' "$STATUSJ")"

kill "$G_SP" "$G_UPA" "$G_UPB" 2>/dev/null
wait 2>/dev/null

GREEN_SLICE=$(grep -c "fallback reconnect-slice" "$GSPLOG")
GREEN_SE=$(grep -c "^set_extranonce " "$GMLOG")
GREEN_SUM=$(grep "^extquiet " "$GMLOG" | tail -1)
green_field() { echo "$GREEN_SUM" | sed -n "s/.*$1=\([0-9]*\).*/\1/p"; }
GREEN_SE_SEEN=$(green_field set_extranonce_seen); GREEN_SE_SEEN=${GREEN_SE_SEEN:-0}
GREEN_RECON=$(green_field reconnects);            GREEN_RECON=${GREEN_RECON:-0}
GREEN_CONNS=$(green_field conns);                 GREEN_CONNS=${GREEN_CONNS:-0}
GREEN_SUBMITS=$(green_field submits);             GREEN_SUBMITS=${GREEN_SUBMITS:-0}
GREEN_ACCEPTED=$(green_field accepted);           GREEN_ACCEPTED=${GREEN_ACCEPTED:-0}
GREEN_REJECTED=$(green_field rejected);           GREEN_REJECTED=${GREEN_REJECTED:-0}
GREEN_STARTUP_ON=$(grep -c "HASHRATE-SPLIT.*assume_extranonce=on" "$GSPLOG")
GREEN_A=$(grep -c "^A share" "$GLOG")
GREEN_B=$(grep -c "^B share" "$GLOG")
# Cross-routing: a share logged by pool A must carry A's OWN extranonce1
# (aaaa0001); B's must carry bbbb0001. Anything else means the miner mined the
# wrong pool's work — the failure mode this opt-in could plausibly introduce.
GREEN_A_CROSS=$(grep "^A share" "$GLOG" | grep -vc "en=aaaa0001")
GREEN_B_CROSS=$(grep "^B share" "$GLOG" | grep -vc "en=bbbb0001")

echo "==========================================="
echo "[RED]   miner exit code        : $RED_RC"
echo "[RED]   fallback reconnect-slice: $RED_SLICE   (expect >= 1)"
echo "[RED]   set_extranonce lines   : $RED_SE (self-report $RED_SE_SEEN)  (expect 0)"
echo "[RED]   miner reconnects       : $RED_RECON  (expect >= 1)"
echo "[RED]   submits                : $RED_SUBMITS"
echo "[RED]   A / B shares           : $RED_A / $RED_B"
echo "[RED]   startup assume_ext=off : $RED_STARTUP_OFF"
echo "-------------------------------------------"
echo "[GREEN] miner exit code        : $GREEN_RC"
echo "[GREEN] fallback reconnect-slice: $GREEN_SLICE   (expect 0)"
echo "[GREEN] set_extranonce lines   : $GREEN_SE (self-report $GREEN_SE_SEEN)  (expect >= 1)"
echo "[GREEN] miner conns/reconnects : $GREEN_CONNS / $GREEN_RECON  (expect 1 / 0)"
echo "[GREEN] submits                : $GREEN_SUBMITS"
echo "[GREEN] miner acc / rej        : $GREEN_ACCEPTED / $GREEN_REJECTED"
echo "[GREEN] A / B shares           : $GREEN_A / $GREEN_B"
echo "[GREEN] A / B cross-routed     : $GREEN_A_CROSS / $GREEN_B_CROSS  (expect 0 / 0)"
echo "[GREEN] startup assume_ext=on  : $GREEN_STARTUP_ON"
echo "[GREEN] /api/status mode       : $ST_MODE"
echo "[GREEN] /api/status accepted   : A=$ST_A_ACC B=$ST_B_ACC  (expect > 0 both)"
echo "[GREEN] /api/status rejected   : A=$ST_A_REJ B=$ST_B_REJ  (expect 0 both)"
echo "==========================================="

fail=0
# --- RED: today's default behaviour, unchanged ---
[ "$RED_RC" -eq 0 ]          || { echo "FAIL: [RED] miner exit $RED_RC != 0"; fail=1; }
[ "$RED_STARTUP_OFF" -ge 1 ] || { echo "FAIL: [RED] startup line missing assume_extranonce=off"; fail=1; }
[ "$RED_SLICE" -ge 1 ]       || { echo "FAIL: [RED] no 'fallback reconnect-slice' — the ext-quiet miner was NOT reconnect-sliced"; fail=1; }
[ "$RED_SE" -eq 0 ]          || { echo "FAIL: [RED] mux sent $RED_SE set_extranonce with the flag OFF"; fail=1; }
[ "$RED_SE_SEEN" -eq 0 ]     || { echo "FAIL: [RED] miner self-reported $RED_SE_SEEN set_extranonce with the flag OFF"; fail=1; }
[ "$RED_RECON" -ge 1 ]       || { echo "FAIL: [RED] miner never reconnected (reconnects=$RED_RECON)"; fail=1; }
[ "$RED_SUBMITS" -ge 2 ]     || { echo "FAIL: [RED] miner sent < 2 submits (test degenerate)"; fail=1; }

# --- GREEN: the opt-in unlocks the smooth swap ---
[ "$GREEN_RC" -eq 0 ]          || { echo "FAIL: [GREEN] miner exit $GREEN_RC != 0"; fail=1; }
[ "$GREEN_STARTUP_ON" -ge 1 ]  || { echo "FAIL: [GREEN] startup line missing assume_extranonce=on"; fail=1; }
[ "$GREEN_SLICE" -eq 0 ]       || { echo "FAIL: [GREEN] $GREEN_SLICE reconnect-slice fallbacks with the flag ON"; fail=1; }
[ "$GREEN_SE" -ge 1 ]          || { echo "FAIL: [GREEN] miner never received a set_extranonce swap"; fail=1; }
[ "$GREEN_SE_SEEN" -ge 1 ]     || { echo "FAIL: [GREEN] miner self-reported 0 set_extranonce honoured"; fail=1; }
[ "$GREEN_RECON" -eq 0 ]       || { echo "FAIL: [GREEN] miner reconnected $GREEN_RECON time(s) — swap was not smooth"; fail=1; }
[ "$GREEN_CONNS" -eq 1 ]       || { echo "FAIL: [GREEN] miner made $GREEN_CONNS connections (expected exactly 1)"; fail=1; }
[ "$GREEN_SUBMITS" -ge 2 ]     || { echo "FAIL: [GREEN] miner sent < 2 submits (test degenerate)"; fail=1; }
[ "$GREEN_A" -ge 1 ]           || { echo "FAIL: [GREEN] pool A received no shares"; fail=1; }
[ "$GREEN_B" -ge 1 ]           || { echo "FAIL: [GREEN] pool B received no shares"; fail=1; }
[ "$GREEN_A_CROSS" -eq 0 ]     || { echo "FAIL: [GREEN] $GREEN_A_CROSS shares cross-routed to A (wrong extranonce1)"; fail=1; }
[ "$GREEN_B_CROSS" -eq 0 ]     || { echo "FAIL: [GREEN] $GREEN_B_CROSS shares cross-routed to B (wrong extranonce1)"; fail=1; }
[ "$GREEN_REJECTED" -eq 0 ]    || { echo "FAIL: [GREEN] miner saw $GREEN_REJECTED rejected share(s)"; fail=1; }
# --- GREEN: the splitter's own per-pool accounting agrees ---
[ "$ST_MODE" = "hashrate_split" ] || { echo "FAIL: [GREEN] /api/status mode=$ST_MODE"; fail=1; }
[ "$ST_A_ACC" -gt 0 ]          || { echo "FAIL: [GREEN] /api/status pool A accepted=$ST_A_ACC (expected > 0)"; fail=1; }
[ "$ST_B_ACC" -gt 0 ]          || { echo "FAIL: [GREEN] /api/status pool B accepted=$ST_B_ACC (expected > 0)"; fail=1; }
[ "$ST_A_REJ" -eq 0 ]          || { echo "FAIL: [GREEN] /api/status pool A rejected=$ST_A_REJ (expected 0)"; fail=1; }
[ "$ST_B_REJ" -eq 0 ]          || { echo "FAIL: [GREEN] /api/status pool B rejected=$ST_B_REJ (expected 0)"; fail=1; }

if [ "$fail" -eq 0 ]; then
  echo "HSPLIT-ASSUMEEXT PASS: with the flag OFF an ext-quiet miner is"
  echo "  reconnect-sliced (0 set_extranonce); with --assume-extranonce the SAME"
  echo "  miner is swapped smoothly — 0 reconnects, $GREEN_SE_SEEN set_extranonce honoured,"
  echo "  both pools paid (A=$GREEN_A B=$GREEN_B), 0 cross-routed shares, 0 rejects."
  exit 0
else
  echo "HSPLIT-ASSUMEEXT FAILED"; exit 1
fi
