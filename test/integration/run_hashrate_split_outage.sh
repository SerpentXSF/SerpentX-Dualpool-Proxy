#!/usr/bin/env bash
# HSPLIT-OUTAGE: a pool that dies MID-SESSION while a miner is actively split.
#
# run_hashrate_split_splitter_degrade.sh only covers a pool that is already
# unreachable or workless AT CONNECT. This test covers the realistic outage: the
# split is up, both pools are taking shares, and then one of them goes away. The
# mux is supposed to handle the two cases asymmetrically:
#
#   the pool the miner is NOT currently on dies
#       -> recoverable. handle_pool_readable() must call sec_fail() instead of
#          ending the session: close the fd, arm a backoff reconnect (3s,
#          doubling), keep the miner mining the pool it is on, and rejoin
#          ("secondary pool X ready") when the dead pool returns.
#   the pool the miner IS on (the active pool) dies
#       -> fatal for this session: the mux tears it down, the miner reconnects,
#          and the splitter's accept branch re-routes it onto the survivor.
#
# In hashrate_split BOTH pools alternate as the active pool every slice, so
# "the pool the miner is not on" is the mux's async `sec` pool half the time and
# the synchronously-handshaked PRIMARY the other half. Those are the two
# sub-cases, and they are separate scenarios here — S1 and S3 — because which
# one you land on is pure timing, and a test that let the scheduler pick would be
# a coin flip.
#
#   S1  both pools mining; the miner is on the PRIMARY; SIGKILL the SEC pool.
#         asserts: no miner reconnect, the active pool keeps accepting, the mux
#         logs the single-pool degrade + backoff retry, the dead pool gets zero
#         shares; then restart it and assert it rejoins ("secondary pool X
#         ready") and RECEIVES SHARES AGAIN — i.e. the split resumes on its own.
#   S2  both pools mining; SIGKILL the ACTIVE pool (determined at runtime).
#         asserts: the miner ends up on the SURVIVOR (session end + reconnect is
#         the expected path), the dead pool gets nothing more, and the miner is
#         still mining at the end of the run — never stranded.
#   S3  both pools mining; the miner is on the SEC pool; SIGKILL the PRIMARY.
#         Same contract as S1 (the miner is not on the pool that died), so the
#         same assertions: no reconnect, keep mining, degrade logged.
#   global (each scenario) zero cross-routed shares: a pool may only receive
#         shares for a job IT issued (job-id prefix) mined with ITS OWN
#         extranonce1 (fake_upstream logs `en=`).
#
# SIGKILL — not SIGUSR1 — is what models a real outage. SIGUSR1 only stops the
# fake upstream ACCEPTING new connections; the mux's already-established fd stays
# open and keeps receiving notifies, so no outage is observable on the live
# session at all. SIGKILL drops the listener AND the in-flight session with no
# orderly shutdown, exactly like a pool container being killed.
#
# The miner is fake_miner_split.py in a reconnect loop (an ASIC redials a pool
# that drops it). Each loop iteration is one miner session, so counting sessions
# is how "did the miner reconnect?" is measured. The kill-to-check window is kept
# under 4s so a reconnect can only be the mux ending the session — the splitter's
# health probe needs two failed 5s rounds before it would evict anyone.
#
# KNOWN RESULT AS OF THIS COMMIT: S1 and S2 pass; S3 FAILS, reproducibly. That is
# a real defect, not a flaky test — handle_pool_readable() (splitmux.c:1879-1885)
# gates recovery on `p == m->sec`, the role the pool was handed once at session
# start (`m->sec = primary ^ 1`), rather than on `p != m->active`, the pool the
# miner is actually mining right now. Since the active pool alternates every
# slice, the PRIMARY is the non-active pool about half the time, and its death
# then ends a session the mux could have carried on. The miner does recover (it
# reconnects and the splitter re-routes it to the survivor, which is what S2
# proves), so this costs an unnecessary disconnect per outage, not correctness.
# The assertion is deliberately NOT relaxed to make the suite green.
#
# Run inside the dualpool-dev image (python3+gcc+make+jansson):
#   docker run --rm -v "$PWD":/repo -w /repo dualpool-dev \
#       bash test/integration/run_hashrate_split_outage.sh
set -u
cd "$(dirname "$0")"
ROOT=../..
BIN=/tmp/dualpool_splitter_hsplit_outage
LPORT=3339
APORT=4041
BPORT=4042

