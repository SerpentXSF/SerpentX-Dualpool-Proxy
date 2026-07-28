#!/bin/sh
# Dual-Pool Proxy container entrypoint. If /config/config.json is present it is used
# as-is. Otherwise a config is generated from environment variables (the .env
# quickstart) so non-developers never have to hand-edit JSON. GPLv3.
set -e
CONFIG=/config/config.json

if [ ! -f "$CONFIG" ]; then
  if [ -z "$POOL_A_URL" ] || [ -z "$POOL_B_URL" ]; then
    echo "dualpool: no $CONFIG and POOL_A_URL/POOL_B_URL not set." >&2
    echo "dualpool: mount a config at /config/config.json OR set env vars"    >&2
    echo "dualpool: (see .env.example)."                                      >&2
    exit 1
  fi
  mkdir -p /config
  echo "dualpool: generating $CONFIG from environment" >&2

  fa=""
  [ -n "$POOL_A_FAILOVER_URL" ] && fa=",\"failover\":{\"url\":\"$POOL_A_FAILOVER_URL\",\"user\":\"$POOL_A_USER\",\"pass\":\"${POOL_A_PASS:-x}\"}"
  fb=""
  [ -n "$POOL_B_FAILOVER_URL" ] && fb=",\"failover\":{\"url\":\"$POOL_B_FAILOVER_URL\",\"user\":\"$POOL_B_USER\",\"pass\":\"${POOL_B_PASS:-x}\"}"

  cat > "$CONFIG" <<EOF
{
  "downstream": { "stratum_port": ${STRATUM_PORT:-3333}, "web_port": ${WEB_PORT:-8080} },
  "mode": "${MODE:-farm_split}",
  "ratio_a": ${RATIO_A:-70},
  "interval_ms": ${INTERVAL_MS:-180000},
  "web_password": "${WEB_PASSWORD:-}",
  "pools": [
    { "url": "${POOL_A_URL}", "user": "${POOL_A_USER}", "pass": "${POOL_A_PASS:-x}", "ckproxy_mode": "${POOL_A_MODE:-userproxy}"${fa} },
    { "url": "${POOL_B_URL}", "user": "${POOL_B_USER}", "pass": "${POOL_B_PASS:-x}", "ckproxy_mode": "${POOL_B_MODE:-userproxy}"${fb} }
  ]
}
EOF
fi

exec dualpool-splitter --config "$CONFIG"
