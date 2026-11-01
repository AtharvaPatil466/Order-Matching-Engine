#!/usr/bin/env python3
"""Every performance number in the docs has a row in docs/PERF_SOURCES.md.

usage: doc_perf_numbers_sourced.py <repo root> [--missing | --fix-lines]

A "performance number" is a number carrying a time, rate, size or ratio unit
(261 ns, 2.80M ops/s, 10.3 MiB, 3.8x), or any number in a table column whose
header names a latency/throughput statistic (P50, Max, ns/op, IOPS, ...). The
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
        r"(?:×|x)(?!\s*[\d~]))")  # a ratio, not "50K × 86400s"
TOKEN = re.compile(rf"(?<![\w.%]){NUM}[KkMG]?[ _]?{UNIT}(?![\w])")
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
            continue  # the header names the statistics; it carries no numbers
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
    docs = {}
    for f in DOCS:
        for sig, ls in scan((root / f).read_text()).items():
            docs[(f, sig)] = ls
    rows, text = read_rows(root / SOURCES)

    missing = [k for k in docs if k not in rows]
    dead = [k for k in rows if k not in docs]
    drift = [k for k in rows if k in docs and rows[k][1] != docs[k]]
    legend = set(re.findall(r"^\| `([A-Z0-9-]+)` \|", "\n".join(text), re.M))
    nosrc = [k for k in rows if k in docs
             and not {s.strip() for s in rows[k][2].split(",")} <= legend]

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
    bad = len(missing) + len(dead) + len(drift) + len(nosrc)
    if bad:
        print(f"\n{bad} problems; {len(docs)} numbered lines in {len(DOCS)} docs")
        return 1
    print(f"{len(docs)} numbered lines in {len(DOCS)} docs, each with a source row")
    return 0


if __name__ == "__main__":
    sys.exit(main())
