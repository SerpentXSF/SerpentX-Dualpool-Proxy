# Configuration & Optimization

Dual-Pool Proxy is configured either by a **`config.json`** (mounted at `/config/`) or by
**environment variables** (`.env`, used only when no `config.json` is present).
This page documents every option and how to tune it.

---

## Config file reference (`config.json`)

```json
{
  "downstream": { "stratum_port": 3333, "web_port": 8080 },
  "mode": "farm_split",
  "ratio_a": 70,
  "interval_ms": 180000,
  "web_password": "",
  "pools": [
    { "url": "poolA:3333", "user": "walletA.worker", "pass": "x",
      "ckproxy_mode": "userproxy",
      "failover": { "url": "backupA:3333", "user": "walletA.worker", "pass": "x" } },
    { "url": "poolB:3333", "user": "walletB.worker", "pass": "x",
      "ckproxy_mode": "userproxy" }
  ]
}
```

| Field | Meaning | Default |
|---|---|---|
| `downstream.stratum_port` | Port miners connect to | `3333` |
| `downstream.web_port` | Dashboard / API port | `8080` |
| `mode` | `farm_split` or `time_slice` | `farm_split` |
| `ratio_a` | Percent of hashrate to Pool A (0–100) | `50` |
| `interval_ms` | Time-slice slice length (ms), 1000–3600000 | `180000` |
| `web_password` | If set, dashboard/API/metrics require this key | `""` (off) |
| `pools[]` | Exactly **two** pool entries | — |
| `pools[].url` | Pool `host:port` | required |
| `pools[].user` | Username / wallet.worker | required |
| `pools[].pass` | Password (usually `x`) | `x` |
| `pools[].ckproxy_mode` | `userproxy` or `proxy` (see below) | `userproxy` |
| `pools[].failover` | Optional `{url,user,pass}` backup for **that** pool | none |

### Environment-variable equivalents (`.env`)

`POOL_A_URL`, `POOL_A_USER`, `POOL_A_PASS`, `POOL_A_FAILOVER_URL`, `POOL_A_MODE`
(and the `POOL_B_*` equivalents), plus `RATIO_A`, `MODE`, `INTERVAL_MS`,
`STRATUM_PORT`, `WEB_PORT`, `WEB_PASSWORD`. See [`.env.example`](../.env.example).

---

## Choosing your split (`ratio_a`)

`ratio_a` is the **long-run share of hashrate** sent to Pool A, weighted by each
miner's real hashrate (not by miner count). Examples:

- `ratio_a: 70` → 70% to A, 30% to B.
- `ratio_a: 100` → everything to A (B is a hot standby via failover/donation).
- `ratio_a: 0` → everything to B.

**Small-fleet reality:** with only 1–3 miners the split is *coarse* — Dual-Pool Proxy
can't split a single miner in `farm_split` mode (one miner = one pool for its
session). Options:
- Run **2+ miners** so the weighted split can approximate your ratio.
- Use **`time_slice`** mode to split a *single* miner over time.
- Accept coarse granularity (e.g. one BitAxe at `ratio_a: 70` will sit entirely
  on A until it reconnects).

The dashboard shows **actual vs target** so you can see the realized split.

---

## Choosing the mode

| | `farm_split` (default) | `time_slice` |
|---|---|---|
| Best for | 2+ miners / a farm | a single miner |
| How | each miner pinned to one pool | one miner recycled between pools every `interval_ms` |
| Switching loss | none | a few stale shares per boundary |
| `interval_ms` | ignored | use **minutes** (e.g. `180000` = 3 min) |

Don't use `time_slice` for a farm — it recycles *all* connections at each
boundary. It exists specifically to give a lone miner a dual-pool split that no
stock firmware offers.

---

## `ckproxy_mode`: `userproxy` vs `proxy`

Each pool is served by a stock `ckproxy` under the hood.

- **`userproxy` (default, recommended):** opens an upstream connection per
  downstream username, so **your worker names appear on the pool's dashboard**.
  Essential for solo/DATUM/public pools where your BTC address *is* the username.
- **`proxy`:** all shares are submitted under the single username in the config.
  Slightly lighter, but you lose per-worker visibility at the pool.

---

## Failover (per pool)

Add a `failover` block to a pool and its `ckproxy` will automatically switch to
the backup upstream if the primary dies — independently for each pool. This is
**inside** one pool (primary → backup).

Separately, Dual-Pool Proxy does **pool-level failover**: if a whole pool (both its
endpoints) becomes unreachable — or stops sending work for 90s while it has
miners — Dual-Pool Proxy stops sending it new miners, **evicts** its current miners so
they reconnect onto the surviving pool (**donation**), and automatically returns
traffic when the pool recovers. Nothing to configure; it's always on.

---

## Dashboard, API & metrics

- **Dashboard:** `http://<host>:8080` — live status + a settings form that edits
  the **whole config** (pools, ratio, mode) and **writes it to `config.json`**.
  Ratio/mode apply instantly (no miner disconnects); pool/credential changes are
  saved and take effect after a restart. Leave a password field **blank to keep**
  the current password.
- **REST:** `GET /api/status` (JSON), `POST /api/config`
  (`{ratio_a, mode, interval_ms, pools:[{url,user,pass}]}` — any subset).
- **Prometheus:** `GET /metrics` — import
  [`grafana/dualpool-dashboard.json`](../grafana/dualpool-dashboard.json).
- **Password:** set `web_password` (or `WEB_PASSWORD`). Then `/api/*` and
  `/metrics` require it as header `X-DualPool-Key: <pw>` or `?key=<pw>`. The
  dashboard prompts once and remembers it. Static files stay open. It's a simple
  LAN guard, not a hardened auth system — don't expose Dual-Pool Proxy to the public
  internet.

## Which file is the "live" config? (`config.json` vs `.env`)

**`config/config.json` is the single source of truth once running.** Edit it two
equivalent ways:

- **The dashboard** (easiest) → writes `config/config.json`.
- **Editing `config/config.json`** directly on the host.

**`.env` is only a first-run convenience:** on the very first start, if no
`config/config.json` exists, the container generates one from `.env`. After that
`config.json` exists and **wins**, so editing `.env` alone does nothing. To make
`.env` regenerate the config, delete `config/config.json` and restart (you'll lose
dashboard edits). For day-to-day changes, use the dashboard or edit `config.json`.

## Applying changes

- **Ratio / mode** → apply **instantly** from the dashboard (or `docker kill -s
  HUP dualpool-proxy` after editing `config.json`), without dropping miners.
- **Pool URLs / usernames / passwords** → saved immediately, take effect on
  `docker compose restart dualpool-proxy` (a pool swap reconnects the miners on it).

## Optimization checklist

- **Match `ratio_a` to your goal**, and verify the realized split on the dashboard.
- **≥2 miners** for a smooth farm-split; otherwise use `time_slice`.
- Keep **`userproxy`** so pools show your workers (especially solo pools).
- Add a **`failover`** to each pool for resilience.
- Put the **solo/self-hosted pool on your LAN** for lowest latency (see
  [STRATEGIES.md](STRATEGIES.md)).
- Scrape **`/metrics`** into Grafana to watch accept/reject and the split over time.
- Set a **`web_password`** if others share your network.
