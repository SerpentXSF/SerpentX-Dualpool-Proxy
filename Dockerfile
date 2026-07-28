# Dual-Pool Proxy — Dual-Pool Stratum Proxy. Multi-stage: build stock ckpool (ckproxy),
# build our splitter, assemble a small runtime image. GPLv3.

# ---- Stage 1: build stock ckpool -> ckproxy/ckpmsg ---------------------------
FROM debian:bookworm-slim AS ckpool-build
# Pinned to v1.2.0. Unlike the older releases it already carries the proxy
# notify-keying fix AND the modern Stratum-V1 protocol support (error-tuple
# parsing + difficulty handling) that solo.ckpool and public-pool.io require, so
# it works with modern pools as well as Kryptex-style ones. v1.2.0's one blocker
# was a double-free in generator.c parse_share() that aborted the proxyrecv thread
# within seconds of shares flowing (diagnosed from a core dump). We fix it with
# docker/patches/0002-proxy-recv-double-free.patch. ckpool is otherwise unmodified.
#
# The legacy jansson release v1.0.0 is still selectable (--build-arg CKPOOL_REF=v1.0.0);
# it needs 0001 instead but cannot speak modern pools' protocol. See
# docker/patches/README.md.
ARG CKPOOL_REF=v1.2.0
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential git yasm pkg-config libtool autoconf automake \
      ca-certificates libzmq3-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
# Clone the exact pinned ref. Do NOT fall back to master: the patches below target
# a specific version, and silently building an unpinned tree would apply them to
# the wrong base.
RUN git clone --depth 1 --branch "${CKPOOL_REF}" https://github.com/ckolivas/ckpool.git
COPY docker/patches/ /src/patches/
WORKDIR /src/ckpool
# Apply the bug fix matching the pinned ref. Fail the build if the expected patch
# does not apply cleanly rather than silently shipping a crash/corruption bug.
RUN if [ "${CKPOOL_REF}" = "v1.2.0" ]; then \
      echo "Applying v1.2.0 patches: 0002 (double-free), 0003 (track pool diff), 0004 (forward version-rolling)..." && \
      git apply -v /src/patches/0002-proxy-recv-double-free.patch && \
      git apply -v /src/patches/0003-proxy-client-track-pool-diff.patch && \
      git apply -v /src/patches/0004-proxy-forward-version-rolling.patch ; \
    elif [ "${CKPOOL_REF}" = "v1.0.0" ]; then \
      echo "Applying v1.0.0 patches: 0001 (notify-keying), 0002 (double-free)..." && \
      git apply -v /src/patches/0001-proxy-notify-keying.patch && \
      git apply -v /src/patches/0002-proxy-recv-double-free.patch ; \
    else \
      echo "CKPOOL_REF=${CKPOOL_REF}: no matching backport patch, building stock." ; \
    fi
RUN ./autogen.sh && ./configure && make -j"$(nproc)"
RUN mkdir -p /out && \
    cp src/ckpool /out/ckpool && \
    ( cp src/ckpmsg /out/ckpmsg 2>/dev/null || true ) && \
    ls -l /out

# ---- Stage 2: build the Dual-Pool Proxy splitter ----------------------------------
FROM debian:bookworm-slim AS splitter-build
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential libjansson-dev pkg-config \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /build
COPY Makefile ./
COPY src/ ./src/
RUN make dualpool-splitter && ls -l dualpool-splitter

# ---- Runtime ---------------------------------------------------------------
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      tini libjansson4 libzmq5 ca-certificates \
 && rm -rf /var/lib/apt/lists/*
COPY --from=ckpool-build   /out/ckpool            /usr/local/bin/ckpool
COPY --from=ckpool-build   /out/ckpmsg            /usr/local/bin/ckpmsg
COPY --from=splitter-build /build/dualpool-splitter /usr/local/bin/dualpool-splitter
COPY web/                  /usr/local/share/dualpool/web/
COPY config.example.json   /usr/local/share/dualpool/config.example.json
COPY docker/entrypoint.sh  /usr/local/bin/dualpool-entrypoint.sh
RUN chmod +x /usr/local/bin/dualpool-entrypoint.sh

# The entrypoint uses /config/config.json if present, else generates it from env
# vars (the .env quickstart), then runs the splitter (which spawns two ckproxy).
EXPOSE 3333 8080
VOLUME ["/config"]
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/dualpool-entrypoint.sh"]
