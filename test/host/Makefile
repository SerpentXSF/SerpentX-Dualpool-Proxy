# Host unit tests (plain gcc, socket-free — builds on Linux, WSL, or native Windows).
CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -O0 -g -I../../src/dual_pool/include -I../../src
DP       = ../../src/dual_pool
DP_SRC   = $(DP)/dual_clamp.c $(DP)/pool_scheduler.c $(DP)/pool_failover.c

TESTS    = test_dual_pool test_alloc test_share test_health

run: build
	@for t in $(TESTS); do echo "== $$t =="; ./$$t || exit 1; done

build: $(TESTS)

test_dual_pool: test_dual_pool.c $(DP_SRC)
	$(CC) $(CFLAGS) -o $@ $^

test_alloc: test_alloc.c ../../src/alloc.c $(DP)/pool_scheduler.c $(DP)/dual_clamp.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_share: test_share.c ../../src/share_accounting.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_health: test_health.c ../../src/health.c $(DP)/pool_failover.c
	$(CC) $(CFLAGS) -o $@ $^

# Config parser + ckproxy-emit tests. These need jansson, so they are not part of
# the socket-free `run` set; run them where libjansson-dev is present (the
# dualpool-dev image, or a Linux host): `make config`.
config: test_config
	@echo "== test_config =="; ./test_config

test_config: test_config.c ../../src/config.c ../../src/ckproxy_config.c $(DP)/dual_clamp.c
	$(CC) $(CFLAGS) -o $@ $^ -ljansson

# Stratum message parser/emit tests. Pure/socket-free but needs jansson, so run
# alongside `config`: `make stratum`.
stratum: test_stratum_msg
	@echo "== test_stratum_msg =="; ./test_stratum_msg

test_stratum_msg: test_stratum_msg.c ../../src/stratum_msg.c
	$(CC) $(CFLAGS) -o $@ $^ -ljansson

# Adaptive slice scheduler tests. Pure/socket-free, no jansson needed: `make sched`.
sched: test_split_sched
	@echo "== test_split_sched =="; ./test_split_sched

test_split_sched: test_split_sched.c ../../src/split_sched.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

clean:
	rm -f $(TESTS) test_config test_stratum_msg test_split_sched

.PHONY: run build config stratum sched clean
