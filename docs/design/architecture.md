# Dual-Pool Stratum Proxy — Architecture & Design

**License:** GPLv3 (derivative of ckpool)

---

## 1. Purpose

A standalone Linux/Docker service that makes **any** stock-firmware Stratum
SHA-256d miner (BitAxe, NerdAxe, Antminer, HammerMiner DC02/BC, Avalon,
Whatsminer, …) mine to **two pools simultaneously** in a configurable ratio,
with per-pool failover — **without touching miner firmware**.

Every Stratum miner lets you set a pool `host:port`. This proxy presents itself
as *one* pool, and internally splits hashrate across two real pools by ratio.

### Why it exists / who it's for

This spun out of an ESP32 firmware project that added true dual-pool mining
*inside* the miner firmware. That only works on open-source firmware
(BitAxe/NerdAxe/NerdQAxe). Closed miners (e.g. HammerMiner — firmware is closed,
only web-UI blobs are published) can't be modified. A proxy solves it for
everyone.

### Competitive position

Nobody owns this exact niche. Everything that truly ratio-splits is
**firmware/miner-software** (NerdQAxe dual-pool, cgminer `--quota`, Braiins OS
pool groups, LuxOS hashrate splitting — all require reflashing or open firmware).
Everything at the **proxy layer** is either failover/round-robin
(Stratehm/stratum-proxy, xmrig-proxy, stock ckproxy), closed-source time-slice
(Ultimate-Proxy, SPLIT Proxy — people pay for these), or Ethash-era fee-skimmers
(TCMinerProxy family). 

**Our moat:** open, maintained, SHA-256d, proxy-level ratio split that works on
**closed/mixed fleets with no reflash and no warranty risk** — one box for all
miners. The single most important differentiator for our home-miner audience is a
**DATUM-style embedded web dashboard**; for this audience that is the product,
not a nice-to-have.

**Honest baseline we beat:** HAProxy `balance` with server weights 70/30 across
two ckproxy backends gives ~80% of farm-split mode with zero new code. Our
splitter earns its existence through four things HAProxy cannot do:
**hashrate-weighted** (not connection-weighted) allocation, **per-pool share
accounting**, **donation/recovery** logic, and the **web UI**. This is stated
plainly in the README because it is the first question any reviewer asks.

---

## 2. Architecture — orchestrator + two stock ckproxy instances

One Docker container runs three processes, supervised by our new `splitter`
binary. **ckpool binaries are unmodified** — all new code is the splitter.

```
  Miners                         Container (our image)
 BitAxe ─┐                     ┌─────────────────────────────────────────────┐
 NerdAxe ┤                     │  splitter (new C)                            │
 Antminer┼── :3333 (stratum)──►│   • assigns each miner → pool A or B         │      Real Pool A
 Hammer ─┘                     │     by HASHRATE-WEIGHTED error diffusion     │ ──►  (+ failover,
        ┌── :8080 (web UI)────►│   • transparent Stratum relay per miner      │      handled by
   User └──────────────────────│   • passive share sniff + ckpmsg polling     │      ckproxy A)
                               │   • supervises + health-gates the ckproxies  │      Real Pool B
                               │  ckproxy A (STOCK) ─ unix/127.0.0.1:4001 ─────┼──►  (+ failover,
                               │  ckproxy B (STOCK) ─ unix/127.0.0.1:4002 ─────┼──►  by ckproxy B)
                               └─────────────────────────────────────────────┘
```

**Key structural property:** in farm-split, a miner is relayed end-to-end to
exactly one ckproxy for its whole session. Therefore ckproxy handles all the hard
Stratum work (subscribe/authorize/notify, **vardiff**, `mining.configure`
version-rolling/ASICBoost, extranonce subdivision, and native **intra-pool
primary→failover**), and **"route every share to the owning pool" is satisfied
structurally** — a miner never talks to the wrong pool, so no job-tagging is
needed in farm-split. This is the whole reason the architecture is safe.

