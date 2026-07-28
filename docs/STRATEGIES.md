# Strategies: Getting a Real Edge as a Home Miner

You will never out-hash Foundry or AntPool. But a home miner *can* win on four
things centralized farms can't easily take from you: **sovereignty**,
**variance shaping**, **fee-sharing pool choice**, and **visibility**. SerpentX
is built to deliver all four. This page is the honest playbook — including what's
real and what's hype.

> Not financial advice. Mining is variable and can be unprofitable. The math
> below is expected-value reasoning, not a promise of returns.

---

## The core idea: a barbell, not a bet

Splitting your hashrate across a **steady-payout pool** and a **solo/lottery
leg** is a *barbell*. Its expected value is essentially the same as mining 100%
to the steady pool (minus small fee differences) — what the split buys you is a
**bounded, self-chosen exposure to the right tail** (finding a whole block)
without giving up your regular income.

- A 70/30 split ≈ 70% steady cashflow + a 30%-sized lottery ticket.
- Block-finding is *memoryless*: "pool luck" has **zero** predictive power.
  Anything marketed as "luck-based switching" is the gambler's fallacy in a config
  file. SerpentX deliberately does **simultaneous** splitting, not switching, so
  it's immune to that trap and to PPLNS/TIDES window games.

Solo odds are real but long: a ~1.2 TH/s BitAxe is roughly a 1-in-tens-of-
thousands shot per year at current network hashrate — yet BitAxes *have* found
blocks. Size your solo leg like a lottery ticket you can afford, not a plan.

---

## 1. Sovereignty: use your own node's block templates

The single biggest edge is building your **own** block templates on your **own**
node, instead of renting work from a distant pool. Two mature, MIT-licensed ways,
both of which SerpentX can point a pool at with **zero extra config** (they're
local Stratum V1 endpoints):

- **[DATUM Gateway](https://github.com/OCEAN-xyz/datum_gateway) (OCEAN / TIDES).**
  Runs beside your Bitcoin node, builds templates locally, and gives you **TIDES**
  pooled payouts *while you choose your own transactions* — non-custodial, paid
  direct from the coinbase. Default stratum port `23334`. See
  [OCEAN's DATUM docs](https://ocean.xyz/docs/datum).
- **[public-pool](https://github.com/benjamin-wilson/public-pool).** A
  self-hostable **solo** pool against your own node. Great as the lottery leg.

**The "sovereign preset"** ([`config.sovereign.example.json`](../config.sovereign.example.json)):
Pool A = OCEAN via local DATUM Gateway (steady TIDES, your templates), Pool B =
self-hosted public-pool (solo lottery). Both non-custodial, both using your node.
That is the whole decentralization pitch, running in one box.

**Node tuning notes:** Bitcoin Core ≥28 has **full-RBF on by default**, so your
local mempool already reflects the highest-fee replacements — a small but real
fee edge over a stale template, plus much lower new-block latency on a LAN. Choose
[Core or Knots](https://knowingbitcoin.com/bitcoin-core-vs-knots-2026/) as a
**values** decision, not a profit hack — relay filtering can slightly *reduce*
your fee revenue and is not a consensus difference.

---

## 2. Fee capture is a pool-choice decision, not a solo trick

Out-of-band fee programs — [mempool.space Accelerator](https://mempool.space/accelerator),
[MARA Slipstream](https://slipstream.mara.com) — pay extra fees to *participating
pools*. A lone miner can't join them directly. The realistic way to capture that
value is to **mine on a pool that participates** (OCEAN does, and TIDES passes
real fees through). So: pick your steady leg partly on **payout scheme + fee
policy**, not just headline fee %.

Payout schemes in one line each:
- **FPPS** — pool eats all variance, ~2–4% fee, pays *estimated* fees (you trust
  the estimate). Best for very small hashrate that needs steady income.
- **PPLNS** — lower fee, real fees passed through, hop-resistant, you bear variance.
- **TIDES (OCEAN)** — real block rewards incl. real fees, direct from coinbase,
  non-custodial, hop-resistant.
- **Solo** — pure lottery.

A reasonable default: **small fleet →** FPPS (steady) + solo (lottery);
**larger/self-hosting →** TIDES + solo.

---

## 3. Efficiency & share correctness (protect the revenue you have)

- **Version rolling / ASICBoost** (`mining.configure`) must pass through
  correctly — a wrong version mask silently costs efficiency. SerpentX pins each
  miner to one pool per session so its ckproxy negotiates the mask cleanly.
- **Low latency** matters more than "better templates": a solo/DATUM pool on your
  **LAN** delivers new-block work in ~ms vs 50–150ms to a distant pool, cutting
  stale shares. Keep your lottery/solo leg local when you can.
- **Vardiff:** with a split, each pool sees only part of your hashrate — the
  underlying ckproxy manages per-connection difficulty; watch reject/stale on the
  dashboard and prefer pools with sane vardiff.

---

## 4. Visibility centralized pools don't give you

- **Per-pool share accounting** is built in — you can compare SerpentX's own
  accepted/rejected counts against what each pool *reports* ("trust but verify").
- **Prometheus `/metrics`** + the bundled
  [Grafana dashboard](../grafana/serpentx-dashboard.json): watch the realized
  split, accept/reject rates, and pool up/down over time.
- Pair with device tooling — [pyasic](https://github.com/UpstreamData/pyasic)
  (LAN discovery, J/TH), [AxeOS/ESP-Miner](https://github.com/skot/ESP-Miner)
  APIs and BitAxe autotune/benchmark tools — to see efficiency next to earnings.

---

## What's hype (so you don't chase it)

- **"Luck-based" or "smart" pool switching** — gambler's fallacy; block-finding is
  memoryless.
- **Secret better templates** — the fee delta between a well-run node's template
  and a big pool's is within noise. The edge is *sovereignty + latency*, not alpha.
- **NiceHash-vs-pool arbitrage** for a home miner — renting doesn't change EV, it
  just buys variance (same as your split). A [renter did win a block with ~$75 of
  rented hashrate](https://cointelegraph.com/news/nicehash-untagged-bitcoin-blocks-solo-miner-myth)
  — that's a lottery ticket, not arbitrage.
- **Hashprice derivatives / hedging** — real, but
  [institutional and closed](https://luxor.tech/derivatives). For the home miner,
  choosing an FPPS pool *is* the hedge.

---

## Suggested setups

| Goal | Pool A | Pool B | Ratio |
|---|---|---|---|
| Max sovereignty | OCEAN via local DATUM (TIDES) | self-hosted public-pool (solo) | 80/20 |
| Steady + lottery (simple) | an FPPS/PPLNS pool | solo.ckpool.org | 70/30 |
| All-in decentralized | OCEAN/DATUM | OCEAN/DATUM backup or public-pool | 100/0 + failover |
| Single BitAxe, split over time | steady pool | solo pool | `time_slice`, 70/30, 3-min interval |

See [ROADMAP.md](ROADMAP.md) for planned integrations (SV2 Job Declaration,
pyasic discovery, alerting/BLOCK-FOUND watchtower, a built-in split calculator).

*Verify current pool terms and software
before relying on them.*