# Slice knobs: target 3 shares, slices clamped to [8s,10s]. The miner submits
# every 0.4s, so the measured share rate always drives the computed slice below
# the floor -> every slice is 8s (the first is 10s, before there is an estimate).
# That leaves ~7s between "we observed which pool is active" and the next swap,
# so the choreography is never racing the scheduler.
TARGET_SHARES=3
MIN_SLICE=8
MAX_SLICE=10

WORKER=wallet.outage
fail=0

# ---------------------------------------------------------------------------
# Build the REAL splitter (0-warning gate).
# ---------------------------------------------------------------------------
echo "== compiling dualpool-splitter =="
BUILD_OUT=$(make -C "$ROOT" dualpool-splitter 2>&1)
BUILD_RC=$?
echo "$BUILD_OUT"
if [ "$BUILD_RC" -ne 0 ]; then echo "HSPLIT-OUTAGE FAILED: compile error"; exit 1; fi
if echo "$BUILD_OUT" | grep -qi "warning:"; then
  echo "HSPLIT-OUTAGE FAILED: build emitted warnings"; exit 1; fi
cp "$ROOT/dualpool-splitter" "$BIN"

# ---------------------------------------------------------------------------
# helpers (LOG / SPLOG / MINERLOG are re-pointed per scenario)
# ---------------------------------------------------------------------------
LOG=; SPLOG=; MINERLOG=; UPA=; UPB=; SP=; MINERPID=

mark()  { echo "--- $1 ---" >> "$LOG"; }         # phase marker inside the pool log
after() { awk -v m="--- $1 ---" 'f{print} $0==m{f=1}' "$LOG"; }
shares(){ grep -c "^$1 share " "$LOG"; }         # total shares pool $1 received
# shares pool $2 received after marker $1
shares_after() { after "$1" | grep -c "^$2 share "; }

start_up() {   # start_up <tag> <port> -> echoes the pid
  python3 fake_upstream.py --interval 1.2 --port "$2" --tag "$1" --log "$LOG" \
      >> "$LOG.up$1.out" 2>&1 &
  echo $!
}

# The pool the miner is CURRENTLY on == the pool that most recently received a
# share. Read from the log; never assumed.
active_pool() { grep -E "^[AB] share " "$LOG" | tail -1 | cut -c1; }
# The pool the splitter handed the mux as the synchronous PRIMARY.
primary_pool() { sed -n 's/.*hsplit route -> start \([AB]\).*/\1/p' "$SPLOG" | tail -1; }
other_pool()  { [ "$1" = "A" ] && echo B || echo A; }
miner_sessions() { grep -c "^=== miner session .* start ===" "$MINERLOG"; }

miner_loop() {   # miner_loop <total seconds>
  local end=$(( $(date +%s) + $1 )) n=0 left
  while :; do
    left=$(( end - $(date +%s) ))
    [ "$left" -le 1 ] && break
    n=$((n+1))
    echo "=== miner session $n start ===" >> "$MINERLOG"
    python3 fake_miner_split.py 127.0.0.1 "$LPORT" "$left" "$WORKER" >> "$MINERLOG" 2>&1
    echo "=== miner session $n end (rc=$?) ===" >> "$MINERLOG"
    sleep 0.3
  done
}

wait_for() {   # wait_for <seconds> <shell-condition...>
  local deadline=$(( $(date +%s) + $1 )); shift
  while [ "$(date +%s)" -lt "$deadline" ]; do
    eval "$@" && return 0
    sleep 0.25
  done
  return 1
}

