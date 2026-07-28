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

## X.3.5 — Hardening & robustness

Small, non-behavioral fixes on top of X.3, from a full code audit. No config or
usage changes.

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
