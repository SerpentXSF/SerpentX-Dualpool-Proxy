#!/usr/bin/env bash
# HSPLIT-M5: capability detection + reconnect-slice fallback.
#
# A "naive" miner (fake_miner_naive.py) never sends mining.extranonce.subscribe
# and ignores mining.set_extranonce — modelling ASIC firmware that does not
# support the smooth-swap extension. The mux must therefore NOT try the M4
# smooth set_extranonce swap on it; instead, at each slice deadline it logs the
# reconnect-slice fallback, shutdown()s the miner, and returns. The miner
# reconnects and the probe (standing in for the M6.2 splitter) binds it to the
# NEXT pool via an alternating start_pool (0,1,0,1,...). Over a ~20 s run this
# makes BOTH pools receive shares across the reconnects, with ZERO set_extranonce
# ever sent to the miner.
#
# Asserts:
#   (a) the mux NEVER sent the naive miner a mining.set_extranonce (0 seen)
#   (b) the reconnect-slice fallback log line appeared
#   (c) both pools received shares across the reconnects (alternation works)
#   (d) the miner actually reconnected (conns >= 2) and submitted shares
#   (e) the probe/loop exited cleanly (rc 0)
#
# Run inside the dualpool-dev image (has python3+gcc+make+jansson):
#   docker run --rm -v "$PWD":/repo -w /repo dualpool-dev \
#       bash test/integration/run_hashrate_split_m5.sh
set -u
cd "$(dirname "$0")"
ROOT=../..
PROBE=/tmp/splitmux_probe_m5
LOG=/tmp/hsplit_m5.log
PROBELOG=/tmp/hsplit_m5_probe.log
MINERLOG=/tmp/hsplit_m5_miner.log
LPORT=3337
APORT=4031
BPORT=4032
RUN_S=20

# ---------------------------------------------------------------------------
# 1. Compile the probe (0-warning gate).
# ---------------------------------------------------------------------------
echo "== compiling splitmux_probe (M5) =="
PROBE_OUT=$(gcc -std=c11 -Wall -Wextra -I"$ROOT/src" \
  splitmux_probe.c "$ROOT/src/splitmux.c" "$ROOT/src/stratum_msg.c" \
  "$ROOT/src/split_sched.c" -ljansson -lpthread -lm -o "$PROBE" 2>&1)
PROBE_RC=$?
echo "$PROBE_OUT"
if [ "$PROBE_RC" -ne 0 ]; then echo "HSPLIT-M5 FAILED: probe compile error"; exit 1; fi
if echo "$PROBE_OUT" | grep -qi "warning:"; then
  echo "HSPLIT-M5 FAILED: probe compile emitted warnings"; exit 1; fi

# ---------------------------------------------------------------------------
# 2. Two fake upstreams (A/B) + probe in reconnect-loop w/ alternating start.
# ---------------------------------------------------------------------------
: > "$LOG"; : > "$PROBELOG"; : > "$MINERLOG"
echo "== starting fake upstreams A($APORT) + B($BPORT) + reconnect-loop probe =="
python3 fake_upstream.py --interval 1.2 --port "$APORT" --tag A --log "$LOG" & UPA=$!
python3 fake_upstream.py --interval 1.2 --port "$BPORT" --tag B --log "$LOG" & UPB=$!
sleep 0.6
# Short slices so each connection reaches its deadline (and the fallback drop)
# quickly; --alt-start rotates 0,1,0,1,... across reconnects.
"$PROBE" --listen "$LPORT" --upstream "127.0.0.1:$APORT" \
  --upstream2 "127.0.0.1:$BPORT" \
  --ratio 50 --target 3 --min 2 --max 4 \
  --reconnect-loop 40 --alt-start > "$PROBELOG" 2>&1 & PB=$!
sleep 0.4

echo "== running naive miner (${RUN_S}s, reconnects on drop) =="
python3 fake_miner_naive.py 127.0.0.1 "$LPORT" "$RUN_S" wallet.w1 > "$MINERLOG" 2>&1
MINER_RC=$?

