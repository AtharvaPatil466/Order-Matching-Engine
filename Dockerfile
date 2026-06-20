# ── Stage 1: Build ──
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y \
    clang-18 \
    cmake \
    ninja-build \
    && rm -rf /var/lib/apt/lists/*

ENV CXX=clang++-18

WORKDIR /src
COPY . .

# Build in release mode
RUN cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=clang++-18 \
    && cmake --build build --parallel

# ── Stage 2: Runtime ──
FROM ubuntu:24.04 AS runtime

RUN apt-get update && apt-get install -y \
    libstdc++6 \
    curl \
    iptables \
    iproute2 \
    faketime \
    && rm -rf /var/lib/apt/lists/*

# Detect libfaketime path (varies by Ubuntu release). Captured at
# build time so the runtime entrypoint can opt into it via env.
RUN find /usr/lib -name 'libfaketime.so.1' -print -quit > /etc/libfaketime.path 2>/dev/null \
    || echo "" > /etc/libfaketime.path

WORKDIR /app

# Copy binaries
COPY --from=builder /src/build/OrderEngine /app/
COPY --from=builder /src/build/GatewayServer /app/
COPY --from=builder /src/build/MdSubscriber /app/

# Copy config template
COPY config/engine.conf.example /app/config/engine.conf

# Journal volume
VOLUME /app/journal

# Admin HTTP
EXPOSE 8080
# Gateway
EXPOSE 9001
# Replication
EXPOSE 9002

# Health check using the /health endpoint
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -sf http://localhost:8080/health || exit 1

# Entrypoint shim — if OB_ENABLE_FAKETIME=1, LD_PRELOAD libfaketime
# so the chaos suite's clock-skew scenarios can advance the engine's
# perceived time without restarting the container. /etc/faketimerc
# is the runtime control file; the chaos harness writes to it.
# Otherwise the engine runs unaffected.
RUN printf '%s\n' \
    '#!/bin/sh' \
    'if [ "${OB_ENABLE_FAKETIME:-0}" = "1" ]; then' \
    '    FT_LIB=$(cat /etc/libfaketime.path)' \
    '    if [ -n "$FT_LIB" ] && [ -f "$FT_LIB" ]; then' \
    '        : > /etc/faketimerc  # start with no offset; harness updates' \
    '        export LD_PRELOAD="$FT_LIB"' \
    '        export FAKETIME_NO_CACHE=1' \
    '        echo "[entrypoint] libfaketime enabled at $FT_LIB"' \
    '    else' \
    '        echo "[entrypoint] OB_ENABLE_FAKETIME=1 but libfaketime not found"' \
    '    fi' \
    'fi' \
    'exec /app/OrderEngine "$@"' \
    > /app/entrypoint.sh && chmod +x /app/entrypoint.sh

ENTRYPOINT ["/app/entrypoint.sh"]
CMD ["--threads", "4", "--port", "8080", "--symbols", "4"]
