#!/usr/bin/env python3
"""Configure and build the bcs_engine pybind11 module.

Run with the project venv so the right pybind11 / interpreter is used:

    bcs_research/.venv/bin/python bcs_research/build_bridge.py

The module is built into bcs_research/build/ and discovered by tests via
tests/conftest.py (which adds that directory to sys.path).
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
BINDINGS_DIR = HERE / "bindings"
BUILD_DIR = HERE / "build"


def main() -> int:
    try:
        import pybind11
    except ImportError:
        print("pybind11 not installed in this interpreter. Run with the venv:\n"
              "  bcs_research/.venv/bin/python bcs_research/build_bridge.py",
              file=sys.stderr)
        return 1

    BUILD_DIR.mkdir(exist_ok=True)
    configure = [
        "cmake",
        "-S", str(BINDINGS_DIR),
        "-B", str(BUILD_DIR),
        f"-Dpybind11_DIR={pybind11.get_cmake_dir()}",
        f"-DPython_EXECUTABLE={sys.executable}",
        f"-DPYTHON_EXECUTABLE={sys.executable}",
        "-DCMAKE_BUILD_TYPE=Release",
    ]
    subprocess.run(configure, check=True)
    subprocess.run(["cmake", "--build", str(BUILD_DIR), "-j"], check=True)

    built = sorted(BUILD_DIR.glob("bcs_engine*.so"))
    if not built:
        print("Build finished but no bcs_engine*.so found.", file=sys.stderr)
        return 1
    print("Built module(s):")
    for so in built:
        print(f"  {so}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