### 2.1 ckproxy mode: default `userproxy` (-u)

Plain proxy mode presents all downstream shares to the pool as a single upstream
user (from the ckproxy config), so **per-miner worker names disappear at the
pool** — bad for solo pools where the BTC address *is* the username
(solo.ckpool / public-pool users would never see their address). We default each
ckproxy to **userproxy mode**, which opens an upstream connection per downstream
username and preserves pool-side identity. Configurable per pool. Passthrough
(-P) is only useful against ckpool-based upstreams and is never the default.

---

## 3. The `splitter` — modules (single C binary, links ckpool's bundled jansson)

Each module has one purpose and is independently testable.

| Module | Responsibility |
|---|---|
| `config` | Parse our JSON (jansson): `ratio_a`, `mode`, `interval_ms`, ports, 2 pools + failovers, ckproxy mode. Clamp via `dual_clamp`. |
| `ckproxy_config` | Emit `ckproxyA.json` / `ckproxyB.json` (ckpool `proxy` arrays incl. each pool's failover + `nonce1length`/`nonce2length`), then fork/exec + supervise the two stock ckproxy processes (distinct sockdirs, exponential-backoff restart). |
| `alloc` | **Hashrate-weighted** error-diffusion allocator. Reuses `pool_scheduler`'s Bresenham math; weights each allocation unit by measured per-miner hashrate, not 1-per-connection. Decides each new/reconnecting miner's pool. |
| `relay` | Per miner: `accept()`, pick pool, `connect()` to that ckproxy, pump bytes **verbatim** both directions (epoll), sniffing a copy of each line. Forces clean TCP disconnect on reassignment (never splices a live session across pools). |
| `share_accounting` | Track `mining.set_difficulty` per session; correlate JSON-RPC `id` submit→result; keep **difficulty-weighted** per-pool accepted/rejected counters. |
| `health` | Poll each ckproxy via `ckpmsg` unix-socket admin API (authoritative hashrate/shares) + liveness from job flow. Feed `pool_failover` per pool. On pool DOWN: stop allocating there, **actively evict** its miners (close downstream sockets so they reconnect and get reallocated), with per-source-IP reconnect backoff. On recovery: drift allocations back on natural reconnects, don't mass-evict. |
| `webui` | Serve a single static dashboard page + a tiny JSON REST API (`/api/status`, `/api/config`) + Prometheus `/metrics`. Settings form writes config + hot-applies. Optional dashboard password (doubles as CSRF guard). |

### 3.1 The allocator must be hashrate-weighted (critical for small fleets)

Connection-count error diffusion only converges over many miners/churn. A single
BitAxe = 100/0 forever; one S19 + two BitAxes ≈ 99/1 by hashrate regardless of
connection split. So the allocator weights the diffusion accumulator by measured
per-miner hashrate (from difficulty-weighted accepted shares, and/or ckpmsg
per-worker stats), and treats **natural reconnects** (ASICs reconnect often) as
free re-allocation points to correct drift. cgminer's work-based quota semantics
are the reference model. The UI is honest about granularity when miner count is
tiny.

---

## 4. Allocation modes

### 4.1 `farm_split` (default) — for fleets, churn-free
Whole miner pinned to one pool for its session. Zero cross-pool share loss,
shares route structurally. Best for any farm with ≥2 miners of comparable
hashrate. This is the solid, always-correct default.

### 4.2 `time_slice` (secondary) — for a single miner, capability-gated
For **one** miner that must itself split across both pools. This is both the
hardest problem and the core value for the single-BitAxe owner, so it is designed
honestly rather than over-promised:

- A miner subscribes **once** and gets one `extranonce1`. Naively gating pool B's
  jobs through a pool-A subscription produces structurally invalid shares
  (pool-B coinbase built with pool-A extranonce1). This is why the ESP32 project
  did it in firmware, where it owns job creation.
- **Correct proxy approach:** at subscribe, detect whether the miner supports
  `mining.set_extranonce` / `mining.extranonce.subscribe`. If yes, at each slice
  boundary send `set_extranonce` + `set_difficulty` + `notify(clean_jobs=true)`
  for the incoming pool, and route each `submit` to the pool that issued that job
  (job-tagging). If the miner lacks the capability, **refuse time_slice mode
  gracefully** and tell the user to use farm_split (or a second miner).
- Boundaries cost stale shares, so `interval_ms` is **minute-scale**, not the
  firmware's sub-second Bresenham. We explicitly do **not** promise NerdQAxe
  parity. Implemented as a separate module so its complexity can't destabilize
  farm_split.

---

## 5. Failover — two layers + donation

- **Intra-pool (endpoint level):** each ckproxy natively fails over between a
  pool's primary and `failover` upstream. Free, battle-tested.
- **Pool level → donate (`pool_failover.c` reused):** `health` feeds
  `PF_EV_CONNECTED/DISCONNECTED` per pool from ckpmsg + job-flow liveness. When a
  pool reads **DOWN** (both its endpoints dead), the allocator stops assigning to
  it, **evicts** its pinned miners so they reconnect onto the survivor, and
  donates all hashrate to the other pool. On recovery, allocations drift back on
  natural reconnects.
- **Reconnect-storm guard:** per-source-IP backoff before re-accepting, so a
  farm of 50 ASICs reconnecting at once doesn't melt down (a documented failure
  of older proxies).

