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

clean:
	rm -f $(TESTS)

.PHONY: run build clean