start_scenario() {   # start_scenario <name> <miner seconds>
  LOG=/tmp/hsplit_outage_$1.log
  SPLOG=/tmp/hsplit_outage_$1_sp.log
  MINERLOG=/tmp/hsplit_outage_$1_miner.log
  : > "$LOG"; : > "$SPLOG"; : > "$MINERLOG"
  UPA=$(start_up A "$APORT")
  UPB=$(start_up B "$BPORT")
  sleep 0.6
  "$BIN" --listen "$LPORT" --poolA 127.0.0.1:$APORT --poolB 127.0.0.1:$BPORT \
    --mode hashrate_split --ratio 50 --target-shares "$TARGET_SHARES" \
    --min-slice "$MIN_SLICE" --max-slice "$MAX_SLICE" > "$SPLOG" 2>&1 & SP=$!
  sleep 0.6
  miner_loop "$2" & MINERPID=$!
}

stop_scenario() {
  kill "$MINERPID" 2>/dev/null
  pkill -f "fake_miner_split.py 127.0.0.1 $LPORT" 2>/dev/null
  kill "$SP" "$UPA" "$UPB" 2>/dev/null
  sleep 0.5
  wait 2>/dev/null
}

# Every share a pool received must carry a job IT issued and ITS OWN extranonce1.
check_routing() {   # check_routing <scenario label>
  local abadj bbadj abade bbade
  abadj=$(grep "^A share " "$LOG" | grep -vc "job=A-")
  bbadj=$(grep "^B share " "$LOG" | grep -vc "job=B-")
  abade=$(grep "^A share " "$LOG" | grep -vc "en=aaaa0001")
  bbade=$(grep "^B share " "$LOG" | grep -vc "en=bbbb0001")
  echo "[$1] cross-routed: A job=$abadj en=$abade   B job=$bbadj en=$bbade"
  [ "$abadj" -eq 0 ] || { echo "FAIL: [$1] $abadj A-shares carry a job A never issued"; fail=1; }
  [ "$bbadj" -eq 0 ] || { echo "FAIL: [$1] $bbadj B-shares carry a job B never issued"; fail=1; }
  [ "$abade" -eq 0 ] || { echo "FAIL: [$1] $abade A-shares mined with a foreign extranonce1"; fail=1; }
  [ "$bbade" -eq 0 ] || { echo "FAIL: [$1] $bbade B-shares mined with a foreign extranonce1"; fail=1; }
}

trap 'stop_scenario' EXIT

# ===========================================================================
# S1 — the mux's SEC pool dies while the miner is on the PRIMARY, then returns.
# ===========================================================================
echo
echo "===== [S1] non-active SEC pool dies mid-session, then comes back ====="
start_scenario s1 80

# Wait for a live split (both pools mining) with the miner ON THE PRIMARY, so the
# pool we are about to kill is the mux's async `sec`.
PRI=$(wait_for 30 '[ -n "$(primary_pool)" ]' && primary_pool)
SEC=$(other_pool "$PRI")
echo "[S1] primary=$PRI sec=$SEC"
if ! wait_for 45 '[ "$(shares A)" -ge 1 ] && [ "$(shares B)" -ge 1 ] && [ "$(active_pool)" = "'"$PRI"'" ]'; then
  echo "FAIL: [S1] never reached 'both pools mining, miner on the primary' (test degenerate)"
  echo "       A=$(shares A) B=$(shares B) active=$(active_pool)"
  fail=1
fi
S1_SESS0=$(miner_sessions)

mark KILL
kill -9 $([ "$SEC" = "A" ] && echo "$UPA" || echo "$UPB") 2>/dev/null
echo "[S1] SIGKILLed $SEC (miner is on $PRI)"
sleep 3.5                       # < the 5s health probe round: no eviction yet

