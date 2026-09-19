#!/usr/bin/env bash
# Reproduce an Ubuntu CI lane locally, before pushing.
#
# WHY THIS EXISTS: macOS and Linux disagree in ways this project has been
# bitten by repeatedly, and every one of them was found by CI rather than
# locally — a slow and public way to learn:
#   * -Werror. The Linux lane treats warnings as errors; Apple clang does not
#     warn about the same things, so a clean local build is not evidence.
#   * epoll vs kqueue. The gateway's readiness path is a different file.
#   * libstdc++ vs libc++. Different container internals, different UB
#     exposure, different behaviour under assertions.
#   * io_uring. The async journal path only compiles on Linux at all.
#
# Defaults to the "Ubuntu Debug Sanitizers" lane from .github/workflows/ci.yml.
# Not a substitute for CI — that also runs the latency gate and the full
# fault-injection matrix — but it turns the common failure into a local one.
#
# The source tree is mounted READ-ONLY and the build lands inside the
# container, so a Linux build can never leave objects in a host build dir for
# a later macOS build to link against. Stale-object false greens have cost
# this project real time more than once.
#
# Usage:
#   scripts/verify_linux.sh              # Debug + ASan/UBSan (the CI lane)
#   scripts/verify_linux.sh tsan         # ThreadSanitizer lane
#   scripts/verify_linux.sh release      # Release, no sanitizers
#   scripts/verify_linux.sh faultinject  # Fault-injection lane

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="ob-linux-verify:24.04"
LANE="${1:-sanitizers}"

# Test exclusions must MATCH ci.yml exactly, or this script reports a failure
# the real lane would never see — which is worse than not running it, because
# it teaches you to ignore the output. The TSan lane's extra exclusions are
# documented in ci.yml: PropertyTest is a heavy fuzzer and TSan x property
# fuzzing is impractically slow (it times out at 180s here, which looks like a
# hang rather than a skip), and AdminServerEndpointsTest has accept/recv
# teardown races that are acceptable on an admin port off the data path.
EXCLUDE_DEFAULT='BenchmarkRegressionTest'
EXCLUDE_TSAN='BenchmarkRegressionTest|PropertyTest|AdminServerEndpointsTest'

case "$LANE" in
  sanitizers)
    CM_ARGS="-DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON -DENABLE_THREAD_SANITIZER=OFF -DBUILD_BENCHMARKS=ON"
    EXCLUDE="$EXCLUDE_DEFAULT"
    # Two, not nproc: a sanitizer build of this tree peaks at a few GB per
    # translation unit and OOM-kills cc1plus when fanned out. That surfaces as
    # "Killed signal terminated program cc1plus", not as a compile error —
    # the same reasoning as the parallelism cap in ci.yml.
    JOBS=2 ;;
  tsan)
    CM_ARGS="-DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=OFF -DENABLE_THREAD_SANITIZER=ON -DBUILD_BENCHMARKS=OFF"
    EXCLUDE="$EXCLUDE_TSAN"
    JOBS=2 ;;
  release)
    CM_ARGS="-DCMAKE_BUILD_TYPE=Release -DENABLE_SANITIZERS=OFF -DBUILD_BENCHMARKS=ON"
    EXCLUDE="$EXCLUDE_DEFAULT"
    JOBS=4 ;;
  faultinject)
    CM_ARGS="-DCMAKE_BUILD_TYPE=Debug -DENABLE_FAULT_INJECTION=ON -DENABLE_SANITIZERS=OFF -DBUILD_BENCHMARKS=OFF"
    EXCLUDE="$EXCLUDE_DEFAULT"
    JOBS=4 ;;
  *)
    echo "unknown lane: $LANE (want: sanitizers | tsan | release | faultinject)" >&2
    exit 2 ;;
esac

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "==> building $IMAGE (one-off; cached afterwards)"
    # Build from an EMPTY scratch context, not from $REPO_ROOT. The image needs
    # nothing from the tree, and the tree carries multi-GB gitignored build
    # directories that docker would otherwise upload as build context on every
    # cold build.
    ctx="$(mktemp -d)"
    trap 'rm -rf "$ctx"' EXIT
    cat > "$ctx/Dockerfile" <<'DOCKERFILE'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates liburing-dev python3 \
    && rm -rf /var/lib/apt/lists/*
DOCKERFILE
    docker build -t "$IMAGE" "$ctx"
    rm -rf "$ctx"
fi

echo "==> lane: $LANE"
echo "==> excluding: $EXCLUDE"
echo "==> $CM_ARGS"

# :ro on the source mount is the point — see the header.
docker run --rm -t \
    -v "$REPO_ROOT":/src:ro \
    "$IMAGE" \
    bash -euo pipefail -c "
        cmake -S /src -B /build $CM_ARGS -DBUILD_TESTS=ON
        cmake --build /build --parallel $JOBS
        ctest --test-dir /build --output-on-failure -L project -E '$EXCLUDE'
    "
echo "LINUX VERIFY ($LANE) EXIT=$?"
