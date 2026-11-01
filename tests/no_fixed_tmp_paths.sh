#!/bin/sh
# Tests own their filesystem (roadmap 2.10). A fixed path under /tmp is shared
# by every ctest -j job, sibling worktree and earlier run, so one can truncate,
# replay or delete another's file. Use uniqueTempPath() from tests/TempPath.h,
# or ${TMPDIR:-/tmp}/name_$$ in a shell test.
# usage: no_fixed_tmp_paths.sh <source root>
cd "${1:?source root}" || exit 1
# The quote and the path are split so this file does not match itself.
hits=$(grep -rn '"''/tmp/' tests benchmarks)
rc=$?
[ "$rc" -eq 1 ] && exit 0
if [ "$rc" -eq 0 ]; then
    echo "fixed /tmp paths in tests or benchmarks (use uniqueTempPath() from tests/TempPath.h):"
    echo "$hits"
fi
exit 1
