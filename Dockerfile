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

# Build in release mode against a PORTABLE baseline, never -march=native.
# This image is a deployment artifact: native pins it to whatever CPU happened
# to build it, and running it on an older host is a SIGILL, not a slow path.
#   x86-64-v3 = AVX2/BMI2/FMA, Haswell/Excavator 2013-2015 onward. Covers the
#               GitHub ubuntu-latest runners and any realistic deployment
#               target, while still giving the vectorised codegen the engine
#               wants.
# Override for an older floor (or a specific known host) at build time:
#   docker build --build-arg OB_ARCH=x86-64-v2 .
# Empty picks the baseline for the architecture actually being built, so an
# arm64 build (a dev box running docker compose) does not try an x86 -march.
ARG OB_ARCH=
RUN ARCH="$OB_ARCH"; \
    if [ -z "$ARCH" ]; then \
        case "$(uname -m)" in \
            aarch64|arm64) ARCH=armv8-a ;; \
            *)             ARCH=x86-64-v3 ;; \
        esac; \
    fi; \
    echo "Building for -march=${ARCH}" \
    && cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=clang++-18 \
    -DOB_ARCH="${ARCH}" \
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
COPY --from=builder /src/build/MdSubscriber /app/

# Copy config template
COPY config/engine.conf.example /app/config/engine.conf

# Journal volume
VOLUME /app/journal

# Admin HTTP. OrderEngine listens on 127.0.0.1 unless told otherwise, and a
# published port or a k8s probe arrives on the container's interface, not its
# loopback — so the image widens it. The container's network namespace is the
# boundary; auth still applies.
ENV OB_ADMIN_BIND=0.0.0.0
EXPOSE 8080
# Order entry (TcpGateway). Refuses to start without OB_PARTICIPANT_CREDENTIALS.
EXPOSE 9001

# Health check using the /health endpoint
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
    CMD curl -sf http://localhost:8080/health || exit 1

ENTRYPOINT ["/app/OrderEngine"]
CMD ["--threads", "4", "--port", "8080", "--symbols", "4"]
