#!/usr/bin/env bash
#
# import_bench_results.sh — land one bench-x86 workflow run as results/<sha>/.
#
# Downloads the run's artifact with `gh run download`, refuses anything but a
# successful run of the Bench x86 workflow whose env.txt names the commit the
# run was on, and copies the raw files unchanged to results/<sha>/ with a
# README.md. The README quotes env.txt lines verbatim and lists the files: this
# script parses no benchmark output and writes no number of its own. Rewriting
# a doc number from these files, and its docs/PERF_SOURCES.md row, is a
# separate, reviewed change.
#
# usage: scripts/import_bench_results.sh <run-id>
#        (run ids: gh run list --workflow bench-x86.yml)
# env:   RESULTS_DIR (default <repo>/results) — for the test only.

set -euo pipefail

WORKFLOW="Bench x86"   # the name: of .github/workflows/bench-x86.yml
die() { echo "FATAL: $*" >&2; exit 1; }

run_id="${1:-}"
case "$run_id" in
    ''|*[!0-9]*) echo "usage: scripts/import_bench_results.sh <numeric run-id>" >&2; exit 2 ;;
esac
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
results="${RESULTS_DIR:-$repo/results}"

IFS=$'\t' read -r workflow sha conclusion url < <(
    gh run view "$run_id" --json workflowName,headSha,conclusion,url \
        --jq '[.workflowName, .headSha, .conclusion, .url] | @tsv')
[ "$workflow" = "$WORKFLOW" ] || die "run $run_id is '$workflow', not '$WORKFLOW'"
[ "$conclusion" = success ] || die "run $run_id concluded '$conclusion'; only a successful run is landed"
case "$sha" in
    *[!0-9a-f]*|'') die "run $run_id has no usable head sha ('$sha')" ;;
esac
dest="$results/$sha"
[ ! -e "$dest" ] || die "$dest exists; landed results are never overwritten"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
gh run download "$run_id" --name "bench-x86-$sha" --dir "$tmp"
[ -f "$tmp/env.txt" ] || die "the artifact has no env.txt"
grep -qx "commit: $sha" "$tmp/env.txt" \
    || die "env.txt does not say 'commit: $sha', the commit run $run_id was on"

mkdir -p "$dest"
cp -R "$tmp/." "$dest/"
{
    echo "# bench-x86 run $run_id at $sha"
    echo
    echo "Raw output of .github/workflows/bench-x86.yml ($url), landed unchanged"
    echo "by \`scripts/import_bench_results.sh $run_id\`. docs/PERF_SOURCES.md names the"
    echo "file each doc number comes from."
    echo
    echo "## Environment (env.txt, verbatim)"
    echo
    echo '```'
    grep -E '^(commit|date_utc|worktree_changes|runner|journal_fs):|^Linux |Model name|clang version|^cmake version|^--- (nproc|governor|liburing|pinning|workload)|copied' \
        "$tmp/env.txt" || true
    echo '```'
    echo
    echo "## Files"
    echo
    echo '```'
    (cd "$tmp" && ls -1)
    echo '```'
} > "$dest/README.md"
echo "landed run $run_id as $dest"