S1_SESS1=$(miner_sessions)
S1_ACT=$(shares_after KILL "$PRI")
S1_DEAD=$(shares_after KILL "$SEC")
S1_DEGRADE=$(grep -c "single-pool degrade" "$SPLOG")
S1_EVICT=$(grep -c "evicted" "$SPLOG")
echo "[S1] after the kill: $PRI shares=$S1_ACT  $SEC shares=$S1_DEAD"
echo "[S1] miner sessions $S1_SESS0 -> $S1_SESS1   degrade lines=$S1_DEGRADE   evictions=$S1_EVICT"
[ "$S1_SESS1" -eq "$S1_SESS0" ] || { echo "FAIL: [S1] the miner RECONNECTED ($S1_SESS0 -> $S1_SESS1) — a pool the miner is not on must not end the session"; fail=1; }
[ "$S1_ACT" -ge 4 ]   || { echo "FAIL: [S1] active pool $PRI got only $S1_ACT shares after the kill (miner stalled)"; fail=1; }
[ "$S1_DEAD" -eq 0 ]  || { echo "FAIL: [S1] SIGKILLed pool $SEC received $S1_DEAD shares"; fail=1; }
[ "$S1_DEGRADE" -ge 1 ] || { echo "FAIL: [S1] the mux never logged the single-pool degrade + backoff retry"; fail=1; }

# --- bring it back: it must rejoin AND start receiving shares again ---
sleep 3.5                       # ~7s down: the 3s retry has already failed once
mark BACK
if [ "$SEC" = "A" ]; then UPA=$(start_up A "$APORT"); else UPB=$(start_up B "$BPORT"); fi
echo "[S1] restarted $SEC"
if wait_for 40 'grep -q "secondary pool '"$SEC"' ready" "$SPLOG"'; then S1_READY=1; else S1_READY=0; fi
wait_for 30 '[ "$(shares_after BACK "'"$SEC"'")" -ge 2 ]'
S1_BACK=$(shares_after BACK "$SEC")
S1_SESS2=$(miner_sessions)
echo "[S1] 'secondary pool $SEC ready' logged=$S1_READY   $SEC shares after restart=$S1_BACK"
echo "[S1] miner sessions now=$S1_SESS2"
[ "$S1_READY" -eq 1 ] || { echo "FAIL: [S1] the mux never logged 'secondary pool $SEC ready' (no rejoin)"; fail=1; }
[ "$S1_BACK" -ge 2 ]  || { echo "FAIL: [S1] restored pool $SEC got $S1_BACK shares — the split did not resume"; fail=1; }
[ "$S1_SESS2" -eq "$S1_SESS0" ] || { echo "FAIL: [S1] the miner reconnected ($S1_SESS0 -> $S1_SESS2) across the whole outage"; fail=1; }

check_routing S1
stop_scenario

# ===========================================================================
# S2 — the ACTIVE pool dies. The miner must land on the survivor, not stranded.
# ===========================================================================
echo
echo "===== [S2] the ACTIVE pool dies mid-session ====="
start_scenario s2 70

if ! wait_for 45 '[ "$(shares A)" -ge 1 ] && [ "$(shares B)" -ge 1 ]'; then
  echo "FAIL: [S2] the split never reached both pools (test degenerate)"; fail=1
fi
ACT=$(active_pool)
SURV=$(other_pool "$ACT")
echo "[S2] active=$ACT survivor=$SURV"
mark KILL
kill -9 $([ "$ACT" = "A" ] && echo "$UPA" || echo "$UPB") 2>/dev/null
echo "[S2] SIGKILLed the ACTIVE pool $ACT"

wait_for 35 '[ "$(shares_after KILL "'"$SURV"'")" -ge 5 ]'
S2_SURV=$(shares_after KILL "$SURV")
S2_DEAD=$(shares_after KILL "$ACT")
echo "[S2] after the kill: survivor $SURV shares=$S2_SURV   dead $ACT shares=$S2_DEAD"
[ "$S2_SURV" -ge 5 ] || { echo "FAIL: [S2] survivor $SURV got only $S2_SURV shares — the miner was STRANDED when its pool died"; fail=1; }
[ "$S2_DEAD" -eq 0 ] || { echo "FAIL: [S2] SIGKILLed pool $ACT received $S2_DEAD shares"; fail=1; }

