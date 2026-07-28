# ckpool build patches

These patches are applied to the **stock ckpool** source during the Docker build
(Stage 1). They are minimal bug fixes — not feature changes. ckpool remains
otherwise unmodified.

**Which patches apply depends on `CKPOOL_REF`:**
- **v1.2.0** (default, pinned) — applies `0002` + `0003` + `0004`. v1.2.0 already
  contains the notify-keying fix and the modern Stratum-V1 protocol support (error
  tuples, difficulty handling) that older releases lack, so it works with modern
  pools (solo.ckpool, public-pool.io) as well as Kryptex. `0002` fixes a
  double-free crash; `0003` makes proxied miners track the pool's difficulty;
  `0004` forwards the miner's ASICBoost version-rolling bits upstream.
- **v1.0.0** (legacy jansson fallback) — applies `0001` + `0002`. Stable but cannot
  speak modern pools' protocol; kept for reference. It needs `0002` too: v1.0.0
  contains the same double-free (from upstream `d385b74b`), which stays dormant
  only until `0001` lets shares flow — so both are applied together.

## 0004-proxy-forward-version-rolling.patch  (v1.2.0)

**Forwards the miner's ASICBoost version-rolling bits upstream, in `stratifier.c`
`submit_share()` and `generator.c` `proxy_send()`.**

Modern miners (BitAxe, NerdAxe, …) roll the block version for efficiency and find
shares against the *rolled* version. The stratifier validated those shares locally
with the version bits, but `submit_share()` never passed the `version_mask` to the
generator, and `proxy_send()` built a 5-field `mining.submit` without it. The pool
then rehashed with the original (un-rolled) version, computed a different hash, and
rejected every forwarded share as *"Above target"* / invalid — on every pool. The
fix carries `version_mask` through to the generator and appends it as the 6th
`mining.submit` param when present, so the pool rehashes with the same version the
miner used. Verified by packet capture: forwarded submits carry the version bits
and the pool replies `result: true`. This was the last blocker to valid upstream
shares.

## 0003-proxy-client-track-pool-diff.patch  (v1.2.0)

**Makes proxied miners mine at the pool-dictated difficulty in `stratifier.c`
`update_diff()`.**

In proxy mode ckpool forwards upstream only shares that meet the upstream
("proxy") difficulty. But `update_diff()` — which adopts the pool's
`mining.set_difficulty` — only ever *lowered* connected clients to a decreased
proxy diff and `return`ed early when the diff *rose*, never raising them. So a
BitAxe/Hammer that advertised a low `mining.suggest_difficulty` (~462) kept mining
at ~462 while a high-difficulty pool (public-pool.io defaults to ~100000,
solo.ckpool assigns a high diff to the aggregate proxy connection) expected far
more. The miner's low-diff shares are validated locally but almost never meet the
upstream diff, so they're silently *not forwarded* — the worker looks dead at the
pool. The fix drops the rose/`return` asymmetry and syncs every client on the
subproxy to the pool diff in both directions (the generator re-sends the diff on
every notify, so clients converge within one job). Kryptex is unaffected — it
dictates a low diff that the fix simply tracks.

## 0002-proxy-recv-double-free.patch  (v1.2.0 and v1.0.0)

**Removes a double-free / use-after-free in `generator.c` `parse_share()`.**

`parse_share()` handles the upstream pool's response to each forwarded share. On
finding the matching pending share it did `HASH_DEL(...); free(share);` inside the
lock, then — because the `share` pointer was still non-NULL — fell through the
`if (!share)` guard and read `share->diff` / `share->client_id` (use-after-free)
and `free(share)` a second time (double-free). glibc aborts with
`free(): double free detected in tcache 2`, crashing the `proxyrecv` thread within
seconds of shares flowing. The fix drops the premature inner `free`; the share is
freed exactly once at the end after its fields are read. Diagnosed from a core dump
(`gdb` backtrace: `parse_share` at generator.c:1869 ← `proxy_recv`).

## 0001-proxy-notify-keying.patch  (v1.0.0 only)

**One line in `src/generator.c` (`proxy_send`): `HASH_FIND_INT` → `HASH_FIND_I64`.**

In proxy mode, when a miner submits a share, `proxy_send()` looks up the notify
instance (the job the miner actually mined) by its job id so it can forward the
share upstream against the correct job. The job id is a 64-bit value (`int64_t`),
and instances are stored 64-bit-keyed (`HASH_ADD_I64`, generator.c:1039) — but the
lookup used the 32-bit `HASH_FIND_INT`. The mismatched key width makes the lookup
miss, or collide onto the wrong job. The result: every forwarded share is either
dropped ("failed to find matching jobid in proxysend") or submitted against the
wrong job and **rejected as invalid by the upstream pool**, even though the local
stratifier counted it accepted. Symptom: 100% invalid / absent worker at the pool
while the proxy's own dashboard looks healthy.

This is the identical fix ckpool made upstream in commit `308410dd` ("Fix proxy
notify instances being keyed wrongly", 2026-07-20), which first shipped in v1.2.0.
This backport applies it to the older v1.0.0 base only; the default v1.2.0 build
already includes it and does not use this patch.
