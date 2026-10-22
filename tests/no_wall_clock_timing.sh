#!/bin/sh
# Benchmarks, tests and tools time intervals with steady_clock (BENCH-9,
# GTPERF-4). high_resolution_clock is system_clock under libstdc++ — the Linux
# platform the published numbers come from — so an NTP step lands inside a
# measured interval. usage: no_wall_clock_timing.sh <repo root>
root="${1:?repo root}"
hits=$(grep -rn --include='*.cpp' --include='*.h' 'high_resolution_clock::' "$root/benchmarks" "$root/tests" "$root/tools")
if [ -n "$hits" ]; then
    echo "time intervals with std::chrono::steady_clock, not high_resolution_clock:"
    echo "$hits"
    exit 1
fi
