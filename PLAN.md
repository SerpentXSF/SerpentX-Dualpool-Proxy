# PLAN.md — Dual-Pool Stratum Proxy

Implementation plan. Design spec:
[`docs/design/architecture.md`](docs/design/architecture.md).

**Architecture in one line:** a new C `splitter` binary supervises **two stock
(unmodified) `ckproxy` instances** — one per pool — and assigns each miner to a
pool by a hashrate-weighted error-diffusion ratio, relaying transparently so
shares route to the owning pool structurally.

**Environment:** dev host is Windows; ckpool is Linux-only. Pure-logic C is
gcc-testable natively; everything socket/ckproxy is built and run under **WSL2 /
Docker** (both confirmed available).

Legend: `[ ]` todo · `[~]` in progress · `[x]` done.

---

## Guardrails (apply throughout)

- **Never modify ckpool source.** Only clone + build it. All new logic lives in
  `src/`.
- **Never modify the ESP32 firmware project.** The `dual_pool` files are *copied*
  into `src/dual_pool/` with GPLv3 headers, not referenced in place.
- **Keep pure-logic modules socket-free** (`alloc`, `share_accounting`, `config`,
  `dual_pool/*`) so their unit tests build anywhere. Socket code (`relay`, `health`,
  `webui`) is separate and Linux-only.

---

## Milestone 0 — Scaffolding & licensing  `[x]`

- [x] `LICENSE` = full GPLv3 text.
- [x] Copy `pool_scheduler.{c,h}`, `pool_failover.{c,h}`, `dual_clamp.{c,h}` into
      `src/dual_pool/` (+ `include/`), add GPLv3 provenance headers.
- [x] `config.example.json` (from spec §6.1).
- [x] `test/host/Makefile` modeled on the ESP32 `dual_pool/test_host/Makefile`.
- [x] Port the existing dual_pool host tests; `make -C test/host run` passes
      (gcc 15.2 under WSL — no native Windows gcc; WSL is the build env).
- [x] `git init` + commits; `.gitattributes` enforces LF for Linux/Docker sources.
- [x] **(pulled from M7)** `web/index.html` — branded Dual-Pool Proxy dashboard shell,
      verified rendering live with demo-data fallback.

## Milestone 1 — Build ckpool in Docker  `[x]`

