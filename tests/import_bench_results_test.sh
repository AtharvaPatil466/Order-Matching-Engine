#!/bin/sh
# scripts/import_bench_results.sh lands only a successful bench-x86 run, only
# under the commit its env.txt names, never over an existing results/<sha>/,
# and writes no number that is not in the run's own env.txt. `gh` is a stub.
# usage: import_bench_results_test.sh <repo root>
root="${1:?repo root}"
import="$root/scripts/import_bench_results.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
sha=0123456789abcdef0123456789abcdef01234567
fail=0
check() { if [ "$1" = ok ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }

mkdir -p "$work/bin" "$work/artifact"
cat > "$work/bin/gh" <<'EOF'
#!/bin/sh
case "$1 $2" in
    "run view") printf '%s\n' "$FAKE_META" ;;
    "run download")
        while [ $# -gt 0 ]; do [ "$1" = --dir ] && dir="$2"; shift; done
        cp -R "$FAKE_ARTIFACT/." "$dir/" ;;
    *) echo "stub gh: unexpected $*" >&2; exit 1 ;;
esac
EOF
chmod +x "$work/bin/gh"
printf 'commit: %s\ndate_utc: 2026-11-02T00:00:00Z\n--- uname -a\nLinux runner 6.8.0 x86_64 GNU/Linux\njournal_fs: ext2/ext3\n' "$sha" \
    > "$work/artifact/env.txt"
echo "honest output" > "$work/artifact/honest-prod-1.txt"
echo "honest-prod-1 0" > "$work/artifact/exit-codes.txt"
tab="$(printf '\t')"
meta() { echo "$1${tab}$2${tab}$3${tab}https://github.com/o/r/actions/runs/42"; }

attempt() {  # attempt <meta>; leaves the exit code in $rc
    PATH="$work/bin:$PATH" FAKE_META="$1" FAKE_ARTIFACT="$work/artifact" \
        RESULTS_DIR="$work/results" bash "$import" "${2:-42}" > "$work/out" 2>&1
    rc=$?
}
# refused: the script itself said no (not, say, a missing file or a crash).
refused() { [ "$rc" -ne 0 ] && grep -q -e '^FATAL' -e '^usage' "$work/out"; }

attempt "$(meta 'Bench x86' "$sha" success)" abc
refused && [ ! -e "$work/results" ] && check ok "a non-numeric run id is refused" \
    || check no "a non-numeric run id is refused"
attempt "$(meta 'Journal Contention (H6)' "$sha" success)"
refused && [ ! -e "$work/results/$sha" ] && check ok "another workflow's run is refused" \
    || check no "another workflow's run is refused"
attempt "$(meta 'Bench x86' "$sha" failure)"
refused && [ ! -e "$work/results/$sha" ] && check ok "a failed run is refused" \
    || check no "a failed run is refused"
attempt "$(meta 'Bench x86' fedcba9876543210fedcba9876543210fedcba98 success)"
refused && [ ! -e "$work/results/fedcba9876543210fedcba9876543210fedcba98" ] \
    && check ok "an env.txt naming another commit is refused" \
    || check no "an env.txt naming another commit is refused"

attempt "$(meta 'Bench x86' "$sha" success)"
d="$work/results/$sha"
[ "$rc" -eq 0 ] && cmp -s "$d/env.txt" "$work/artifact/env.txt" && [ -f "$d/honest-prod-1.txt" ] \
    && check ok "a successful run lands as results/<sha>/ with its raw files" \
    || { check no "a successful run lands as results/<sha>/ with its raw files"; cat "$work/out"; }
invented=$(grep '[0-9]' "$d/README.md" 2>/dev/null | while IFS= read -r line; do
    case "$line" in (*"$sha"*|*"runs/42"*|*"import_bench_results.sh 42"*|*.txt) continue ;; esac
    grep -qxF -- "$line" "$work/artifact/env.txt" || echo "$line"
done)
[ -f "$d/README.md" ] && grep -q '^Linux runner' "$d/README.md" && [ -z "$invented" ] \
    && check ok "README lists env.txt lines and no number of its own" \
    || { check no "README lists env.txt lines and no number of its own"; echo "$invented"; }
attempt "$(meta 'Bench x86' "$sha" success)"
refused && cmp -s "$d/env.txt" "$work/artifact/env.txt" \
    && check ok "an existing results/<sha>/ is not overwritten" \
    || check no "an existing results/<sha>/ is not overwritten"
exit "$fail"
