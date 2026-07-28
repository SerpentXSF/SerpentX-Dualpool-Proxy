<p align="center">
  <img src="web/serpentx-emblem.png" alt="Dual-Pool Proxy" height="160">
</p>

<h1 align="center">Dual-Pool Proxy — Dual-Pool Stratum Proxy</h1>

<p align="center">
  <b>Mine any stock-firmware Stratum miner to two SHA-256 pools at once, in a ratio you choose — without reflashing anything.</b>
</p>

<p align="center">
  <i>Version X.3 · License: GPLv3 · Built on <a href="https://github.com/ckolivas/ckpool">ckpool/ckproxy</a> · Linux/Docker</i>
</p>

---

Point your **BitAxe, NerdAxe, Antminer, HammerMiner, Avalon, or Whatsminer** at
Dual-Pool Proxy instead of at a pool. It presents itself as **one** pool, then splits
your hashrate across **two** real pools by a configurable ratio, with per-pool
failover. No firmware changes, no warranty risk, one box for your whole fleet.

**Typical use:** run *70% steady-payout pool + 30% solo/lottery* across every
miner you own — including closed-firmware machines that could never do this
themselves — to decentralize away from big centralized pools.

> **Status:** **X.3** — verified end-to-end with real hardware (BitAxe, NerdAxe,
> NerdQAxe, HammerMiner) against live pools: forwarded shares are accepted upstream
> and the proxy worker registers at the pool with full hashrate. See
> [CHANGELOG.md](CHANGELOG.md) for releases and [docs/ROADMAP.md](docs/ROADMAP.md)
> for what's next.

## Features

- **Simultaneous ratio split** across two pools — each miner is pinned to one pool
  to hit your target ratio (allocation is by miner count today; hashrate-weighting
  for mixed fleets is on the roadmap).
- **Farm-split** (churn-free, for 2+ miners) and **time-slice** (a single miner
  across both pools, works with any firmware).
- **Per-pool failover** + pool-level **donation, eviction, and recovery** — a dead
  pool's hashrate moves to the survivor and drifts back on recovery.
- **Correct share routing by construction** — every share goes to the pool that
  issued the job; per-pool **difficulty-weighted accounting**.
- **Live web dashboard** — pool status, actual-vs-target split, per-pool shares,
  per-miner list, and a settings form that **hot-applies** (no miner drops).
- **Prometheus `/metrics`** + a bundled Grafana dashboard.
- **Dockerized**, configured by `.env`, a `config.json`, or the dashboard.
- Built on **near-stock ckpool** — battle-tested Stratum, vardiff, ASICBoost. We
  apply only a few minimal upstream bug-fix patches (see
  [docker/patches/](docker/patches/README.md)); ckpool is otherwise unchanged.

## Quickstart

> Requires Docker (Linux host, Raspberry Pi, WSL2, or Docker Desktop).

```bash
git clone https://github.com/SerpentXSF/SerpentX-Dualpool-Proxy.git
cd SerpentX-Dualpool-Proxy
cp .env.example .env          # edit your two pools + ratio
docker compose up -d --build
```

Then point each miner at `stratum+tcp://<host-ip>:3333` and open the dashboard at
`http://<host-ip>:8080`. Full walkthrough: **[docs/GETTING_STARTED.md](docs/GETTING_STARTED.md)**.

Minimal `.env`:

```ini
POOL_A_URL=stratum.example.com:3333
POOL_A_USER=bc1qYourAddressA.worker1
POOL_B_URL=solo.ckpool.org:3333
POOL_B_USER=bc1qYourAddressB.worker1
RATIO_A=70
```

## How it works

```
  your miners ──► :3333 ──┐
  (point here)            │   ┌──► ckproxy A ──► Pool A (+ failover)
                          ├──►│ splitter
  you ─────────► :8080 ───┘   └──► ckproxy B ──► Pool B (+ failover)
                              (assigns each miner to a pool to hit your ratio;
                               serves the live dashboard + /api + /metrics)
```

