# Roadmap

What's built today (see [PLAN.md](../PLAN.md) for detail): hashrate-weighted
farm-split, single-miner time-slice, per-pool + pool-level failover with
donation/eviction/recovery, per-pool difficulty-weighted share accounting, a live
branded dashboard with hot-apply config, Prometheus `/metrics`, optional password,
`.env`/config-file/env-var setup, and a Dockerized ckpool build. All covered by
unit + integration tests.

Below is what could come next, focused on how a home miner competes
with centralized hashrate. Tags: **[quick-win]** small, **[medium]** moderate,
**[ambitious]** large. Community PRs welcome.

## Decentralization & protocol
- **[quick-win] Sovereign preset, documented end-to-end** — a one-command compose
  that stands up Bitcoin node + [DATUM Gateway](https://github.com/OCEAN-xyz/datum_gateway)
  (OCEAN/TIDES) + [public-pool](https://github.com/benjamin-wilson/public-pool)
  solo + Dual-Pool Proxy. (Preset config shipped; full compose stack is TODO.)
- **[medium] Stratum V2 upstream via the [SRI translator](https://github.com/stratum-mining/stratum) sidecar** —
  let an SV2 **Job Declaration** pool be one of the two upstreams while miners stay
  on stock V1. Timely: the first production JD block was mined June 2026.
- **[ambitious] Native SV2 client** once multiple JD pools are live.
- **[quick-win] Track [Braidpool](https://github.com/braidpool/braidpool)** — not
  usable yet; revisit as it matures.

## Miner's edge features
- **[quick-win] Built-in split calculator** in the dashboard — given hashrate,
  network difficulty, and ratio, show steady sats/day, solo odds/year, and
  variance bands (simple Poisson math). Nobody ships this.
- **[quick-win] "Trust but verify" panel** — Dual-Pool Proxy-counted shares vs
  pool-reported hashrate/shares, with drift alerts.
- **[medium] Fee-aware ratio boost (opt-in, capped)** — raise the solo slice when
  projected next-block fees exceed a threshold (mempool.space API). Prize-size
  based, explicitly **not** "luck" based.
- **[medium] Alerting hooks** — webhook / Telegram / ntfy on pool-down, failover,
  stale-rate spikes, device offline, and a **BLOCK FOUND** watchtower.

## Ops & monitoring
- **[medium] [pyasic](https://github.com/UpstreamData/pyasic) integration** — LAN
  miner discovery, per-device temp/power/**J/TH** next to per-device shares,
  one-click repoint to Dual-Pool Proxy.
- **[quick-win] Ship the Grafana dashboard** (done: `grafana/dualpool-dashboard.json`)
  and a measured overhead/stale-rate benchmark for launch credibility.
- **[medium] Efficiency advisor** — combine power + shares into fleet J/TH and
  sats/kWh, linking out to BitAxe autotune/benchmark tools.

## Correctness & performance
- **[medium] Version-rolling mask negotiation audit** across the two upstreams
  (intersect masks, re-negotiate on failover) + regression test.
- **[medium] Fractional-hashrate-aware vardiff** so a small miner on the minority
  leg still gets a sane difficulty.
- **[medium] Deeper pool liveness via `ckpmsg`** — query each ckproxy's upstream
  connection/work state directly (current probe uses TCP reachability + job-flow
  staleness).

## Packaging & distribution
- **[medium] Umbrel + Start9 packages** — where this audience already runs
  public-pool and DATUM.
- **[quick-win] Guided ckproxy restart** from the dashboard after a pool or
  credential edit (the edit is already persisted to `config.json`; today it needs a
  manual `docker compose restart`).

---

*See [STRATEGIES.md](STRATEGIES.md) for the
reasoning and citations. Priorities may shift with community input.*
