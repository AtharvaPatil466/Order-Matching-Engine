#!/bin/sh
# Roadmap 2.6: research and simulation code is not part of the engine.
# It lives in OrderMatcherResearch; the engine library and the shipped binaries
# must carry none of its symbols.
#
# The binaries alone cannot show this. OrderMatcher is a static archive, so the
# linker drops an object nothing references: with SimulationDriver.cpp still in
# the library, nm OrderEngine was clean. The library is checked as well.
#
# usage: no_research_in_engine.sh <libOrderMatcher.a> <OrderEngine> [<binary>...]
set -u
pattern='Simulation|Research|NoiseTrader|InformedTrader|MarketMaker'
[ "$#" -ge 2 ] || { echo "usage: $0 <engine library> <binary>..."; exit 2; }
status=0
for f in "$@"; do
    # nm failing (missing tool, unreadable file) must fail the check, not pass it.
    if ! syms=$(nm -C "$f" 2>/dev/null) || [ -z "$syms" ]; then
        echo "FAIL: nm could not read $f"
        status=1
        continue
    fi
    hits=$(printf '%s\n' "$syms" | grep -E "$pattern")
    if [ -n "$hits" ]; then
        echo "FAIL: $f carries research/simulation symbols:"
        printf '%s\n' "$hits" | head -20
        status=1
    else
        echo "ok: $f"
    fi
done
exit "$status"
