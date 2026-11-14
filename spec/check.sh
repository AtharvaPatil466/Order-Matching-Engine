#!/usr/bin/env bash
# Run TLC on a TLA+ spec in this directory.
#   ./check.sh                       -> MpscQueue (uses MpscQueue.cfg)
#   ./check.sh <ModuleName>          -> any module (uses <ModuleName>.cfg)
#   ./check.sh <ModuleName> <Cfg>    -> run <ModuleName> with an explicit cfg,
#                                       e.g. the bug-injected non-vacuity
#                                       variant:  ./check.sh Auction AuctionBroken.cfg
#
# Requires java (17+ tested) and tla2tools.jar in this directory.
#
# TLC's deadlock check is ON: a state with no successor fails the run
# ("Deadlock reached", exit 11). A spec whose behaviours legitimately end
# (a bound is spent, or the system is idle) says so with an explicit
# terminal stuttering step in its Next relation, so a stuck protocol still
# shows up as a deadlock.

set -euo pipefail

cd "$(dirname "$0")"

if [ ! -f tla2tools.jar ]; then
    echo "tla2tools.jar missing — fetching..."
    curl -L --silent --fail \
        -o tla2tools.jar \
        https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar
fi

# The v1.8.0 release jar. A different jar (a local newer build) is allowed
# with TLA2TOOLS_ANY_JAR=1; a fetched one must match.
JAR_SHA256=e1f11b75c4b5bc4478ba0ac5cb6ae28e40a01b0016bc33e0aa97da22dfdbcd7f
if [ "${TLA2TOOLS_ANY_JAR:-0}" != 1 ]; then
    actual=$( (sha256sum tla2tools.jar 2>/dev/null || shasum -a 256 tla2tools.jar) | cut -d' ' -f1)
    if [ "$actual" != "$JAR_SHA256" ]; then
        echo "tla2tools.jar sha256 $actual is not the v1.8.0 release ($JAR_SHA256)" >&2
        exit 2
    fi
fi

MODULE="${1:-MpscQueue}"
CONFIG="${2:-}"

if [ -n "$CONFIG" ]; then
    java -XX:+UseParallelGC \
         -cp tla2tools.jar tlc2.TLC \
         -workers auto \
         -config "$CONFIG" \
         "$MODULE"
else
    java -XX:+UseParallelGC \
         -cp tla2tools.jar tlc2.TLC \
         -workers auto \
         "$MODULE"
fi
