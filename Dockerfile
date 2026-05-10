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
    && rm -rf /var/lib/apt/lists/*

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

ENTRYPOINT ["/app/OrderEngine"]
CMD ["--threads", "4", "--port", "8080", "--symbols", "4"]
