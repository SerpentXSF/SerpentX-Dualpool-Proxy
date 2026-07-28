#!/usr/bin/env bash
# X.2 robustness tests:
#  A) the dashboard reports CURRENTLY-connected miners (not the cumulative routing
#     count) — connected drops to 0 after miners leave, while routed stays > 0.
#  B) a ckproxy that keeps dying is detected as crash-looping, respawns with
#     exponential backoff, and its pool is marked down (donated away).
# Runs in serpentx-dev. GPLv3.
set -u
cd "$(dirname "$0")/../.."
make serpentx-splitter >/dev/null 2>&1 || { echo "FAIL: compile"; exit 1; }
cp serpentx-splitter /tmp/sx_rb
cd test/integration
fail=0

# ---------- A) connected vs cumulative ----------
echo "== A: current-connected miner count =="
python3 fake_upstream.py --port 4601 --tag A --log /dev/null >/dev/null 2>&1 & UPA=$!
python3 fake_upstream.py --port 4602 --tag B --log /dev/null >/dev/null 2>&1 & UPB=$!
sleep 0.5
/tmp/sx_rb --listen 3370 --web 8097 --poolA 127.0.0.1:4601 --poolB 127.0.0.1:4602 --ratio 70 >/tmp/sx_rb_a.log 2>&1 & SPA=$!
sleep 0.6
for i in $(seq 1 10); do python3 fake_miner.py 127.0.0.1 3370 1 "rig$i" 4 >/dev/null 2>&1 & done
sleep 2.5
S=$(curl -s http://127.0.0.1:8097/api/status)
CONN=$(echo "$S"    | python3 -c 'import sys,json;d=json.load(sys.stdin);print(sum(p["connected"] for p in d["pools"]))')
ROUTED=$(echo "$S"  | python3 -c 'import sys,json;d=json.load(sys.stdin);print(sum(p["routed"] for p in d["pools"]))')
MET=$(curl -s http://127.0.0.1:8097/metrics | grep -c "serpentx_miners_connected_pool{")
echo "   while connected: connected=$CONN routed=$ROUTED metrics_lines=$MET"
sleep 5    # miners disconnect
CONN2=$(curl -s http://127.0.0.1:8097/api/status | python3 -c 'import sys,json;d=json.load(sys.stdin);print(sum(p["connected"] for p in d["pools"]))')
echo "   after they leave: connected=$CONN2 (routed stays $ROUTED)"
kill $SPA $UPA $UPB 2>/dev/null

[ "$CONN" -ge 8 ]    || { echo "FAIL(A): connected=$CONN while 10 miners held"; fail=1; }
[ "$CONN2" -eq 0 ]   || { echo "FAIL(A): connected=$CONN2 after miners left (should be 0 — current, not cumulative)"; fail=1; }
[ "$ROUTED" -ge 8 ]  || { echo "FAIL(A): routed=$ROUTED (cumulative should be >=8)"; fail=1; }
[ "$MET" -eq 2 ]     || { echo "FAIL(A): metrics missing per-pool connected gauge"; fail=1; }

# ---------- B) crash-loop -> backoff + donate ----------
echo "== B: ckproxy crash-loop detection + backoff =="
RUN=/tmp/rbrun; rm -rf "$RUN"; mkdir -p "$RUN"
cat > "$RUN/config.json" <<'EOF'
{ "downstream": { "stratum_port": 3371, "web_port": 8098 }, "mode": "farm_split", "ratio_a": 50,
  "pools": [ {"url":"127.0.0.1:9901","user":"a","pass":"x","ckproxy_mode":"proxy"},
             {"url":"127.0.0.1:9902","user":"b","pass":"x","ckproxy_mode":"proxy"} ] }
EOF
# /bin/false exits immediately (like a ckproxy that can't log in) -> crash loop
SERPENTX_CKPOOL_BIN=/bin/false SERPENTX_RUNDIR="$RUN/run" /tmp/sx_rb --config "$RUN/config.json" >/tmp/sx_rb_b.log 2>&1 & SPB=$!
sleep 20
CL=$(grep -c "crash-looping" /tmp/sx_rb_b.log)
BK=$(grep -oE "respawn in [0-9]+s" /tmp/sx_rb_b.log | grep -oE "[0-9]+" | sort -n | tail -1)
kill $SPB 2>/dev/null
echo "   crash-loop log lines=$CL   max backoff seen=${BK:-0}s"
[ "$CL" -ge 1 ]      || { echo "FAIL(B): crash-loop not detected"; fail=1; }
[ "${BK:-0}" -ge 8 ] || { echo "FAIL(B): backoff did not grow (max=${BK:-0}s)"; fail=1; }

echo "-------------------------------------------"
if [ "$fail" -eq 0 ]; then
  echo "ROBUSTNESS PASS: current-connected counts + crash-loop backoff/donation verified."
  exit 0
else
  echo "ROBUSTNESS FAILED"; exit 1
fi
