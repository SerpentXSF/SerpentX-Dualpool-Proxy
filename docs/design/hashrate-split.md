# Hashrate Split mode — design

**Status:** design (approved) · **Target:** a future release (X.4)

## 1. Purpose

Let a **single miner with no dual-mining firmware** (HammerMiner, Antminer,
Whatsminer, any closed-firmware SHA-256 box) split its hashrate across **both**
pools at once — the way NerdQAxe/BitAxe *DualMiner* firmware does — without
reflashing. The miner time-slices between Pool A's work and Pool B's work; over a
cycle each pool receives its `ratio_a` share of the hashrate.

Works for multiple miners too (each splits independently), but the design is
optimized for the single-miner case.

### Non-goals (v1)
- Truly simultaneous dual-pool hashing (physically impossible on one ASIC — a chip
  hashes one job at a time; "split" is fast time-slicing).
- Fixing pools that reject un-negotiated version rolling (OCEAN — separate work).
- Stratum V2 upstreams.

## 2. The physics

An ASIC works one block header at a time, so a "hashrate split" is rapid, clean
**alternation**: mine Pool A's job for a slice, switch to Pool B's job for a slice,
repeat. Firmware does this at the chip level; here the proxy does it at the Stratum
level by controlling which pool's job the miner is currently given.

## 3. Architecture

Today the splitter **byte-relays** a miner end-to-end to exactly one ckproxy. That
path is unchanged for `farm_split`/`time_slice` miners.

For a `hashrate_split` miner the splitter becomes a small **Stratum-aware
multiplexer**:

```
                 ┌───────────────► ckproxy A ──► Pool A   (both stay subscribed,
   miner ──►  splitter (mux)       │              both receiving notify/diff)
   (:3333)      └───────────────► ckproxy B ──► Pool B
```

- The mux is a **downstream client to BOTH ckproxy A and B** — each gives it an
  extranonce1, difficulty, and a live `mining.notify` stream.
- The mux is the **Stratum server to the miner** — at any instant it presents the
  *active* pool's extranonce/difficulty/job.
- It tracks a per-miner **active pool** and a **job-id → pool** map for routing the
  miner's `mining.submit` back to the correct ckproxy.

This is additive: a new module (e.g. `splitmux.c`) invoked only for split-mode
connections; `relay.c` is untouched for the other modes.

## 4. The switch (per miner)

On each slice boundary, hand the miner from the active pool to the other:

1. Wait for the **target pool's next clean job** (`clean_jobs=true`), never switch
   mid-job.
2. Send the miner `mining.set_extranonce` (target pool's enonce1 / nonce2 size) →
   `mining.set_difficulty` (target diff) → `mining.notify` (the clean job).
3. Flip `active pool`; subsequent submits route to the target pool.

### Capability detection & fallback
At subscribe, detect whether the miner supports the extranonce extension
(`mining.extranonce.subscribe` acknowledged / honors `set_extranonce`):
- **Supported** → smooth swap as above (no reconnect).
- **Not supported** → fall back to the **reconnect-recycle** path (drop + reconnect
  the miner onto the next pool each slice), i.e. today's `time_slice` behavior, so
  the feature still works, just with a brief reconnect per switch.

## 5. Adaptive slice sizing

Each slice targets **~N accepted shares** (default **N = 10**) so slices are never
near-empty on high-difficulty pools nor spammy on easy ones:

```
slice_seconds ≈ N × pool_diff × 2^32 / miner_hashrate      (clamped to [10, 120] s)
```

- `miner_hashrate` = the mux's measured per-connection rate.
- `pool_diff` = the active pool's current difficulty.
- **`ratio_a` sets the long-run split**: over an A+B cycle, A's total slice time is
  weighted to `ratio_a` %, B's to `100 − ratio_a` %.
- Bootstrap before a hashrate estimate exists: start from a `startdiff`-style
  seed, converge after the first slice.

Advanced (config) knobs: `target_shares` (N), `min_slice_s`, `max_slice_s`.

## 6. Stale-share safety

- **Clean-job switching only** — a switch waits for the target pool's next
  `clean_jobs` notify, so the miner never mines a superseded template.
- **Job-routed submits with a grace window** — a submit whose job-id maps to the
  pool we *just left* is still forwarded to that pool for a short window (it's valid
  inside the pool's stale tolerance, ~tens of seconds); a submit for a job the pool
  has since superseded is dropped, not sent (avoids "stale" rejects inflating the
  error rate).
- Difficulty and extranonce are always the ones matching the job the share was found
  on (tracked per job-id).

## 7. Config & dashboard

- New mode: `"mode": "hashrate_split"`, and a third dropdown option
  "hashrate_split — split one miner's hashrate across both pools".
- The existing time-slice interval field becomes the split's advanced settings:
  target shares/slice (N) with min/max slice clamps.
- `ratio_a` reused as the split proportion.
- `time_slice` is **kept as-is** alongside the new mode (simple reconnect slicing).

## 8. Milestones (TDD)

1. **Stratum message model** — parse/emit the subset we need (subscribe, notify,
   set_difficulty, set_extranonce, submit, configure) with unit tests. Pure, no
   sockets.
2. **Slice scheduler** — adaptive sizing + ratio weighting + clean-job gating, as a
   pure module with tests (deterministic clock injected).
3. **Split mux (single upstream)** — mux talks to one ckproxy and relays a miner,
   proving the stratum-aware path works end-to-end (integration harness).
4. **Dual-upstream swap** — add the second ckproxy + `set_extranonce`/`set_difficulty`
   swap + job→pool routing + grace window.
5. **Capability detection + reconnect fallback**.
6. **Wire the mode** into config/splitter/dashboard; docs.

## 9. Risks / open questions

- **Miner set_extranonce compliance varies** — mitigated by the reconnect fallback;
  a per-miner capability log helps users see which path is active.
- **Frequent re-subscribes** annoy some pools — the smooth path avoids re-subscribe;
  only fallback miners re-subscribe, and the adaptive interval keeps it infrequent.
- **Efficiency** — any time-slicing loses a little to switch overhead + work ramp;
  adaptive sizing minimizes it. Expect a small single-digit % vs a dedicated miner.
- **Interaction with per-pool difficulty floors** (public-pool.io) — the adaptive
  sizer uses the live pool diff, so it naturally respects a high floor.
