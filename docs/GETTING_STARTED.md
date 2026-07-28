# Getting Started with Dual-Pool Proxy

Dual-Pool Proxy makes **any** Stratum SHA-256 miner mine to **two pools at once** in a
ratio you pick, with per-pool failover — no firmware changes. This guide gets you
running in a few minutes.

---

## 1. Prerequisites

- A Linux host with **Docker** (native Linux, a Raspberry Pi, WSL2, or Docker
  Desktop all work).
- Your two pools' details: `host:port`, and a username for each (for most solo /
  DATUM / public pools the username is your **BTC address**, often
  `address.workername`).
- Your miners (BitAxe, NerdAxe, Antminer, HammerMiner, Avalon, Whatsminer, …) on
  the same LAN.

No Bitcoin node is required for the basic setup. (For the fully self-sovereign
setup, see [STRATEGIES.md](STRATEGIES.md).)

---

## 2. Install & run (Docker Compose — recommended)

```bash
git clone https://github.com/SerpentXSF/SerpentX-Dualpool-Proxy.git
cd SerpentX-Dualpool-Proxy
cp .env.example .env          # edit your two pools + ratio
docker compose up -d --build  # builds ckpool + Dual-Pool Proxy, starts everything
```

Edit `.env` with your pools before (or after) starting — at minimum:

```ini
POOL_A_URL=stratum.example.com:3333
POOL_A_USER=bc1qYourAddressA.worker1
POOL_B_URL=solo.ckpool.org:3333
POOL_B_USER=bc1qYourAddressB.worker1
RATIO_A=70                    # 70% of hashrate to Pool A, 30% to Pool B
```

The container exposes two ports:

| Port | Purpose |
|------|---------|
| `3333` | Point your miners here (`stratum+tcp://<host-ip>:3333`) |
| `8080` | Web dashboard (`http://<host-ip>:8080`) |

> Prefer a config file over `.env`? Drop a `config.json` in `./config/` (see
> [`config.example.json`](../config.example.json) and
> [CONFIGURATION.md](CONFIGURATION.md)). A mounted `config.json` takes precedence
> over `.env`.

---

## 3. Point your miners at Dual-Pool Proxy

On each miner's web UI (AxeOS, Antminer UI, etc.), set the **primary pool** to:

```
stratum+tcp://<host-ip>:3333
```

Use your normal pool username/worker name and password (`x` is fine). That's it —
Dual-Pool Proxy presents itself as one pool and splits behind the scenes.

**Tip:** you don't need to change usernames per miner. In the default `userproxy`
mode, each miner's worker name is preserved upstream so you still see it on the
pool's own dashboard.

---

## 4. Verify on the dashboard

Open `http://<host-ip>:8080`. You should see:

- **Both pools** as `online`.
- The **actual-vs-target split** bar filling in as miners connect.
- Your **miners listed** with which pool each is on.
- **Accepted/rejected** shares per pool.

If a pool shows `off`, check its `host:port`/credentials in `.env` or
`config.json`. Changes to the ratio/mode apply live from the dashboard's
**Settings** form (no miner disconnects).

---

## 5. Which mode? (farm-split vs time-slice)

- **`farm_split` (default)** — best for **2+ miners**. Each miner is assigned to
  one pool (weighted by its hashrate) and stays there. Zero switching loss.
- **`time_slice`** — for a **single miner** that must itself split across both
  pools. The miner is recycled onto the next pool every `interval_ms`. Works with
  any miner; costs a few stale shares per switch (keep the interval in minutes).

Set this via `MODE=` in `.env`, the `mode` field in `config.json`, or the
dashboard.

---

## 6. Common commands

```bash
docker compose logs -f dualpool-proxy     # watch logs
docker compose restart dualpool-proxy     # restart
docker compose down                 # stop
docker kill -s HUP dualpool-proxy         # hot-reload config.json without restart
```

---

## 7. Next steps

- **Tune your split and pools:** [CONFIGURATION.md](CONFIGURATION.md)
- **Get a real edge (decentralize, reduce variance, self-host):**
  [STRATEGIES.md](STRATEGIES.md)
- **Monitoring with Prometheus + Grafana:** import
  [`grafana/dualpool-dashboard.json`](../grafana/dualpool-dashboard.json); metrics
  are at `http://<host>:8080/metrics`.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Pool shows `off` | Wrong `host:port` or the pool is down. Check logs. |
| Miner won't connect | Confirm `stratum+tcp://<host>:3333` and that port 3333 is reachable on your LAN / firewall. |
| Dashboard needs a key | You set `WEB_PASSWORD`. Enter it when prompted (stored in your browser). |
| Split looks off with 1–2 miners | Expected — with few miners the split is coarse; it's weighted by hashrate, not miner count. See CONFIGURATION.md. |
| Build fails on `git clone ckpool` | Network/DNS during `docker build`. Retry; the ckpool clone needs internet. |