# After the miner stops reconnecting, the probe's next accept times out (5 s) and
# the loop exits on its own. Wait longer than that before declaring a hang.
sleep 7
if kill -0 "$PB" 2>/dev/null; then
  echo "NOTE: probe still running after miner left (reconnect loop did not exit)"
  kill "$PB" 2>/dev/null; wait "$PB" 2>/dev/null; PROBE_OK=0
else
  wait "$PB"; PRC=$?
  if [ "$PRC" -eq 0 ]; then PROBE_OK=1
  else echo "NOTE: probe exited abnormally (rc=$PRC)"; PROBE_OK=0; fi
fi

kill "$UPA" "$UPB" 2>/dev/null
wait 2>/dev/null

# ---------------------------------------------------------------------------
# 3. Assertions.
# ---------------------------------------------------------------------------
A_SHARES=$(grep -c "^A share" "$LOG")
B_SHARES=$(grep -c "^B share" "$LOG")
# set_extranonce actually delivered to the miner (per-line "set_extranonce <en>").
SE_LINES=$(grep -c "^set_extranonce " "$MINERLOG")
# fallback log line from the mux (probe stderr).
FALLBACK=$(grep -c "fallback reconnect-slice" "$PROBELOG")
# miner self-report summary.
SUM=$(grep -o "set_extranonce_seen=[0-9]* conns=[0-9]* submits=[0-9]* acks=[0-9]*" "$MINERLOG" | tail -1)
SE_SEEN=$(echo "$SUM" | sed -n 's/.*set_extranonce_seen=\([0-9]*\).*/\1/p'); SE_SEEN=${SE_SEEN:-0}
CONNS=$(echo "$SUM"  | sed -n 's/.*conns=\([0-9]*\).*/\1/p'); CONNS=${CONNS:-0}
SUBMITS=$(echo "$SUM" | sed -n 's/.*submits=\([0-9]*\).*/\1/p'); SUBMITS=${SUBMITS:-0}

echo "-------------------------------------------"
echo "fake_miner exit code      : $MINER_RC"
echo "A share lines             : $A_SHARES"
echo "B share lines             : $B_SHARES"
echo "set_extranonce lines(miner): $SE_LINES"
echo "set_extranonce_seen (sum) : $SE_SEEN"
echo "fallback log lines        : $FALLBACK"
echo "miner connections         : $CONNS"
echo "miner submits             : $SUBMITS"
echo "-------------------------------------------"

fail=0
[ "$MINER_RC" -eq 0 ] || { echo "FAIL: fake_miner exit $MINER_RC != 0"; fail=1; }
[ "$SE_LINES" -eq 0 ] || { echo "FAIL: mux sent $SE_LINES set_extranonce to a naive miner"; fail=1; }
[ "$SE_SEEN"  -eq 0 ] || { echo "FAIL: miner saw $SE_SEEN set_extranonce (smooth swap applied)"; fail=1; }
[ "$FALLBACK" -ge 1 ] || { echo "FAIL: reconnect-slice fallback never logged"; fail=1; }
[ "$A_SHARES" -ge 1 ] || { echo "FAIL: pool A got no shares (alternation broken)"; fail=1; }
[ "$B_SHARES" -ge 1 ] || { echo "FAIL: pool B got no shares (alternation broken)"; fail=1; }
[ "$CONNS"    -ge 2 ] || { echo "FAIL: miner never reconnected (conns=$CONNS)"; fail=1; }
[ "$SUBMITS"  -ge 2 ] || { echo "FAIL: miner sent < 2 submits (test degenerate)"; fail=1; }
[ "$PROBE_OK" -eq 1 ] || { echo "FAIL: probe did not exit cleanly (crash or hang)"; fail=1; }

if [ "$fail" -eq 0 ]; then
  echo "HSPLIT-M5 PASS: naive miner reconnect-sliced, 0 set_extranonce, both pools mined."
  exit 0
else
  echo "HSPLIT-M5 FAILED"; exit 1
fi