One container runs a small router (**splitter**) plus **two near-stock
[ckproxy](https://github.com/ckolivas/ckpool) instances**, one per pool. Because a
miner is relayed end-to-end to exactly one ckproxy for its session, ckproxy
handles all the hard Stratum work and shares are **structurally** routed to the
right pool. The splitter owns allocation, failover/donation, accounting, and the UI.

- **Farm-split (default):** each miner assigned to one pool to hit your target
  ratio, pinned for its session. Zero switching loss.
- **Time-slice (single miner):** the proxy recycles the miner's connection each
  interval so it reconnects onto the next pool. Any miner; minute-scale churn.

## Get a real edge

Dual-Pool Proxy isn't just a splitter — it's a tool for the home miner to compete on
**sovereignty, variance shaping, fee-sharing pool choice, and visibility**.
The **sovereign preset** ([`config.sovereign.example.json`](config.sovereign.example.json))
runs *OCEAN via a local DATUM Gateway (your own block templates, TIDES payouts) +
a self-hosted solo pool* — the whole decentralization pitch in one box. Read the
honest playbook (with the math, and what's hype): **[docs/STRATEGIES.md](docs/STRATEGIES.md)**.

## Documentation

| Doc | What's in it |
|---|---|
| [Getting Started](docs/GETTING_STARTED.md) | Install, run, point miners, verify |
| [Configuration & Optimization](docs/CONFIGURATION.md) | Every option; how to tune the split, modes, failover, auth |
| [Strategies / Edge](docs/STRATEGIES.md) | Decentralization, variance math, payouts, what's hype |
| [Roadmap](docs/ROADMAP.md) | Planned integrations (SV2, pyasic, alerting, …) |
| [Design spec](docs/design/architecture.md) | Architecture & rationale |

## Why not just HAProxy?

Weighted TCP balancing across two ckproxy backends gets you *most* of farm-split
for free — and it's a good sanity check. Dual-Pool Proxy exists for the things a
dumb TCP balancer can't do: **correct-by-construction share routing**, **per-pool
share accounting**, **pool donation/recovery** (a dead pool's hashrate moves to the
survivor and drifts back), and **the dashboard**. If those don't matter to you,
HAProxy is genuinely fine.

## Building / testing

Targets Linux; on Windows use WSL/Docker. See
[docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) and
[CONFIGURATION.md](docs/CONFIGURATION.md).

```bash
make -C test/host run                       # pure-logic unit tests (plain gcc)
docker build -f docker/Dockerfile.dev -t dualpool-dev .   # dev image (jansson, python, curl)
docker run --rm -v "$PWD:/build" dualpool-dev make -C test/host config            # config parser tests (jansson)
docker run --rm -v "$PWD:/build" dualpool-dev ./test/integration/run_t2.sh        # split + routing
#   run_failover.sh  run_webui.sh  run_timeslice.sh                               # more suites
bash test/integration/run_t3.sh             # full stack: splitter + 2 real ckproxy (Docker)
```

## License

**GPLv3.** Dual-Pool Proxy is a derivative of [ckpool](https://github.com/ckolivas/ckpool)
(GPLv3), so it must be GPLv3 too. The reused allocation/failover files under
`src/dual_pool/` originate from the author's ESP32 firmware project and are
relicensed to GPLv3 here (provenance noted in each file header). See
[LICENSE](LICENSE).

## Credits & acknowledgements

- [ckpool / ckproxy](https://github.com/ckolivas/ckpool) by Con Kolivas — the
  Stratum proxy engine this builds on.
- Prior art that proved the demand and the math: NerdQAxe firmware dual-pool,
  cgminer `--quota`, Braiins OS pool groups, LuxOS hashrate splitting.
- The decentralization ecosystem Dual-Pool Proxy plugs into:
  [OCEAN/DATUM](https://github.com/OCEAN-xyz/datum_gateway),
  [public-pool](https://github.com/benjamin-wilson/public-pool),
  [Stratum V2 / SRI](https://github.com/stratum-mining/stratum),
  [pyasic](https://github.com/UpstreamData/pyasic),
  [ESP-Miner / AxeOS](https://github.com/skot/ESP-Miner).

Contributions welcome — see [docs/ROADMAP.md](docs/ROADMAP.md).
