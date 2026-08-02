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
  "target_shares": 10,
  "min_slice_s": 10,
  "max_slice_s": 120,
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
| `mode` | `farm_split`, `time_slice`, or `hashrate_split` | `farm_split` |
| `ratio_a` | Percent of hashrate to Pool A (0–100) | `50` |
| `interval_ms` | Time-slice slice length (ms), 1000–3600000 | `180000` |
| `target_shares` | `hashrate_split`: shares per slice before swapping pools, 1–1000 | `10` |
| `min_slice_s` | `hashrate_split`: minimum slice length (s), 1–3600 | `10` |
| `max_slice_s` | `hashrate_split`: maximum slice length (s), 1–3600 | `120` |
| `assume_extranonce` | `hashrate_split`: trust miners to honour `mining.set_extranonce` without advertising it (see below) | `false` |
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

| | `farm_split` (default) | `time_slice` | `hashrate_split` |
|---|---|---|---|
| Best for | 2+ miners / a farm | a single miner | a single miner you want on **both** pools **at once** |
| How | each miner pinned to one pool | one miner recycled between pools every `interval_ms` | the proxy time-slices the miner across pools via a Stratum-aware multiplexer |
| Switching loss | none | a few stale shares per boundary | minimal — adaptive slices on clean-job boundaries |
| `interval_ms` | ignored | use **minutes** (e.g. `180000` = 3 min) | ignored |
| `target_shares`/`min_slice_s`/`max_slice_s` | ignored | ignored | used (see below) |

Don't use `time_slice` for a farm — it recycles *all* connections at each
boundary. It exists specifically to give a lone miner a dual-pool split that no
stock firmware offers.

---

## `hashrate_split` mode

> ⚠️ **Experimental.** `farm_split` and `time_slice` remain the supported modes;
> use this one knowing what has and hasn't been proven.
>
> **Validated:** a single ~1.5 TH/s miner (BitAxe-class, BM1370) against
> solo.ckpool + Kryptex — 12 hours unattended, **4,672 shares accepted, 1
> rejected (0.02%)**, work split 69.8/30.2 against a 70/30 target, credited
> hashrate matching the miner's real rate, and no reconnect churn. This was with
> each pool's difficulty pinned (see below) — **that part is required.**
>
> **Not proven / known limits:**
> - A **large miner against a pool that opens at a very high difficulty** (Kryptex
>   opens sessions at 1,000,000) can struggle to bootstrap: it finds its first
>   share too slowly for the pool's vardiff to come down, and some firmware
>   watchdogs reconnect first. The proxy now suggests a difficulty on the miner's
>   behalf once it can measure the rate, which rescues most sessions, but this is
>   reactive — it needs one share first.
> - Every fresh session pays a small **difficulty-settling burst** of rejects while
>   both pools size the miner. It amortises to ~0 over hours, but is visible on
>   short runs and on frequent restarts.
> - Only miners that honour `mining.set_extranonce` get the smooth in-place swap;
>   others fall back to a brief reconnect per slice (see `assume_extranonce`).

`hashrate_split` puts a **single miner on both pools at the same time**, the way
some dual-pool BitAxe/NerdQAxe firmware works — but without reflashing anything.
The miner connects once, to the proxy; internally, the proxy multiplexes it
across upstream connections to Pool A and Pool B, swapping which pool it's
mining for at slice boundaries so its shares land on both according to `ratio_a`.

### Required: pin each pool's difficulty

**Set `startdiff` and `mindiff` on BOTH pools before using this mode.** It is not
a tuning option — without it the mode looks broken.

A pool sizes a miner with vardiff by watching how fast shares arrive. Time-slicing
breaks that assumption: from Pool A's point of view your miner hashes at full rate
during A's slices and **stops completely** during B's, and vice versa. Each pool
chases that on/off pattern, and every difficulty change strands the shares already
in flight, which come back as `Above target` rejects. Pinning a floor stops the
chase.

Measured on the same miner, same pools, back to back:

| | Difficulty behaviour | Reject rate |
|---|---|---|
| No `mindiff` | oscillating (`1205 → 226 → 653` …) | **2.2%**, climbing |
| `mindiff` pinned | fixed, unchanged over 64 pool swaps | **0.02%** |

