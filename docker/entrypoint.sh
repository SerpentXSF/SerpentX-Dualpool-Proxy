#!/bin/sh
# Dual-Pool Proxy container entrypoint. If /config/config.json is present it is used
# as-is. Otherwise a config is generated from environment variables (the .env
# quickstart) so non-developers never have to hand-edit JSON. GPLv3.
set -e
CONFIG=/config/config.json

# Escape a value for embedding inside a JSON string (backslash and double-quote),
# so a special character in a password/username can't produce invalid JSON.
esc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }

if [ ! -f "$CONFIG" ]; then
  if [ -z "$POOL_A_URL" ] || [ -z "$POOL_B_URL" ]; then
    echo "dualpool: no $CONFIG and POOL_A_URL/POOL_B_URL not set." >&2
    echo "dualpool: mount a config at /config/config.json OR set env vars"    >&2
    echo "dualpool: (see .env.example)."                                      >&2
    exit 1
  fi
  mkdir -p /config
  echo "dualpool: generating $CONFIG from environment" >&2

  # Escape every string value that gets embedded in the JSON.
  A_URL=$(esc "$POOL_A_URL");  A_USER=$(esc "$POOL_A_USER");  A_PASS=$(esc "${POOL_A_PASS:-x}");  A_MODE=$(esc "${POOL_A_MODE:-userproxy}")
  B_URL=$(esc "$POOL_B_URL");  B_USER=$(esc "$POOL_B_USER");  B_PASS=$(esc "${POOL_B_PASS:-x}");  B_MODE=$(esc "${POOL_B_MODE:-userproxy}")
  WEBPW=$(esc "${WEB_PASSWORD:-}")
  MODEV=$(esc "${MODE:-farm_split}")

  fa=""
  [ -n "$POOL_A_FAILOVER_URL" ] && fa=",\"failover\":{\"url\":\"$(esc "$POOL_A_FAILOVER_URL")\",\"user\":\"$A_USER\",\"pass\":\"$A_PASS\"}"
  fb=""
  [ -n "$POOL_B_FAILOVER_URL" ] && fb=",\"failover\":{\"url\":\"$(esc "$POOL_B_FAILOVER_URL")\",\"user\":\"$B_USER\",\"pass\":\"$B_PASS\"}"

  # Optional per-pool difficulty floor. Emit only if the value is a plain integer,
  # so a stray value can't inject into the JSON.
  da=""
  case "$POOL_A_STARTDIFF" in ''|*[!0-9]*) ;; *) da="$da,\"startdiff\":$POOL_A_STARTDIFF" ;; esac
  case "$POOL_A_MINDIFF"   in ''|*[!0-9]*) ;; *) da="$da,\"mindiff\":$POOL_A_MINDIFF"     ;; esac
  db=""
  case "$POOL_B_STARTDIFF" in ''|*[!0-9]*) ;; *) db="$db,\"startdiff\":$POOL_B_STARTDIFF" ;; esac
  case "$POOL_B_MINDIFF"   in ''|*[!0-9]*) ;; *) db="$db,\"mindiff\":$POOL_B_MINDIFF"     ;; esac

  cat > "$CONFIG" <<EOF
{
  "downstream": { "stratum_port": ${STRATUM_PORT:-3333}, "web_port": ${WEB_PORT:-8080} },
  "mode": "${MODEV}",
  "ratio_a": ${RATIO_A:-70},
  "interval_ms": ${INTERVAL_MS:-180000},
  "web_password": "${WEBPW}",
  "pools": [
    { "url": "${A_URL}", "user": "${A_USER}", "pass": "${A_PASS}", "ckproxy_mode": "${A_MODE}"${da}${fa} },
    { "url": "${B_URL}", "user": "${B_USER}", "pass": "${B_PASS}", "ckproxy_mode": "${B_MODE}"${db}${fb} }
  ]
}
EOF
fi

exec dualpool-splitter --config "$CONFIG"