- [x] `Dockerfile` multi-stage: stage 1 clones `ckolivas/ckpool` and builds it
      (autogen/configure/make) -> `ckpool` + `ckpmsg`. **Builds clean** in Docker
      (current master is a C++/yyjson build; we run the binary, don't link it).
- [x] Confirmed ckpool proxy config format (`proxy[]` url/auth/pass, `serverurl`,
      `mindiff`/`startdiff`) and run modes (`-p` proxy, `-u` userproxy, `-s`, `-n`).
- [x] Full image builds: ckpool + splitter + web assets + tini entrypoint.

## Milestone 2 — Document the seams  `[ ]`

- [ ] `docs/seams.md`: how ckproxy's generator↔stratifier↔connector pass work and
      shares, the `ckpmsg` admin socket (stats/users/workers), userproxy vs proxy
      identity behavior, and exactly where the splitter relays + sniffs. This is
      the map the later milestones extend against.

## Milestone 3 — Config + supervise two ckproxy  `[x]`

- [x] `config.{c,h}` — parse our JSON (jansson), defaults, clamps, two-pool
      validation. **Test passing** (in dualpool-dev image; WSL lacks jansson).
- [x] `ckproxy_config.{c,h}` — emit `ckproxy{A,B}.json` (proxy array + failover +
      serverurl) and `ckproxy_spawn` (fork/exec `-p`/`-u`, console output ->
      per-proxy log file). **Emit test passing.**
- [x] Splitter `--config` mode: parse -> emit both configs -> spawn both ckproxy
      (distinct sockdirs) -> wait for listen -> monitor thread respawns on exit.
- [x] **T3 verified:** container boots, both ckproxy come up, splitter routes
      miners 70/30 through the real two-ckproxy chain.

**Follow-up (harness fidelity, not a product gap):** the minimal fake upstream
doesn't serve real work, so ckproxy holds downstream subscribes -> fake miners
don't complete the Stratum handshake in T3 (routing still verified). To validate
full share flow, either (a) flesh out `fake_upstream.py` to satisfy ckproxy's
generator (subscribe/mining.configure/notify with valid-looking work), or
(b) run once against a real test pool. Tracked for M5/M7.

## Milestone 4 — Farm-split allocation + share accounting  `[~]`

- [x] `alloc.{c,h}` — hashrate-weighted error diffusion (generalizes
      `pool_scheduler` Bresenham); per-miner weight input; pool-down donation.
      **T1 test passing**: equal-weight 70/30, extremes, and the discriminating
      skewed-weight case (700GH alone on A vs 7×100GH on B = 50/50 by hashrate).
- [x] `share_accounting.{c,h}` — pure core: track `set_difficulty` per session,
      correlate submit `id`→result, difficulty-at-submit-time weight, per-pool
      accept/reject, ignore unknown/dupe ids. **T1 test passing.**
- [x] `relay.{c,h}` — poll()-based verbatim bidirectional pump + optional passive
      line-sniff hook. (Linux; covered by T2.)
- [x] `splitter.c` — CLI-configured farm-split front-end: listen, alloc_pick,
      connect owning upstream, thread-per-conn relay, donation on upstream fail.
- [x] **T2 harness** (`test/integration/run_t2.sh`): two fake upstreams + fake
      miners + real splitter. **PASSES**: 40 miners @ ratio 70 -> 28/12 split;
      shares 84/36 = conns*3 (every share routed to the owning pool).
- [ ] Thin jansson line-parser feeding `share_accounting` events. (Needs
      `libjansson-dev`; will fold into the splitter's live accounting + `/metrics`.)
- [ ] Clean TCP disconnect + reallocation on pool reassignment (with M5 eviction).

## Milestone 5 — Pool-level failover, eviction, donation  `[x]`

- [x] `health.{c,h}` — wraps `pool_failover` per pool; tolerates N misses before
      DOWN, recovers on success. **T1 test passing** (down/recover, intermittent).
- [x] Splitter probe thread: connect-tests each pool every 3s, drives
      health -> allocator (donation), and **evicts** a pool's pinned miners
      (`shutdown()` their sockets) the instant it goes DOWN. Recovery re-enables
      allocation; drift returns on natural reconnects.
- [x] Live-connection registry for eviction (per-pool, mutex-guarded).
- [x] **Failover integration test** (`run_failover.sh`) **PASS**: donation (new
      miners -> survivor, dead pool gets none), eviction (held miner disconnected,
      logged), and recovery (traffic returns when the pool comes back). Uses a
      SIGUSR1 "stop accepting, keep held sessions" mode on the fake upstream to
      exercise the production case (probe-down while sessions live).

**Note:** in production, deeper "ckproxy-up-but-upstream-pool-dead" detection
(vs. ckproxy process death) is best driven by `ckpmsg` stats / job-flow
liveness — a refinement of the probe signal. Current probe catches
ckproxy/endpoint unreachability. Tracked for M7 polish.

## Milestone 6 — Single-miner time-slice  `[x]`

- [x] Implemented as **connection-recycling** time-slice (simpler + works with
      ANY miner, no capability gating): reuses `pool_scheduler` error-diffusion,
      advanced one slice per boundary; at each boundary `evict_all()` recycles the
      miner so it reconnects and is re-subscribed to the next slice's pool.
      Cost: minute-scale boundary churn (documented) — clean full reconnect avoids
      the extranonce-swap hazard entirely.
- [x] `--mode time_slice --interval MS` (and via config `mode`/`interval_ms`).
      Synthetic 100ms scheduler tick decouples the wall-clock interval from the
      scheduler's clamped internal interval.
- [x] **Integration test** (`run_timeslice.sh`) **PASS**: one reconnecting miner
      alternates pools by ratio (A=7/B=7 over 14 boundaries at ratio 50).

## Milestone 7 — Web UI, Docker, docs, tests  `[~]`

- [x] `webui.{c,h}` + `web/index.html` — live status (per-pool state, actual-vs-
      target ratio, per-pool weighted accepted/rejected, per-miner→pool list) +
      settings form (ratio slider, mode) that hot-applies. JSON REST
      `/api/status`, `/api/config`. Served by the splitter on `:8080`.
- [x] Relay sniff -> per-pool difficulty-weighted share accounting + worker names.
- [x] **Web integration test** (`run_webui.sh`) **PASS**: live status (28/12 at
      ratio 70, 20 miners, pools on), dashboard served, config hot-applied 70->40.
- [x] Multi-stage `Dockerfile` + `docker-compose.test.yml` (T3 full stack).
- [x] **T3** full-stack compose test: both ckproxy up + farm-split routing.
- [x] Prometheus `/metrics` + bundled `grafana/dualpool-dashboard.json`.
- [x] Optional dashboard password (`X-DualPool-Key` / `?key=`), auth-checked
      `/api` + `/metrics`; dashboard sends the key.
- [x] `POST /api/config` persists ratio/mode/interval to the config file; SIGHUP
      hot-reloads without dropping miners. Dashboard comes up before ckproxy wait.
- [x] `docker-compose.yml` + `.env.example` + `docker/entrypoint.sh` (generate
      config from env). `config.sovereign.example.json` (DATUM/OCEAN + solo).
- [x] Job-flow liveness in the probe (pool with miners + no work 90s => down).
- [x] End-user docs: GETTING_STARTED, CONFIGURATION, STRATEGIES, ROADMAP + README
      overhaul.
- [ ] Deferred to [docs/ROADMAP.md](docs/ROADMAP.md): SV2 translator sidecar,
      pyasic discovery, alerting/BLOCK-FOUND, split calculator, ckpmsg-deep
      liveness, Umbrel/Start9 packaging, version-mask audit + fractional vardiff.

---

## Open questions / decisions to revisit

- **Hashrate weight source:** derive from difficulty-weighted accepted shares in
  `share_accounting`, from `ckpmsg` per-worker stats, or both? Start with
  share-derived; cross-check against ckpmsg.
- **ckproxy transport:** unix sockets vs `127.0.0.1:400x` for splitter→ckproxy.
  Default to localhost TCP for simplicity; revisit if perf demands unix sockets.
- **Web UI framework:** plain static HTML + fetch() (no build step) to keep the
  image tiny and dependency-free. Confirmed direction.
