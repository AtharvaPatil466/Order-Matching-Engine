#!/usr/bin/env python3
"""Roadmap 2.4: no guarantee goes in a doc without a test that would fail if it
were false.

Every `<!-- claim:ID -->` marker in the claim docs needs a row in
docs/CLAIMS.md, every row needs a marker, and every test a row names must exist:

  `Name`       a test in `ctest -N -L project` (a gtest case by its discovered
               name, Suite.Case), so every CI lane runs it
  `tla:Cfg`    spec/Cfg.cfg exists and .github/workflows/tla.yml checks it

Existing is not enough: a row could name a real test that never touches what
the claim is about (DOCS-3: a checkpoint claim cited a test with no
checkpoint in it). So every row also has an entry in CLAIMS.md's evidence
table naming the calls its tests must make, and each named test's source
(the gtest file that defines the case, the executable's sources, or the shell
or Python script ctest runs) must contain at least one of them.

ponytail: a text search, not a proof that the test asserts the claim. It
catches a test that never reaches the subject, which is the failure seen.

A row whose test column says `no test — pending decision 1.x` passes only if
its ID is in PENDING with that decision.

usage: check_claims.py <repo root> <build dir> [<ctest executable>]
"""
import re
import subprocess
import sys
from pathlib import Path

DOCS = ["README.md", "Project_Overview.md", "docs/Compliance.md",
        "docs/Verification.md", "docs/ProductionReadiness.md"]

# Claims about code whose future waits on a user decision, as {id: decision}.
# Nothing else may go untested.
PENDING = {}

MARKER = re.compile(r"<!-- claim:([a-z][a-z0-9-]*) -->")
ROW_ID = re.compile(r"`?([a-z][a-z0-9-]*)`?")
PENDING_CELL = re.compile(r"no test — pending decision (1\.\d+)")
CTEST_LINE = re.compile(r"^\s*Test\s+#\d+:\s+(\S+)\s*$")
EVIDENCE_HEADING = "## Evidence"
ADD_TEST = re.compile(r"add_test\(\s*NAME\s+(\S+)\s+COMMAND\s+([^)]*)\)", re.S)
ADD_EXE = re.compile(r"add_executable\(\s*(\S+)\s+([^)]*)\)", re.S)


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


def read_evidence(claims: Path) -> dict[str, list[str]]:
    """ID -> the calls its tests must contain, from the table under EVIDENCE_HEADING."""
    text = claims.read_text()
    if EVIDENCE_HEADING not in text:
        return {}
    terms = {}
    for line in text.split(EVIDENCE_HEADING, 1)[1].splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if line.startswith("|") and len(cells) >= 2 and (m := ROW_ID.fullmatch(cells[0])):
            terms[m.group(1)] = re.findall(r"`([^`]+)`", cells[1])
    return terms


def test_sources(root: Path) -> tuple[dict[str, list[Path]], list[Path]]:
    """(ctest name -> source files, every test .cpp) from tests/CMakeLists.txt."""
    tdir = root / "tests"
    cmake = (tdir / "CMakeLists.txt").read_text()
    exes = {m.group(1): [tdir / f for f in m.group(2).split()
                         if f.endswith((".cpp", ".h"))]
            for m in ADD_EXE.finditer(cmake)}
    by_test = {}
    for m in ADD_TEST.finditer(cmake):
        files = []
        for tok in m.group(2).split():
            tok = tok.replace("${CMAKE_CURRENT_SOURCE_DIR}/", "") \
                     .replace("${CMAKE_SOURCE_DIR}/", "../")
            if tok in exes:
                files += exes[tok]
            elif tok.endswith((".sh", ".py")):
                files.append(tdir / tok)
        by_test[m.group(1)] = files
    return by_test, sorted(tdir.glob("*.cpp"))


def gtest_files(name: str, cpps: list[Path]) -> list[Path]:
    """Files defining the gtest case Prefix/Suite.Case/Param."""
    core = next((p for p in name.split("/") if "." in p), name)
    suite, _, case = core.partition(".")
    pat = re.compile(rf"TEST(?:_F|_P)?\(\s*{re.escape(suite)}\s*,\s*{re.escape(case)}\s*\)")
    return [f for f in cpps if pat.search(f.read_text(errors="replace"))]


def check_exercised(cid: str, cell: str, terms: list[str], sources, cpps) -> list[str]:
    if not terms:
        return [f"{cid}: no evidence entry in CLAIMS.md naming what its tests must call"]
    errors = []
    for name in re.findall(r"`([^`]+)`", cell):
        if name.startswith("tla:"):
            continue
        files = sources.get(name) or gtest_files(name, cpps)
        if not files:
            errors.append(f"{cid}: cannot find the source of `{name}`")
            continue
        if absent := [f.name for f in files if not f.is_file()]:
            errors.append(f"{cid}: `{name}` names missing source {', '.join(absent)}")
            continue
        text = "".join(f.read_text(errors="replace") for f in files)
        if not any(t in text for t in terms):
            errors.append(f"{cid}: `{name}` ({', '.join(f.name for f in files)}) "
                          f"contains none of {', '.join(terms)}")
    return errors


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
    evidence = read_evidence(claims)
    sources, cpps = test_sources(root)
    errors += [f"{cid}: evidence entry has no CLAIMS.md row"
               for cid in sorted(evidence) if cid not in rows]
    for cid, cell in sorted(rows.items()):
        errors += check_evidence(cid, cell, root, tests, tla_yml)
        if not PENDING_CELL.search(cell) and any(
                not n.startswith("tla:") for n in re.findall(r"`([^`]+)`", cell)):
            errors += check_exercised(cid, cell, evidence.get(cid, []), sources, cpps)

    for e in errors:
        print("FAIL:", e)
    print(f"{len(marked)} claims marked, {len(rows)} rows, {len(errors)} problem(s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