---

## 6. Configuration & end-user management

### 6.1 Config file (ckpool-style JSON), mounted as a volume
```json
{
  "downstream":  { "stratum_port": 3333, "web_port": 8080 },
  "mode":        "farm_split",
  "ratio_a":     70,
  "interval_ms": 180000,
  "web_password": "",
  "pools": [
    { "url": "poolA.example:3333", "user": "walletA.worker", "pass": "x",
      "ckproxy_mode": "userproxy",
      "failover": { "url": "backupA:3333", "user": "walletA.worker", "pass": "x" } },
    { "url": "poolB.example:3333", "user": "walletB.worker", "pass": "x",
      "ckproxy_mode": "userproxy" }
  ]
}
```

### 6.2 How end-users manage it in a local Docker container (ranked)

This is a priority. The container exposes two ports: `3333` (miners point here)
and `8080` (management). Three tiers, all shipped:

1. **Embedded web dashboard (primary, make-or-break).** The splitter itself
   serves one static HTML page on `:8080` — no second container, no database.
   - **Status:** per-pool up/down/failover, live *actual vs target* ratio,
     per-pool difficulty-weighted accepted/rejected, and a per-miner list showing
     which pool each miner is on right now.
   - **Settings form:** pool URLs/users/passwords, a ratio slider, mode select.
     Saving writes the JSON config and **hot-applies** it (new allocations only —
     existing miners are not dropped). Modeled on DATUM Gateway's dashboard and
     AxeOS — UIs this audience already knows.
2. **Config file on a mounted volume + hot reload.** Edit `config.json` on the
   host; the splitter watches the file (or `SIGHUP`) and applies ratio/mode/pool
   changes **without disconnecting miners**. A 5-line `docker-compose.yml` with a
   `.env` (`POOL_A_URL`, `POOL_B_URL`, `RATIO_A`, …) is the copy-paste quickstart
   for non-developers.
3. **REST + Prometheus.** `/api/status`, `/api/config`, and `/metrics` for
   Grafana / Home Assistant / bitaxe-sentry-style monitors. Cheap once #1 exists.

Later distribution: **Umbrel / StartOS one-click app** (public-pool and DATUM
both ship this way to exactly our users) with an Umbrel home-screen widget.

Explicitly **not** built: CLI-only management, Postgres-backed dashboards,
multi-user auth, Telegram bots — over-engineering for a LAN tool. One optional
dashboard password is enough.

---

## 7. Testing (three tiers)