mark TAIL                       # still mining at the very end of the run?
sleep 6
S2_TAIL=$(shares_after TAIL "$SURV")
echo "[S2] shares in the final 6s window: $S2_TAIL"
[ "$S2_TAIL" -ge 3 ] || { echo "FAIL: [S2] only $S2_TAIL shares in the final window — the miner is not still mining"; fail=1; }

check_routing S2
stop_scenario

# ===========================================================================
# S3 — the mirror of S1: the non-active pool that dies is the PRIMARY.
# ===========================================================================
echo
echo "===== [S3] non-active PRIMARY dies while the miner is on the SEC pool ====="
start_scenario s3 70

PRI=$(wait_for 30 '[ -n "$(primary_pool)" ]' && primary_pool)
SEC=$(other_pool "$PRI")
echo "[S3] primary=$PRI sec=$SEC"
if ! wait_for 45 '[ "$(shares A)" -ge 1 ] && [ "$(shares B)" -ge 1 ] && [ "$(active_pool)" = "'"$SEC"'" ]'; then
  echo "FAIL: [S3] never reached 'both pools mining, miner on the sec pool' (test degenerate)"
  echo "       A=$(shares A) B=$(shares B) active=$(active_pool)"
  fail=1
fi
S3_SESS0=$(miner_sessions)

mark KILL
kill -9 $([ "$PRI" = "A" ] && echo "$UPA" || echo "$UPB") 2>/dev/null
echo "[S3] SIGKILLed $PRI (miner is on $SEC)"
sleep 3.5                       # < the 5s health probe round: no eviction yet

S3_SESS1=$(miner_sessions)
S3_ACT=$(shares_after KILL "$SEC")
S3_DEAD=$(shares_after KILL "$PRI")
S3_DEGRADE=$(grep -c "single-pool degrade" "$SPLOG")
S3_EVICT=$(grep -c "evicted" "$SPLOG")
S3_REROUTE=$(grep -c "hsplit route" "$SPLOG")
echo "[S3] after the kill: $SEC shares=$S3_ACT  $PRI shares=$S3_DEAD"
echo "[S3] miner sessions $S3_SESS0 -> $S3_SESS1   degrade lines=$S3_DEGRADE   evictions=$S3_EVICT   splitter routes=$S3_REROUTE"
[ "$S3_SESS1" -eq "$S3_SESS0" ] || { echo "FAIL: [S3] the miner RECONNECTED ($S3_SESS0 -> $S3_SESS1) — the pool that died is NOT the pool the miner is on, so the session must survive (see report: handle_pool_readable() keys recovery on the static role m->sec instead of on m->active)"; fail=1; }
[ "$S3_ACT" -ge 4 ]   || { echo "FAIL: [S3] the pool the miner was on ($SEC) got only $S3_ACT shares after the kill (miner stalled)"; fail=1; }
[ "$S3_DEAD" -eq 0 ]  || { echo "FAIL: [S3] SIGKILLed pool $PRI received $S3_DEAD shares"; fail=1; }
[ "$S3_DEGRADE" -ge 1 ] || { echo "FAIL: [S3] the mux never logged the single-pool degrade + backoff retry for $PRI"; fail=1; }

check_routing S3
stop_scenario
trap - EXIT

echo
echo "==========================================="
if [ "$fail" -eq 0 ]; then
  echo "HSPLIT-OUTAGE PASS: a pool dying mid-session while the miner is elsewhere"
  echo "  degraded + retried without dropping the miner and rejoined on its own"
  echo "  (both role sub-cases); the ACTIVE pool dying moved the miner to the"
  echo "  survivor; nothing was ever cross-routed and the miner never stalled."
  exit 0
else
  echo "HSPLIT-OUTAGE FAILED"; exit 1
fi
