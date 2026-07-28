#!/usr/bin/env bash
# M7 web dashboard test: splitter (CLI mode, direct to two fake pools) with the
# dashboard on :8080. Drives miners, then checks /api/status reflects the split
# and per-pool shares, static index.html serves, and POST /api/config hot-applies
# a ratio change. Run in the dualpool-dev image. GPLv3.
set -u
cd "$(dirname "$0")/../.."
BIN=/tmp/sx_web_splitter
PORT=3335
WEB=8090

echo "== compiling =="
make dualpool-splitter >/dev/null 2>&1 || { echo "FAIL: compile"; exit 1; }
cp dualpool-splitter "$BIN"

cd test/integration
python3 fake_upstream.py --port 4101 --tag A --log /dev/null >/dev/null 2>&1 & UPA=$!
python3 fake_upstream.py --port 4102 --tag B --log /dev/null >/dev/null 2>&1 & UPB=$!
sleep 0.5
"$BIN" --listen $PORT --web $WEB --webroot ../../web \
       --poolA 127.0.0.1:4101 --poolB 127.0.0.1:4102 --ratio 70 >/tmp/sx_web.log 2>&1 & SP=$!
sleep 1

cleanup() { kill $SP $UPA $UPB 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT

echo "== driving 20 miners x 2 shares (held so results are accounted) =="
# hold each connection ~4s so the submit results are relayed+sniffed and the
# miners are still connected when we snapshot status.
for i in $(seq 1 20); do python3 fake_miner.py 127.0.0.1 $PORT 2 "rig$i" 4 >/dev/null 2>&1 & done
sleep 2.5

echo "== GET /api/status =="
STATUS=$(curl -s "http://127.0.0.1:$WEB/api/status")
echo "$STATUS" | head -c 400; echo; echo "..."

echo "== GET / (static dashboard) =="
HTML_CODE=$(curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$WEB/")
TITLE_OK=$(curl -s "http://127.0.0.1:$WEB/" | grep -c "Dual-Pool Proxy")

echo "== GET /metrics (Prometheus) =="
METRICS_OK=$(curl -s "http://127.0.0.1:$WEB/metrics" | grep -c "dualpool_pool_up")

echo "== POST /api/config (ratio 70 -> 40) =="
POST_CODE=$(curl -s -o /dev/null -w "%{http_code}" -X POST \
  -H "Content-Type: application/json" -d '{"ratio_a":40,"mode":"farm_split"}' \
  "http://127.0.0.1:$WEB/api/config")
sleep 0.3
NEW_RATIO=$(curl -s "http://127.0.0.1:$WEB/api/status" | python3 -c 'import sys,json;print(json.load(sys.stdin)["ratio_a"])')

# parse status fields
read RATIO SA SB PA PB MINERS <<<"$(echo "$STATUS" | python3 -c '
import sys,json
d=json.load(sys.stdin)
print(d["ratio_a"],
      d["pools"][0]["accepted"], d["pools"][1]["accepted"],
      d["pools"][0]["state"], d["pools"][1]["state"],
      len(d["miners"]))')"

echo "-------------------------------------------"
echo "status: ratio_a=$RATIO poolA.accepted=$SA poolB.accepted=$SB state=$PA/$PB miners=$MINERS"
echo "static /: HTTP $HTML_CODE (Dual-Pool Proxy in page: $TITLE_OK)"
echo "POST /api/config: HTTP $POST_CODE ; ratio now $NEW_RATIO"
echo "-------------------------------------------"

fail=0
[ "$RATIO" = "70" ] || { echo "FAIL: status ratio_a != 70"; fail=1; }
[ "$PA" = "on" ] && [ "$PB" = "on" ] || { echo "FAIL: pools not both 'on'"; fail=1; }
# 20 miners x 2 shares = 40 accepted total, split ~70/30 across pools
TOTAL_ACC=$((SA + SB))
[ "$TOTAL_ACC" -eq 40 ] || { echo "FAIL: accepted shares $TOTAL_ACC != 40"; fail=1; }
[ "$SA" -gt "$SB" ] || { echo "FAIL: pool A should have more shares at 70/30 (A=$SA B=$SB)"; fail=1; }
[ "${MINERS:-0}" -ge 1 ] || { echo "FAIL: no connected miners shown in status"; fail=1; }
[ "$HTML_CODE" = "200" ] && [ "$TITLE_OK" -ge 1 ] || { echo "FAIL: dashboard did not serve"; fail=1; }
[ "$POST_CODE" = "200" ] && [ "$NEW_RATIO" = "40" ] || { echo "FAIL: config hot-apply (code=$POST_CODE ratio=$NEW_RATIO)"; fail=1; }
[ "${METRICS_OK:-0}" -ge 1 ] || { echo "FAIL: /metrics did not expose dualpool_pool_up"; fail=1; }
echo "metrics: dualpool_pool_up lines=$METRICS_OK"

if [ "$fail" -eq 0 ]; then
  echo "WEBUI PASS: /api/status live, dashboard served, /api/config hot-applied."
  exit 0
else
  echo "WEBUI FAILED"; echo "--- splitter log ---"; tail -15 /tmp/sx_web.log; exit 1
fi
