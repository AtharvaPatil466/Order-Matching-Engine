#!/bin/sh
# Roadmap 2.9: every #ifdef'd production path has a CI lane that compiles and
# tests it, or it is deleted. OB_HAVE_LIBURING had no lane at all: the
# io_uring journal path was compiled by nothing in CI.
#
# Lists every OB_* macro a CMakeLists.txt defines and fails unless
# scripts/feature_macro_lanes.txt maps it to a ci.yml job that carries the
# configure flag (or configure-log check) turning it on, or says why no lane
# builds it. A new macro with neither fails here.
# usage: scripts/check_feature_macro_lanes.sh
set -u
cd "$(dirname "$0")/.." || exit 1
TABLE=scripts/feature_macro_lanes.txt
CI=.github/workflows/ci.yml

# Names inside *compile_definitions(...), comments and ${VARS} stripped.
defined=$(git ls-files -- CMakeLists.txt '*/CMakeLists.txt' '*.cmake' | xargs awk '
    { sub(/#.*/, "") }
    /compile_definitions[ \t]*\(/ { inside = 1 }
    inside {
        s = $0
        gsub(/\$\{[^}]*\}/, "", s)
        while (match(s, /OB_[A-Z0-9_]+/)) {
            print substr(s, RSTART, RLENGTH)
            s = substr(s, RSTART + RLENGTH)
        }
        if ($0 ~ /\)/) inside = 0
    }' | sort -u)
[ -n "$defined" ] || { echo "FAIL: found no OB_* compile definitions — is this the repo root?"; exit 1; }

# The lines of one job in ci.yml: from "  <job>:" to the next job key.
job_block() {
    awk -v job="$1" '
        $0 ~ "^  " job ":[ \t]*$" { on = 1; next }
        on && /^  [A-Za-z0-9_-]+:[ \t]*$/ { exit }
        on' "$CI"
}

fail=0
listed=" "
while read -r macro kind job text; do
    case "$macro" in ''|'#'*) continue ;; esac
    listed="$listed$macro "
    if ! printf '%s\n' "$defined" | grep -qx "$macro"; then
        echo "FAIL: $TABLE lists $macro, which no CMakeLists.txt defines"; fail=1; continue
    fi
    case "$kind" in
        lane)
            if [ -n "$text" ] && job_block "$job" | grep -qF -- "$text"; then
                echo "  ok  $macro — ci.yml job $job has: $text"
            else
                echo "FAIL: $macro: ci.yml job '$job' does not contain: $text"; fail=1
            fi ;;
        none|always)
            if [ -n "$job" ]; then echo "  --  $macro ($kind): $job $text"
            else echo "FAIL: $macro: a '$kind' line needs a reason"; fail=1; fi ;;
        *)
            echo "FAIL: $macro: kind must be lane, none or always, not '$kind'"; fail=1 ;;
    esac
done < "$TABLE"

for m in $defined; do
    case "$listed" in
        *" $m "*) ;;
        *) echo "FAIL: $m is defined in CMake but not in $TABLE — name the ci.yml job that builds it, or say why none does"
           fail=1 ;;
    esac
done
exit "$fail"
