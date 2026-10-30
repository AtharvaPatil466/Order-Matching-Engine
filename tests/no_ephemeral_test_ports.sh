#!/bin/sh
# Fixed test ports stay below Linux's ephemeral range (ip_local_port_range,
# 32768-60999): an outgoing connection from any process on the box can already
# hold a port in it, and the test's bind fails with EADDRINUSE — the round-3
# Linux TSan run lost EngineRefusesLegacyJournalTest that way on 47890. Bind 0
# where the server reports its port; otherwise pick one below 32768.
# Checks every number on a shell-test line, and on a C++/CMake line that names
# a port. usage: no_ephemeral_test_ports.sh <tests dir>
dir="${1:?tests dir}"
cd "$dir" || exit 1
hits=$(awk '
    FILENAME == "no_ephemeral_test_ports.sh" { next }
    FILENAME !~ /\.sh$/ {
        probe = tolower($0)
        gsub(/report|support|import|export|transport|proportion|portab/, "", probe)
        if (probe !~ /port|listenon|startas|connectto|htons/) next
    }
    {
        s = $0
        while (match(s, /[0-9]+/)) {
            n = substr(s, RSTART, RLENGTH) + 0
            if (RLENGTH == 5 && n >= 32768 && n <= 60999) { print FILENAME ":" FNR ": " $0; break }
            s = substr(s, RSTART + RLENGTH)
        }
    }' *.sh *.cpp *.h CMakeLists.txt)
if [ -n "$hits" ]; then
    echo "fixed test ports in the ephemeral range 32768-60999 (use port 0, or one below 32768):"
    echo "$hits"
    exit 1
fi
