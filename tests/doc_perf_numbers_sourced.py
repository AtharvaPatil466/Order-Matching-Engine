#!/usr/bin/env python3
"""Every performance number in the docs has a row in docs/PERF_SOURCES.md.

usage: doc_perf_numbers_sourced.py <repo root> [--missing | --fix-lines]

A "performance number" is a number carrying a time, rate, size, percentage or
ratio unit (261 ns, 2.80M ops/s, 10.3 MiB, 3.8x, 32.8%, 152/order), including
one in a table header, or any number in a table column whose header names a
latency/throughput statistic (P50, Max, ns/op, IOPS, ...).
A row whose source is WITHDRAWN, UNRECORDED or ESTIMATE also needs the doc to
say so where the reader sees the number: on its line, in its table's header
row or in an enclosing heading (DOCS-4). Classifying a number is not sourcing it. The
check groups them by line; a row in PERF_SOURCES.md is keyed by its file and
the exact numbers on that line, so changing a number needs a new source, and a
line that only moved needs --fix-lines (which rewrites the line numbers and
nothing else). --missing prints rows to fill in for the lines with no source.

ponytail: regex, not a markdown parser. A unit-less number in prose is not
seen; the inventory is per line, so it is covered when its line has any unit.
"""
import re
import sys
from pathlib import Path

DOCS = [
    "README.md",
    "BENCHMARKS.md",
    "PerformanceWhitepaper.md",
    "Project_Overview.md",
    "docs/CapacityPlanning.md",
]
SOURCES = "docs/PERF_SOURCES.md"

NUM = r"~?\d[\d,]*(?:\.\d+)?(?:\s?[–-]\s?\d[\d,]*(?:\.\d+)?)?"
UNIT = (r"(?:ns/op|ns|µs|μs|us|ms|sec|seconds|s|"
        r"(?:ops|orders|appends|entries|msgs?|events)/s(?:ec)?|/s(?:ec)?|"
        r"GB/s|MB/s|bytes|KiB|MiB|GiB|KB|MB|GB|TB|B|instructions|cycles|"
        r"%|/[Oo]rders?|/fills?|"                      # 32.8%, 152/order (DOCS-9)
        r"(?:×|x)(?!\s*[\d~]))")  # a ratio, not "50K × 86400s"
TOKEN = re.compile(rf"(?<![\w.%]){NUM}[KkMG]?[ _]?{UNIT}(?![\w])")
# A source that is not a measurement: the doc line must say so itself
# (DOCS-4), so a reader who never opens this table is not misled.
MUST_SAY = {"WITHDRAWN": ("withdrawn",), "UNRECORDED": ("unrecorded", "not recorded"),
            "ESTIMATE": ("estimate",)}
BARE = re.compile(r"[+~]?\d[\d,]*(?:\.\d+)?[KkMG]?(?:\s?[–-]\s?\d[\d,]*(?:\.\d+)?[KkMG]?)?")
SEPARATOR = re.compile(r"^\s*\|[\s:|-]+\|\s*$")
PERF_HEADER = re.compile(
    r"\bP\d|\bmax\b|\bmin\b|\bmean\b|latency|throughput|ns/op|ops/s|\bns\b|µs|"
    r"GB/s|naive|corrected|penalty|IOPS|\bRTT\b|\blag\b", re.I)
CELL_STRIP = re.compile(r"[*`]")


def cells(line: str) -> list[str]:
    return [c.strip() for c in line.strip().strip("|").split("|")]


def scan(text: str) -> dict[str, list[int]]:
    """{signature: [line numbers]} for one document."""
    found = {}
    lines = text.splitlines()
    perf_cols = None
    for i, line in enumerate(lines):
        is_row = line.lstrip().startswith("|")
        if not is_row:
            perf_cols = None
        if is_row and i + 1 < len(lines) and SEPARATOR.match(lines[i + 1]):
            perf_cols = {j for j, c in enumerate(cells(line)) if PERF_HEADER.search(c)}
            # A header names the statistics, but "At 10K msg/s" is a number too.
            tokens = [m.group(0) for c in cells(line) for m in TOKEN.finditer(c)]
            if tokens:
                found.setdefault(" · ".join(tokens), []).append(i + 1)
            continue
        if is_row and SEPARATOR.match(line):
            continue
        tokens = []
        for j, c in enumerate(cells(line) if is_row else [line]):
            units = [m.group(0) for m in TOKEN.finditer(c)]
            if not units and perf_cols and j in perf_cols:
                units = BARE.findall(CELL_STRIP.sub("", c))
            tokens += units
        if tokens:
            found.setdefault(" · ".join(tokens), []).append(i + 1)
    return found


