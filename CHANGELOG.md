# Changelog

All notable changes to Dual-Pool Proxy are recorded here.

**Versioning:** the major is the literal **X** (the SerpentX house line). The number bumps by
**1** for a normal release and by **0.5** for smaller changes:

```
X.1  →  X.1.5 (small)  →  X.2  →  X.2.5  →  X.3 ...
```

The version is the single source of truth in [`VERSION`](VERSION) /
[`src/version.h`](src/version.h) and is shown in the dashboard footer,
`GET /api/status` (`version`), and `/metrics` (`dualpool_build_info`).

---

## Unreleased

- **New mode — `hashrate_split` (EXPERIMENTAL):** one miner mines **both pools at
  once**, the way some dual-pool BitAxe/NerdQAxe firmware works, but without
  reflashing — the proxy multiplexes a single downstream connection across both
  upstreams and time-slices it by share count (`target_shares`, clamped between
  `min_slice_s`/`max_slice_s`). Miners that honour `mining.set_extranonce` get a
  smooth in-place pool swap; others fall back to a brief reconnect per slice, and
  `assume_extranonce` opts miners that honour it without advertising it into the
  smooth path. Selectable from the **dashboard** or `config.json`, with a full
  config-API round-trip; like other mode/knob changes, it is restart-applied.

  Each pool keeps its own difficulty (a share is credited at the difficulty its
  pool assigned, so presenting one merged difficulty would silently discard work),
  ASICBoost version-rolling is negotiated per pool and reconciled to the
  intersection, submits are routed to the pool whose job they were found on with a
  stale-grace window, pool acknowledgements are relayed back, and split
  connections now report their shares to the dashboard and `/metrics`. The proxy
  also relays a miner's `mining.suggest_difficulty` to both pools and, if the
  miner sends none, suggests one itself from the measured hashrate so a pool that
  opens at a very high difficulty cannot starve the miner.

  **Validated:** a single ~1.5 TH/s miner against solo.ckpool + Kryptex for 20+
  hours — 0.08% rejects, work split within 0.4 points of `ratio_a`, credited
  hashrate ≈ the miner's real rate, multi-hour sessions with no reconnects.
  **Known limits:** a large miner against a pool opening at a very high difficulty
  may need a warm-up before it bootstraps; every fresh session pays a small
  difficulty-settling burst; and the 90s no-work auto-donation is reduced for
  pools serving only split connections (mitigated by the mux's single-pool
  degrade). `farm_split` and `time_slice` remain the supported modes — see
  [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md#hashrate_split-mode).

## X.3.5 — Hardening, robustness & dashboard fixes

Fixes on top of X.3, from a full code audit plus real-world dashboard use.

- **Dashboard — edits no longer revert:** the settings form used to reload every 5s
  and wipe whatever you were typing before you could hit Save. It now pauses syncing
  a field while you're editing it and resumes after Save or Reload, so you can edit
  pools/users from the dashboard *or* the config file, whichever you prefer.
- **Dashboard — fallback pool editing:** you can now set each pool's optional
  **fallback pool** (backup URL/user) from the dashboard, not just the config file.
- **Supervisor:** a died ckproxy is now reaped and respawned without blocking the
  other pool — a long backoff on one pool no longer delays recovering the other.
- **Connection teardown:** a closing miner's slot is freed before its socket is
  closed, so a just-freed file descriptor can't be shut down out from under a new
  connection during pool eviction.
- **Config save:** the config file is now written from a locked, consistent
  snapshot.
- **Entrypoint:** environment values are JSON-escaped when generating `config.json`,
  so a password containing `"` or `\` no longer produces an invalid config; per-pool
  `startdiff`/`mindiff` are validated as integers.
- **Web server:** a null-allocation guard on the accept path.
- **Tests:** the config-parser test suite now has a `make config` target and is run
  in the documented dev-image flow.

## X.3 — Proxied miners now mine valid shares on real pools

This release fixes the core issue that made proxied miners' shares get rejected or
go uncredited at upstream pools while the local dashboard looked healthy. With real
hardware (BitAxe, NerdAxe, NerdQAxe, HammerMiner) pointed at the proxy, forwarded
shares are now accepted upstream and the proxy worker registers at the pool with
its full hashrate.

The root causes were all in **stock ckpool's proxy mode**. We now pin **ckpool
v1.2.0** and apply minimal, build-time bug-fix patches (documented in
[`docker/patches/`](docker/patches/README.md)); ckpool is otherwise unmodified:

- **ASICBoost version-rolling was dropped on forwarded shares** — the main bug.
  Miners roll the block version for efficiency and find shares with the *rolled*
  version, but the proxy forwarded `mining.submit` without the version bits, so the
  pool rehashed with the original version and rejected every share as *"Above
  target"* / invalid. The version bits are now carried through and forwarded.
  (`0004`)
- **Proxied miners weren't driven to the pool's difficulty.** They stayed pinned to
  their own low suggested difficulty, so almost none of their shares met the pool's
  requirement and the worker looked dead at the pool. Clients now track the
  pool-dictated difficulty in both directions. (`0003`)
- **A crash (SIGABRT) under share load** — a double-free in ckpool's proxy receive
  path — is fixed. (`0002`)

New feature: **per-pool `startdiff` / `mindiff`** in the pool config, for pools that
enforce a difficulty floor.

Verified with real miners against solo.ckpool and Kryptex: forwarded shares
accepted 100% (packet-capture-confirmed), worker registered upstream with full
hashrate, zero crashes over sustained runs.

## X.2.5 — Renamed the product to "Dual-Pool Proxy"

The tool is now named **Dual-Pool Proxy**; **SerpentX** is the maker/house brand
(the name behind future tools), not the product name. The SerpentX crest stays on
the dashboard as the maker's mark.

**Breaking name changes — update any automation:**
- Container / compose / image: `serpentx` → `dualpool-proxy`
- Binary: `serpentx-splitter` → `dualpool-splitter`
- Env vars: `SERPENTX_*` → `DUALPOOL_*`
- Prometheus metrics: `serpentx_*` → `dualpool_*`
- Runtime paths: `/tmp/serpentx` → `/tmp/dualpool`, `/usr/local/share/serpentx` → `/usr/local/share/dualpool`
- Dashboard auth header: `X-SerpentX-Key` → `X-DualPool-Key`

No functional behavior changed. The GitHub repo name is unchanged.

## X.2 — Accurate miner counts + churn-resilient supervision

Fixes surfaced by real-world testing:

- **Dashboard/metrics now show currently-connected miners**, not the cumulative
  routing count. Previously "50 miners" meant "50 routings since start" (which
  balloons when miners reconnect); it now reflects live connections and drops to
  zero when miners leave. The actual-vs-target split is computed from current
  connections. New metric `dualpool_miners_connected_pool{pool}`; per-pool
  `connected` and `routed` fields in `/api/status`.
- **Crash-loop resilience:** if a ckproxy keeps dying (e.g. the pool rejects the
  login), it now respawns with **exponential backoff** (1→32s) instead of hammering
  every second, and the pool is flagged **DOWN so miners are donated to the healthy
  pool** instead of churning on the broken one. It auto-recovers once the ckproxy
  stays up. The log points you at `<sockdir>/console.log` for the upstream error.

## X.1.5 — Full config editing from the dashboard

- The dashboard settings form now edits the **whole config** — pool URLs,
  usernames, and passwords included — and writes it back to `config.json`. Ratio
  and mode still apply instantly; pool/credential changes are saved and take
  effect after a restart. (Previously the form only saved ratio/mode; pool fields
  were ignored.)
- `POST /api/config` accepts `pools` and `interval_ms` in addition to
  `ratio_a`/`mode`. A blank password field keeps the current password.
- Docs clarified: `config/config.json` is the live source of truth (edit via the
  dashboard **or** the file); `.env` only seeds it on first run.

## X.1 — Initial release

First public release. A standalone Linux/Docker Stratum proxy that splits any
stock-firmware SHA-256 miner across two pools at once, built on unmodified
[ckpool](https://github.com/ckolivas/ckpool).

**Core**
- **Farm-split** allocation — each miner pinned to one pool to approach the target
  ratio (by miner count; hashrate-weighting is planned).
- Single-miner **time-slice** mode (connection-recycling; works with any miner).
- Per-pool **failover** plus pool-level **donation, eviction, and recovery**.
- Correct share routing by construction; per-pool **difficulty-weighted
  accounting**.

**Interface**
- Live **web dashboard** (branded) with actual-vs-target split, per-pool shares,
  per-miner list, and a hot-applying settings form.
- REST `GET /api/status`, `POST /api/config`; Prometheus `/metrics` + bundled
  Grafana dashboard; optional dashboard password.
- Config via `.env`, `config.json`, or the dashboard; `SIGHUP` hot-reload.

**Packaging & docs**
- Multi-stage Dockerfile (builds ckpool), `docker-compose.yml`, env-var
  entrypoint, sovereign preset (DATUM/OCEAN + solo).
- Getting-started, configuration, strategies, and roadmap docs.
- Unit tests (allocator, share accounting, failover/health, config) + integration
  suites (split/routing, failover, web/metrics, time-slice) + full-stack T3.
