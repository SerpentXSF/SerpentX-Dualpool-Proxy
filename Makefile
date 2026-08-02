# Top-level Dual-Pool Proxy build. `make` builds the splitter; `make test` runs host
# unit tests; `make t2` runs the socket integration harness.
CC       ?= gcc
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -Isrc -Isrc/dual_pool/include
LDFLAGS  ?= -lpthread -lm -ljansson

DP       = src/dual_pool
SPLITTER_SRC = src/splitter.c src/relay.c src/alloc.c src/share_accounting.c \
               src/config.c src/ckproxy_config.c src/health.c src/webui.c \
               src/stratum_msg.c src/split_sched.c src/splitmux.c \
               $(DP)/pool_scheduler.c $(DP)/pool_failover.c $(DP)/dual_clamp.c

# Headers are prerequisites too: the build is a single compile of all sources, so
# without them a header-only edit (version.h, a struct change) leaves a stale
# binary behind — which once meant a release build still reporting the previous
# version string.
SPLITTER_HDR = $(wildcard src/*.h) $(wildcard $(DP)/include/*.h)

dualpool-splitter: $(SPLITTER_SRC) $(SPLITTER_HDR)
	$(CC) $(CFLAGS) -o $@ $(SPLITTER_SRC) $(LDFLAGS)

.PHONY: splitter test t2 clean
splitter: dualpool-splitter

test:
	$(MAKE) -C test/host run

t2:
	./test/integration/run_t2.sh

clean:
	rm -f dualpool-splitter
	$(MAKE) -C test/host clean
