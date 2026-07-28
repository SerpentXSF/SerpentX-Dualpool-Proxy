#!/usr/bin/env bash
# Verify the dashboard's Save & apply writes the FULL config (ratio + pool fields)
# back to config.json, and that a blank password keeps the current one. Runs in
# dualpool-dev; a stub ckproxy (/bin/true) stands in since we only test the
# config-editing path, not upstream mining. GPLv3.
set -u
cd "$(dirname "$0")/../.."
make dualpool-splitter >/dev/null 2>&1 || { echo "FAIL: compile"; exit 1; }
RUN=/tmp/cfgedit; rm -rf "$RUN"; mkdir -p "$RUN"
CFG="$RUN/config.json"; WEB=8096

cat > "$CFG" <<'EOF'
{ "downstream": { "stratum_port": 3360, "web_port": 8096 }, "mode": "farm_split", "ratio_a": 70,
  "web_password": "",
  "pools": [ {"url":"old-a.example:3333","user":"userA","pass":"passA","ckproxy_mode":"userproxy"},
             {"url":"old-b.example:3333","user":"userB","pass":"passB","ckproxy_mode":"proxy"} ] }
EOF

DUALPOOL_CKPOOL_BIN=/bin/true DUALPOOL_RUNDIR="$RUN/run" ./dualpool-splitter --config "$CFG" >/tmp/cfgedit.log 2>&1 &
SP=$!
trap "kill $SP 2>/dev/null; wait 2>/dev/null" EXIT
sleep 2

field(){ python3 -c 'import json,sys;c=json.load(open(sys.argv[1]));print(eval(sys.argv[2]))' "$CFG" "$1"; }

echo "== edit: ratio 70->55, pool A url + password, leave B password blank =="
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"ratio_a":55,"mode":"farm_split","pools":[{"url":"new-a.example:4444","user":"userA","pass":"NEWPASS"},{"url":"old-b.example:3333","user":"userB","pass":""}]}' \
  "http://127.0.0.1:$WEB/api/config" >/dev/null
sleep 0.3

echo "-------------------------------------------"
echo "config.json now: ratio=$(field 'c["ratio_a"]')  A.url=$(field 'c["pools"][0]["url"]')  A.pass=$(field 'c["pools"][0]["pass"]')  A.mode=$(field 'c["pools"][0]["ckproxy_mode"]')  B.mode=$(field 'c["pools"][1]["ckproxy_mode"]')"
echo "api/status:      $(curl -s http://127.0.0.1:$WEB/api/status | python3 -c 'import sys,json;d=json.load(sys.stdin);print("ratio="+str(d["ratio_a"]),"A.url="+d["pools"][0]["url"])')"

echo "== second save with BLANK passwords (should keep A.pass=NEWPASS) =="
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"pools":[{"url":"new-a.example:4444","user":"userA","pass":""},{"url":"old-b.example:3333","user":"userB","pass":""}]}' \
  "http://127.0.0.1:$WEB/api/config" >/dev/null
sleep 0.3
echo "config.json A.pass after blank save: $(field 'c["pools"][0]["pass"]')"
echo "-------------------------------------------"

fail=0
[ "$(field 'c["ratio_a"]')" = "55" ]                 || { echo "FAIL: ratio not saved"; fail=1; }
[ "$(field 'c["pools"][0]["url"]')" = "new-a.example:4444" ] || { echo "FAIL: pool A url not saved"; fail=1; }
[ "$(field 'c["pools"][0]["pass"]')" = "NEWPASS" ]   || { echo "FAIL: pool A password not saved/kept"; fail=1; }
[ "$(field 'c["pools"][0]["ckproxy_mode"]')" = "userproxy" ] || { echo "FAIL: A ckproxy_mode not preserved"; fail=1; }
[ "$(field 'c["pools"][1]["ckproxy_mode"]')" = "proxy" ]     || { echo "FAIL: B ckproxy_mode not preserved"; fail=1; }
[ "$(field 'c["downstream"]["web_port"]')" = "8096" ]        || { echo "FAIL: downstream not preserved"; fail=1; }

if [ "$fail" -eq 0 ]; then
  echo "CONFIG-EDIT PASS: full config (ratio + pools) persisted; blank password kept; other keys preserved."
  exit 0
else
  echo "CONFIG-EDIT FAILED"; tail -5 /tmp/cfgedit.log; exit 1
fi
