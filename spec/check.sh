#!/usr/bin/env bash
# Run TLC on a TLA+ spec in this directory.
#   ./check.sh                       -> MpscQueue (uses MpscQueue.cfg)
#   ./check.sh <ModuleName>          -> any module (uses <ModuleName>.cfg)
#   ./check.sh <ModuleName> <Cfg>    -> run <ModuleName> with an explicit cfg,
#                                       e.g. the bug-injected non-vacuity
#                                       variant:  ./check.sh Auction AuctionBroken.cfg
#
# Requires java (17+ tested) and tla2tools.jar in this directory.

set -euo pipefail

cd "$(dirname "$0")"

if [ ! -f tla2tools.jar ]; then
    echo "tla2tools.jar missing — fetching..."
    curl -L --silent --fail \
        -o tla2tools.jar \
        https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar
fi

MODULE="${1:-MpscQueue}"
CONFIG="${2:-}"

if [ -n "$CONFIG" ]; then
    java -XX:+UseParallelGC \
         -cp tla2tools.jar tlc2.TLC \
         -workers auto -deadlock \
         -config "$CONFIG" \
         "$MODULE"
else
    java -XX:+UseParallelGC \
         -cp tla2tools.jar tlc2.TLC \
         -workers auto -deadlock \
         "$MODULE"
fi