- **T1 — gcc unit tests** (mirrors the ESP32 `dual_pool/test_host`; runs even on
  native Windows because it's socket-free): allocator ratio convergence
  (incl. hashrate-weighted), failover state machine, clamp, share-accounting line
  parser (difficulty-weighted), config parser.
- **T2 — host integration** (WSL/Linux; no Docker/ckproxy needed): the real
  `splitter` binary with its two upstreams pointed **directly at two fake Stratum
  servers** + a fake-miner driver. Asserts realized A/B split matches `ratio_a`,
  shares route to the correct pool, and killing a fake pool triggers eviction +
  donation. **This is the "two fake upstream" harness the requirements ask for.**
- **T3 — docker-compose full stack**: splitter + two **real** stock ckproxy + two
  fake upstreams + fake miner. Validates the actual ckproxy wiring, userproxy
  identity passthrough, extranonce subdivision, and the Dockerfile.

Pure-logic modules stay socket-free so T1 runs anywhere; socket code (relay,
health, webui) is Linux/epoll and runs under WSL/Docker.

---

## 8. Docker & licensing

- **Dockerfile** multi-stage: stage 1 builds ckpool from `ckolivas/ckpool`
  (autogen/configure/make → `ckproxy`); stage 2 builds the `splitter` (links
  `libjansson`); runtime image runs `splitter` under a real init (`tini`), which
  spawns and supervises the two ckproxy. Config mounted as a volume; ports 3333 +
  8080 exposed. Build deps modeled on community ckpool Docker images
  (build-essential, yasm, libjansson-dev, pkg-config, autoconf, libtool).
- **Licensing:** ckpool is **GPLv3**, so this derivative is GPLv3. The four reused
  `dual_pool` files (`pool_scheduler`, `pool_failover`, `dual_clamp`, and the
  reentrant-recv reference) are copied into `src/dual_pool/` with **GPLv3**
  headers noting provenance (the author's ESP32 project, relicensed for this
  project). `LICENSE` = full GPLv3; README flags the obligation.

---

## 9. Known risks / gotchas to design against

1. **Extranonce hell** — `mining.set_extranonce`/`extranonce.subscribe` support is
   spotty in stock firmware; extranonce-change rebinding was a recurring crash/
   "Invalid Extranonce2 size" source in older proxies. Farm-split sidesteps it
   (session pinned); time_slice must capability-gate on it.
2. **Vardiff mismatch across pools** — never count raw accepted shares across two
   vardiff domains; always weight by per-session difficulty.
3. **Version-rolling renegotiation** — some firmware sends `mining.configure` only
   once at connect; on reassignment force a clean TCP disconnect so the miner
   re-handshakes with the new pool's mask.
4. **Reconnect storms / redirect-to-dead-pool** — health-gate before accepting;
   back off per source IP.
5. **Extranonce space subdivision** — upstream pools granting small `enonce2`
   (some NiceHash-style endpoints) limit downstream client count; detect and warn
   in the UI rather than fail silently.
6. **Small-fleet reality** — with N=1..3 miners, connection-count split is
   meaningless; hashrate-weight it and be honest in the UI.

---

## 10. Milestone → module mapping

1. Build ckpool; run one stock `ckproxy` against one upstream; confirm a
   (real or simulated) miner mines through it end-to-end. (`ckproxy_config`)
2. Document the generator↔stratifier↔connector seams and the relay/accounting
   points. (design note in repo)
3. Config parse + emit two ckproxy configs + supervise both. (`config`,
   `ckproxy_config`)
4. Farm-split hashrate-weighted allocation + per-pool share accounting.
   (`alloc`, `relay`, `share_accounting`)
5. Pool-level failover + eviction + donation + recovery. (`health`,
   `pool_failover`)
6. Optional single-miner time-slice mode (capability-gated). (`time_slice`)
7. Dockerize + web UI + README + config example + T2/T3 harness. (`webui`,
   Docker, tests)
```
