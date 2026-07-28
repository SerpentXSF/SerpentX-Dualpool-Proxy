# Dual-Pool Proxy — Dual-Pool Stratum Proxy. Multi-stage: build stock ckpool (ckproxy),
# build our splitter, assemble a small runtime image. GPLv3.

# ---- Stage 1: build stock ckpool (unmodified) -> ckproxy/ckpmsg -------------
FROM debian:bookworm-slim AS ckpool-build
ARG CKPOOL_REF=master
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential git yasm pkg-config libtool autoconf automake \
      ca-certificates libzmq3-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN git clone --depth 1 --branch "${CKPOOL_REF}" https://github.com/ckolivas/ckpool.git \
 || git clone --depth 1 https://github.com/ckolivas/ckpool.git
WORKDIR /src/ckpool
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
