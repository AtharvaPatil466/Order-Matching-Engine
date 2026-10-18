#!/usr/bin/env bash
# Run docs/Runbook.md: every ```bash block, top to bottom, in one shell, inside
# ubuntu:24.04 against OrderEngine / GatewayServer / JournalReplayCLI built from
# this tree (roadmap 1.12-L1, DOC-3).
#
# WHY. The Runbook used to cite flags, signals, endpoints and scripts that did
# not exist; one procedure killed the engine and the DR procedure overwrote the
# journal. Prose review did not catch it. Running the commands does.
#
# Rules it enforces:
#   * A fenced block runs if it is ```bash or ```sh.
#   * Any block may opt out with the line
#       <!-- runbook-smoke: skip — <reason> -->
#     directly above its fence. Skipped blocks are listed with their reason.
#   * A non-bash block (config, sample output) without that marker is an error,
#     so nothing is skipped silently.
#   * Every flag in the first column of a Runbook table must appear in the
#     --help of the binary that section describes (GatewayServer under a
#     heading naming it, OrderEngine otherwise).
#
# The source is mounted read-only and built inside the container. Ports bound
# here (8080, 9876, 9877) live in the container's own network namespace.
#
# Usage: scripts/runbook_smoke.sh

set -euo pipefail

if [ "${1:-}" != "--in-container" ]; then
    REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    IMAGE="ob-runbook-smoke:24.04"
    if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
        echo "==> building $IMAGE (one-off; cached afterwards)"
        ctx="$(mktemp -d)"
        trap 'rm -rf "$ctx"' EXIT
        cat > "$ctx/Dockerfile" <<'DOCKERFILE'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake python3 curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*
DOCKERFILE
        docker build -t "$IMAGE" "$ctx"
    fi
    exec docker run --rm -v "$REPO_ROOT":/src:ro "$IMAGE" \
        timeout 1200 bash /src/scripts/runbook_smoke.sh --in-container
fi

# ── Inside the container ─────────────────────────────────────────────────────
echo "==> building OrderEngine GatewayServer MdSubscriber JournalReplayCLI"
cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
      -DBUILD_BENCHMARKS=OFF > /build.log 2>&1 || { cat /build.log; exit 1; }
cmake --build /build --parallel 2 \
      --target OrderEngine GatewayServer MdSubscriber JournalReplayCLI \
      >> /build.log 2>&1 || { tail -50 /build.log; exit 1; }
export PATH="/build:/build/tools:$PATH"

python3 - /src/docs/Runbook.md /runbook.sh <<'PY'
import re, subprocess, sys

src, out = sys.argv[1], sys.argv[2]
lines = open(src, encoding="utf-8").read().split("\n")
skip_re = re.compile(r"^<!-- runbook-smoke: skip — (.+) -->$")
errors, blocks, skipped, flags = [], [], [], []
heading, i = "", 0
while i < len(lines):
    line = lines[i]
    if line.startswith("## "):
        heading = line
    if line.startswith("|") and not line.startswith("|---"):
        first = line.split("|")[1]
        binary = "GatewayServer" if "GatewayServer" in heading else "OrderEngine"
        for tok in re.findall(r"`(-[^`\s]+)", first):
            flags.append((tok, binary, i + 1))
    if line.startswith("```"):
        lang, start = line[3:].strip(), i + 1
        j = i + 1
        while j < len(lines) and not lines[j].startswith("```"):
            j += 1
        if j == len(lines):
            errors.append(f"Runbook.md:{start}: unterminated code block")
            break
        prev = next((lines[k] for k in range(i - 1, -1, -1) if lines[k].strip()), "")
        m = skip_re.match(prev)
        if m:
            skipped.append((start, lang, m.group(1)))
        elif lang in ("bash", "sh"):
            blocks.append((start, "\n".join(lines[i + 1:j])))
        else:
            errors.append(f"Runbook.md:{start}: ```{lang} block has no runbook-smoke skip marker")
        i = j
    i += 1

helps = {b: subprocess.run([b, "--help"], capture_output=True, text=True).stdout
         for b in ("OrderEngine", "GatewayServer")}
for tok, binary, ln in flags:
    if not re.search(r"^\s+" + re.escape(tok) + r"(\s|$)", helps[binary], re.M):
        errors.append(f"Runbook.md:{ln}: table lists {tok}, which `{binary} --help` does not")

for ln, lang, why in skipped:
    print(f"SKIP  Runbook.md:{ln} ```{lang} — {why}")
print(f"==> {len(flags)} table flag(s) checked against --help")
if errors:
    print("\n".join("ERROR " + e for e in errors))
    sys.exit(1)

with open(out, "w") as f:
    f.write("set -euo pipefail\n")
    for n, (ln, body) in enumerate(blocks, 1):
        f.write(f"BLOCK='{n}/{len(blocks)} (Runbook.md:{ln})'\n")
        f.write(f"echo; echo \"==> RUN block $BLOCK\"\n{body}\n")
    f.write(f"echo; echo 'RUNBOOK SMOKE PASSED: {len(blocks)} block(s) run, {len(skipped)} skipped'\n")
PY

cd /root
trap 'rc=$?; if [ $rc -ne 0 ]; then
        echo "RUNBOOK SMOKE FAILED in block ${BLOCK:-?} (exit $rc)"
        for f in /var/lib/orderengine/*.log; do [ -f "$f" ] && { echo "--- $f"; tail -40 "$f"; }; done
      fi' EXIT
BLOCK=
source /runbook.sh
