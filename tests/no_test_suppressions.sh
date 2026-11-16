#!/bin/sh
# No suppression mechanism in tests (roadmap 2.1). A skipped gtest, a disabled
# one, or a known-divergence corpus file or marker turns a failure into
# silence, and a defect ships through it (ENGTEST-4). A line that must keep one
# says why on the same line: `skip-ok: <reason>`.
# usage: no_test_suppressions.sh <source root>
cd "${1:?source root}" || exit 1
# The tokens are split so this file does not match itself.
hits=$(grep -rnIE 'GTEST_''SKIP|DISABLED''_|\.''known([^A-Za-z0-9_]|$)' tests benchmarks |
       grep -v 'skip-ok: *[^ ]')
files=$(find tests benchmarks -name '*.''known*')
[ -z "$hits$files" ] && exit 0
echo "test suppressions without a 'skip-ok: <reason>' tag on the line:"
[ -n "$hits" ] && echo "$hits"
[ -n "$files" ] && echo "$files"
exit 1
