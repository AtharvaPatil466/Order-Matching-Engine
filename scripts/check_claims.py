#!/usr/bin/env python3
"""Roadmap 2.4: no guarantee goes in a doc without a test that would fail if it
were false.

Every `<!-- claim:ID -->` marker in the claim docs needs a row in
docs/CLAIMS.md, every row needs a marker, and every test a row names must exist:

  `Name`       a test in `ctest -N -L project` (a gtest case by its discovered
               name, Suite.Case), so every CI lane runs it
  `tla:Cfg`    spec/Cfg.cfg exists and .github/workflows/tla.yml checks it

A row whose test column says `no test — pending decision 1.x` passes only if
its ID is in PENDING with that decision.

usage: check_claims.py <repo root> <build dir> [<ctest executable>]
"""
import re
import subprocess
import sys
from pathlib import Path

DOCS = ["README.md", "docs/Compliance.md", "docs/Verification.md",
        "docs/ProductionReadiness.md"]

# Claims about code whose future waits on a user decision: replication (roadmap
# 1.1) and the unshipped protocol stacks (1.2). Nothing else may go untested.
PENDING = {
    "repl-chaos": "1.1",
    "repl-lease-promotion": "1.1",
}

MARKER = re.compile(r"<!-- claim:([a-z][a-z0-9-]*) -->")
ROW_ID = re.compile(r"`?([a-z][a-z0-9-]*)`?")
PENDING_CELL = re.compile(r"no test — pending decision (1\.\d+)")
CTEST_LINE = re.compile(r"^\s*Test\s+#\d+:\s+(\S+)\s*$")


def ctest_names(build: str, ctest: str) -> set[str]:
    out = subprocess.run([ctest, "--test-dir", build, "-N", "-L", "project"],
                         capture_output=True, text=True)
    names = {m.group(1) for line in out.stdout.splitlines()
             if (m := CTEST_LINE.match(line))}
    if out.returncode != 0 or not names:
        sys.exit(f"FAIL: `{ctest} -N` listed no project tests in {build}\n{out.stderr}")
    return names


def read_rows(claims: Path) -> tuple[dict[str, str], list[str]]:
    """ID -> test cell of each table row in CLAIMS.md (columns: ID|Where|Claim|Test|...)."""
    rows, dupes = {}, []
    for line in claims.read_text().splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if not line.startswith("|") or len(cells) < 4:
            continue
        m = ROW_ID.fullmatch(cells[0])
        if not m:
            continue                      # header or separator row
        if m.group(1) in rows:
            dupes.append(m.group(1))
        rows[m.group(1)] = cells[3]
    return rows, dupes


def check_evidence(cid: str, cell: str, root: Path, tests: set[str],
                   tla_yml: str) -> list[str]:
    pending = PENDING_CELL.search(cell)
    if pending:
        if PENDING.get(cid) != pending.group(1):
            return [f"{cid}: '{pending.group(0)}' is not allowlisted for this claim"]
        return []
    names = re.findall(r"`([^`]+)`", cell)
    if not names:
        return [f"{cid}: names no test"]
    errors = []
    for name in names:
        if name.startswith("tla:"):
            cfg = name[4:]
            if not (root / "spec" / f"{cfg}.cfg").is_file():
                errors.append(f"{cid}: spec/{cfg}.cfg does not exist")
            elif not re.search(rf"\b{re.escape(cfg)}\b", tla_yml):
                errors.append(f"{cid}: tla.yml does not check {cfg}")
        elif name not in tests:
            errors.append(f"{cid}: `{name}` is not a project test in ctest -N")
    return errors


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        sys.exit(__doc__)
    root, build = Path(argv[1]), argv[2]
    ctest = argv[3] if len(argv) > 3 else "ctest"

    marked = {}
    for doc in DOCS:
        for m in MARKER.finditer((root / doc).read_text()):
            marked.setdefault(m.group(1), doc)

    claims = root / "docs" / "CLAIMS.md"
    if not claims.is_file():
        sys.exit(f"FAIL: docs/CLAIMS.md is missing; {len(marked)} claim marker(s) in the docs")
    rows, dupes = read_rows(claims)
    tests = ctest_names(build, ctest)
    # Comment lines out: tla.yml's own comments name specs it does not run.
    tla_yml = "\n".join(line for line in
                        (root / ".github" / "workflows" / "tla.yml").read_text().splitlines()
                        if not line.lstrip().startswith("#"))

    errors = [f"{cid}: two rows in CLAIMS.md" for cid in dupes]
    if not marked:
        errors.append("no claim markers in " + ", ".join(DOCS))
    errors += [f"{cid}: marked in {doc} but has no CLAIMS.md row"
               for cid, doc in sorted(marked.items()) if cid not in rows]
    errors += [f"{cid}: CLAIMS.md row has no marker in any claim doc"
               for cid in sorted(rows) if cid not in marked]
    errors += [f"{cid}: allowlisted as pending but has no CLAIMS.md row"
               for cid in sorted(PENDING) if cid not in rows]
    for cid, cell in sorted(rows.items()):
        errors += check_evidence(cid, cell, root, tests, tla_yml)

    for e in errors:
        print("FAIL:", e)
    print(f"{len(marked)} claims marked, {len(rows)} rows, {len(errors)} problem(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
