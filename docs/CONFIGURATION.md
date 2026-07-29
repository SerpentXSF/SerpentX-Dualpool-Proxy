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
| `pools[].startdiff` | Optional starting difficulty for **that** pool | `42` |
| `pools[].mindiff` | Optional minimum difficulty for **that** pool | `1` |

**When to set `startdiff` / `mindiff`:** most pools dictate their own difficulty and
you can leave these out. Set them only for a pool that enforces a difficulty *floor*
(for example public-pool.io defaults to ~100000). Setting the floor to that value
makes your miners submit shares the pool will accept from the first connection. A
value of `0` (or omitting the field) means "use the pool's difficulty."

### Environment-variable equivalents (`.env`)

`POOL_A_URL`, `POOL_A_USER`, `POOL_A_PASS`, `POOL_A_FAILOVER_URL`, `POOL_A_MODE`,
`POOL_A_STARTDIFF`, `POOL_A_MINDIFF` (and the `POOL_B_*` equivalents), plus
`RATIO_A`, `MODE`, `INTERVAL_MS`, `STRATUM_PORT`, `WEB_PORT`, `WEB_PASSWORD`. See
[`.env.example`](../.env.example).

---

## Choosing your split (`ratio_a`)

`ratio_a` is the **long-run share of your fleet** sent to Pool A. In farm-split each
miner is pinned to one pool to approach this ratio by miner count, so with
similar-sized miners it tracks hashrate closely; with very mixed sizes it is coarser
(hashrate-weighted allocation is on the roadmap). Examples:

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

## Pool compatibility & starting difficulty

### Which pools work

The proxy relays standard Stratum V1 and forwards your miners' ASICBoost
version-rolling bits, so it works with the large majority of SHA-256 pools. Verified
with real hardware:

| Pool | Endpoint | Works? | Notes |
|---|---|---|---|
| solo.ckpool | `solo.ckpool.org:3333` | Yes | Sets its own difficulty |
| Kryptex (BCH solo) | `bch-us.kryptex.network:7015` | Yes | Username is `solo:<bch-address>.<worker>` |
| public-pool.io | `public-pool.io:21496` | Yes | Enforces a difficulty floor — set `startdiff` (below) |
| Parasite | `parasite.wtf:42069` | Yes | Sets its own difficulty |
| OCEAN | `mine.ocean.xyz:3334` | Not yet | Rejects version-rolled shares it hasn't negotiated (`H-not-zero`) |
| Stratum V2 pools (e.g. DEMAND) | — | No | The proxy speaks Stratum V1; an SV2 pool needs a V1↔V2 translator in front |

**Rule of thumb:** any pool that accepts a normal V1 miner (BitAxe, Antminer) works.
The exceptions are **OCEAN**, which requires ASICBoost version-rolling to be
negotiated up front (planned), and **Stratum V2-only pools**, which a V1 proxy can't
talk to.

### Setting the starting difficulty (`startdiff`)

**Most pools set the difficulty for you** — leave `startdiff` unset and the pool's
own difficulty takes over within a few seconds of connecting. You only need
`startdiff` when a pool enforces a **minimum difficulty** and rejects anything lower.
The clearest example is **public-pool.io**, whose floor is **100000**:

```json
{ "url": "public-pool.io:21496", "user": "bc1qYourAddress.worker", "pass": "x",
  "startdiff": 100000, "mindiff": 100000 }
```

Without it, public-pool.io rejects every share as *"Difficulty too low"* and your
worker looks dead at the pool.

### Suggested `startdiff` by miner hashrate

If you do need to set a difficulty — a pool with a floor, or you just want a sensible
starting point — pick roughly by hashrate. These target about one submitted share
every 10–15 seconds (responsive without spamming):

| Miner hashrate | Typical hardware | Suggested `startdiff` |
|---|---|---|
| ~0.5 TH/s | BitAxe (BM1366), NerdAxe | `1000` |
| ~1–1.3 TH/s | BitAxe Gamma (BM1370) | `3000` |
| ~2–4.5 TH/s | NerdQAxe++ | `8000` |
| ~13 TH/s | Antminer S9 | `40000` |
| ~50 TH/s | mid-range ASIC | `150000` |
| ~100 TH/s | Antminer S19 / Whatsminer | `350000` |

Two things to keep in mind:

- **The pool's own floor wins.** For public-pool.io use at least **100000** even on a
  small BitAxe — its shares just arrive less often (that's the pool's design). In
  general, set `startdiff` to the *higher* of the pool's floor and the value above.
- **`startdiff` is per pool, not per miner.** In farm-split it applies to every miner
  that lands on that pool, so if miners of different sizes share it, size `startdiff`
  for the largest one. From there the pool's difficulty adjustment takes over.

---

## Failover (per pool)

Add a `failover` block to a pool and its `ckproxy` will automatically switch to
the backup upstream if the primary dies — independently for each pool. This is
**inside** one pool (primary → backup). You can set it either in `config.json` (the
`failover` block per pool) or from the **dashboard** (the "Fallback URL / User"
fields under each pool); the fallback reuses that pool's password. Leave the
fallback URL blank for no fallback.

Separately, Dual-Pool Proxy does **pool-level failover**: if a whole pool (both its
endpoints) becomes unreachable — or stops sending work for 90s while it has
miners — Dual-Pool Proxy stops sending it new miners, **evicts** its current miners so
they reconnect onto the surviving pool (**donation**), and automatically returns
traffic when the pool recovers. Nothing to configure; it's always on.

---

## Dashboard, API & metrics

- **Dashboard:** `http://<host>:8080` — live status + a settings form that edits
  the pools (incl. each pool's **fallback**), ratio, and mode and **writes them to
  `config.json`**. A **ratio** change applies instantly with no miner disconnects;
  **mode**, **interval**, and **pool/credential/fallback** changes are saved and
  take effect after a restart. The form won't overwrite a field while you're
  editing it, and a password field left **blank keeps** the current password.
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

- **Ratio** → applies **instantly** from the dashboard (or `docker kill -s HUP
  dualpool-proxy` after editing `config.json`), without dropping miners.
- **Mode / interval** → saved immediately, take effect on `docker compose restart
  dualpool-proxy`.
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