Size it for roughly one share every 10–15 seconds — the same values as the
[`startdiff` table below](#suggested-startdiff-by-miner-hashrate), applied to
**both** pools, with `mindiff` equal to `startdiff` so nothing can drift under it:

```json
"pools": [
  { "url": "solo.ckpool.org:3333", "user": "bc1qYourAddress.worker", "pass": "x",
    "startdiff": 4096, "mindiff": 4096 },
  { "url": "otherpool:3333", "user": "otherwallet.worker", "pass": "x",
    "startdiff": 4096, "mindiff": 4096 }
]
```

If the two pools enforce different floors, give each its own value — they don't
have to match, they just each have to stop moving. A pool's own floor always wins,
so pick at least that.

This also protects a **large** miner from a pool that opens a session at a very
high difficulty: without a floor it may find its first share too slowly for the
pool's vardiff to recover, and some firmware watchdogs reconnect before it does.

**Knobs:**

- `target_shares` — roughly how many shares to collect before considering a
  slice "done" (a proxy for slice length that adapts to the miner's actual
  hashrate, instead of a fixed wall-clock timer).
- `min_slice_s` / `max_slice_s` — hard floor/ceiling (seconds) on slice length,
  so a slow miner isn't swapped too often and a fast one isn't stuck too long.
  Longer slices mean fewer swaps and less work discarded at each one; the
  validated configuration used 300 s / 900 s.

**How the pool swap happens:** on a miner that honors `mining.set_extranonce`,
the proxy swaps the miner onto the new pool's job stream in place — smooth, no
reconnect. For a miner that doesn't support `set_extranonce`, the proxy
**automatically falls back to reconnect-slicing**: it still splits the miner
across both pools, just with a brief reconnect at each swap instead of an
in-place handoff.

**`assume_extranonce` (advanced, default `false`):** the proxy decides which of
those two paths to use by watching for `mining.extranonce.subscribe` — a miner
that never sends it is assumed not to honour `mining.set_extranonce`, and gets
reconnect-slicing. Several ESP-Miner-derived firmwares (BitAxe, Hammer, NerdAxe,
NerdQAxe) *do* honour `set_extranonce` but never advertise it, so they land on the
reconnect path unnecessarily — costing a disconnect per slice and the ramp-up
after it.

```json
{ "mode": "hashrate_split", "assume_extranonce": true }
```

Setting it tells the proxy to use the smooth in-place swap for **every** split
miner, advertised or not. Turn it on only if your miners really do follow
`set_extranonce`: one that ignores it will keep mining the old pool's extranonce
after a swap and its shares will be **rejected** until it reconnects. The failure
is obvious and immediate — rejects climb right after the first swap — and turning
the key back off restores the previous behaviour with no other change.

Like mode/interval, `hashrate_split`'s knobs are **restart-applied** — changing
them via the dashboard or `config.json` takes effect on the next restart (only
`ratio_a` hot-applies without one).

### Known limitations (first release)

- **Difficulty settling on a fresh session:** when a session starts, each pool
  sizes the miner from scratch, and shares already in flight while a pool moves
  its difficulty are rejected as *"Above target"*. It is a burst at connect, not
  an ongoing rate — it amortises to roughly nothing over a long session, but it
  is visible on short runs and adds up if the proxy restarts often.
- **Bootstrapping a large miner on a high-difficulty pool:** a pool that opens a
  session at a very high difficulty (Kryptex opens at 1,000,000) can starve a big
  miner — it finds its first share too slowly for the pool's vardiff to come down,
  and some firmware watchdogs give up and reconnect first. The proxy suggests a
  difficulty on the miner's behalf once it can measure the rate, which clears the
  problem for most sessions, but it is reactive: it needs one share first. A small
  miner (~1–2 TH/s) is unaffected.
- **Split accounting is per connection:** a split miner's row shows its combined
  accepted/rejected across both pools, while the `pool` column shows the pool it
  started on. Per-pool totals and `/metrics` attribute each share to the pool that
  actually acknowledged it.
- **Reduced 90s no-work auto-donation:** normally, if a pool sends no work for
  90 seconds while it has miners, Dual-Pool Proxy evicts those miners onto the
  healthy pool (see [Failover](#failover-per-pool)). For a pool that's only
  serving `hashrate_split` connections, this auto-donation is weaker, because
  the mux mitigates the same failure itself: it self-heals by degrading the
  split miner onto the single healthy pool rather than waiting on the proxy's
  eviction path. In practice you're still protected, just via a different
  mechanism than the one documented above.

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
  the pools (incl. each pool's **fallback**), ratio, mode, and (for
  `hashrate_split`) `target_shares`/`min_slice_s`/`max_slice_s`, and **writes them
  to `config.json`**. A **ratio** change applies instantly with no miner
  disconnects; **mode**, **interval**, **hashrate_split knobs**, and
  **pool/credential/fallback** changes are saved and take effect after a restart.
  The form won't overwrite a field while you're editing it, and a password field
  left **blank keeps** the current password.
- **REST:** `GET /api/status` (JSON), `POST /api/config`
  (`{ratio_a, mode, interval_ms, target_shares, min_slice_s, max_slice_s,
  pools:[{url,user,pass}]}` — any subset).
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
- **Mode / interval / hashrate_split knobs** (`target_shares`, `min_slice_s`,
  `max_slice_s`) → saved immediately, take effect on `docker compose restart
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