def context(lines: list[str], n: int) -> str:
    """Line n (1-based), its table's header row and its enclosing headings: where
    a doc may say a number is withdrawn, unrecorded or an estimate."""
    i, out = n - 1, [lines[n - 1]]
    while i > 0 and lines[i - 1].lstrip().startswith("|"):
        i -= 1
    if i < n - 1:
        out.append(lines[i])
    fenced, inside = set(), False
    for k, l in enumerate(lines[:n]):   # a "# comment" in a code block is no heading
        if l.lstrip().startswith("```"):
            inside = not inside
        elif inside:
            fenced.add(k)
    level = 7
    for k in range(n - 1, -1, -1):      # each enclosing heading, ### up to #
        l = lines[k]
        depth = len(l) - len(l.lstrip("#"))
        if k not in fenced and 0 < depth < level and l[depth:depth + 1] == " ":
            out.append(l)
            level = depth
    return "\n".join(out).lower()


def read_rows(path: Path) -> tuple[dict, list[str]]:
    """{(file, signature): (row line index, [lines], source)} from PERF_SOURCES.md."""
    rows = {}
    if not path.exists():
        return rows, []
    text = path.read_text().splitlines()
    for k, line in enumerate(text):
        m = re.match(r"^\|\s*`?([\w./-]+\.md):([\d,]+)`?\s*\|(.*)\|(.*)\|(.*)\|\s*$", line)
        if not m:
            continue
        sig = " ".join(m.group(3).replace("`", "").split())
        rows[(m.group(1), sig)] = (k, [int(n) for n in m.group(2).split(",")], m.group(4).strip())
    return rows, text


def main() -> int:
    root = Path(sys.argv[1])
    mode = sys.argv[2] if len(sys.argv) > 2 else ""
    docs, texts = {}, {}
    for f in DOCS:
        texts[f] = (root / f).read_text().splitlines()
        for sig, ls in scan((root / f).read_text()).items():
            docs[(f, sig)] = ls
    rows, text = read_rows(root / SOURCES)

    missing = [k for k in docs if k not in rows]
    dead = [k for k in rows if k not in docs]
    drift = [k for k in rows if k in docs and rows[k][1] != docs[k]]
    legend = set(re.findall(r"^\| `([A-Z0-9-]+)` \|", "\n".join(text), re.M))
    nosrc = [k for k in rows if k in docs
             and not {s.strip() for s in rows[k][2].split(",")} <= legend]

    unmarked = []
    for k in rows:
        if k not in docs:
            continue
        for src in {s.strip() for s in rows[k][2].split(",")} & MUST_SAY.keys():
            unmarked += [(k[0], n, src) for n in docs[k]
                         if not any(w in context(texts[k[0]], n) for w in MUST_SAY[src])]

    if mode == "--missing":
        for f, sig in missing:
            print(f"| {f}:{','.join(map(str, docs[(f, sig)]))} | {sig} | TODO | |")
        return 0
    if mode == "--fix-lines":
        for k in drift:
            n = rows[k][0]
            text[n] = re.sub(r":[\d,]+", ":" + ",".join(map(str, docs[k])), text[n], count=1)
        (root / SOURCES).write_text("\n".join(text) + "\n")
        print(f"rewrote the line numbers of {len(drift)} rows")
        return 0

    for f, sig in missing:
        print(f"no {SOURCES} row: {f}:{','.join(map(str, docs[(f, sig)]))}  {sig}")
    for f, sig in dead:
        print(f"{SOURCES} row for numbers no longer in {f}: {sig}")
    for k in drift:
        print(f"{k[0]}: '{k[1]}' moved from lines {rows[k][1]} to {docs[k]} "
              f"(tests/doc_perf_numbers_sourced.py <root> --fix-lines)")
    for f, sig in nosrc:
        print(f"{SOURCES} row whose source is not in its Sources table: {f}  {sig}")
    for f, n, src in unmarked:
        print(f"{f}:{n}: a {src} number whose line, table header and heading do not say "
              f"{' or '.join(repr(w) for w in MUST_SAY[src])}")
    bad = len(missing) + len(dead) + len(drift) + len(nosrc) + len(unmarked)
    if bad:
        print(f"\n{bad} problems; {len(docs)} numbered lines in {len(DOCS)} docs")
        return 1
    print(f"{len(docs)} numbered lines in {len(DOCS)} docs, each with a source row")
    return 0


if __name__ == "__main__":
    sys.exit(main())
