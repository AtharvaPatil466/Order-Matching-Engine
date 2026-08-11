#!/usr/bin/env python3
"""Compare DeterministicBenchmark runs and fail the build on a latency regression.

Both sides are measured on the SAME CI runner inside the SAME job (see
.github/workflows/benchmark-gate.yml), so the delta reflects the code change and
not machine-to-machine variation. That is deliberately why there is no committed
baseline.json: Release builds use -march=native and GitHub's runner pool is
heterogeneous, so a stored baseline would be measured on different silicon than
the run it gates — which is exactly the failure mode that made the previous
version of this gate a permanent no-op.

Each side may be given several runs of the same binary. The MINIMUM is used: on a
shared runner the fastest sample is the one least contaminated by co-tenant
scheduler noise, so it is the stable statistic to compare. Comparing means or
single runs on shared hardware produces a gate that flakes and then gets disabled.

Usage:
    check_benchmark_regression.py --base base-*.json --head head-*.json
    check_benchmark_regression.py --selftest
"""
from __future__ import annotations

import argparse
import json
import math
import sys

# Per-metric thresholds, chosen from the measured noise floor rather than picked
# round. Method: 40 runs of one unchanged binary, reduced to eight disjoint
# min-of-5 groups (what this gate actually compares), then the worst delta
# between any two groups — i.e. the largest regression this gate could report
# for two builds that are byte-identical:
#
#     raw spread over 40 runs   p50 +24.6%    p99 +102.0%
#     min-of-5 false-delta      p50  +0.0%    p99  +18.5%
#
# So p50 after the min-of-N reduction is exact, and 10% there is a real, tight
# gate. p99 cannot be gated below ~20% without flaking no matter how the runs are
# reduced; 25% still catches a tail blowup, which is worth having since tail
# latency is the point of this engine.
#
# Do NOT tighten p99 toward p50's threshold without re-measuring. A gate that
# fails on unchanged code gets switched off, which is how the previous version of
# this gate ended up a no-op.
DEFAULT_GATES = {
    "p50": 10.0,
    "p99": 25.0,
}


def read_metric(paths: list[str], metric: str) -> float:
    """Return the minimum `latency_ns.<metric>` across `paths`.

    Any unreadable, malformed, or nonsensical input raises — a comparison that
    cannot be made must fail the build, never silently pass.
    """
    values = []
    for path in paths:
        try:
            with open(path) as handle:
                data = json.load(handle)
        except (OSError, json.JSONDecodeError) as exc:
            raise SystemExit(f"{path}: cannot read benchmark JSON ({exc})")
        try:
            value = float(data["latency_ns"][metric])
        except (KeyError, TypeError, ValueError) as exc:
            raise SystemExit(f"{path}: no numeric latency_ns.{metric} ({exc})")
        # NaN and inf must be rejected explicitly: every comparison against NaN
        # is False, so `value <= 0` lets it through, `delta_pct` becomes NaN, and
        # `delta_pct > threshold` is False — the gate reports "ok" on unusable
        # data. That silent pass is the exact failure mode this file exists to
        # prevent, so the finiteness check comes first.
        if not math.isfinite(value) or value <= 0:
            raise SystemExit(f"{path}: latency_ns.{metric} is {value}; "
                             "expected a finite value > 0")
        values.append(value)
    if not values:
        raise SystemExit(f"no benchmark runs supplied for {metric}")
    return min(values)


def compare(base_paths: list[str], head_paths: list[str],
            gates: dict[str, float] | None = None) -> int:
    """Print a per-metric comparison. Return 0 if all pass, 1 if any regressed."""
    gates = DEFAULT_GATES if gates is None else gates
    regressed = []
    for metric, threshold_pct in gates.items():
        base = read_metric(base_paths, metric)
        head = read_metric(head_paths, metric)
        delta_pct = ((head - base) / base) * 100.0
        verdict = "REGRESSED" if delta_pct > threshold_pct else "ok"
        print(f"{metric:>6}: base {base:9.1f} ns -> head {head:9.1f} ns "
              f"({delta_pct:+6.1f}%)  [limit +{threshold_pct:.0f}%]  {verdict}")
        if delta_pct > threshold_pct:
            regressed.append(f"{metric} +{delta_pct:.1f}% (limit +{threshold_pct:.0f}%)")

    if regressed:
        print("\nFAILED: " + ", ".join(regressed))
        return 1
    print("\nPASSED: every gated metric is within its threshold of the merge base.")
    return 0


def selftest() -> int:
    import os
    import tempfile

    def write(p99: float, p50: float = 100.0) -> str:
        fd, path = tempfile.mkstemp(suffix=".json")
        with os.fdopen(fd, "w") as handle:
            json.dump({"latency_ns": {"p50": p50, "p99": p99}}, handle)
        return path

    base = write(100.0)
    only99 = {"p99": 10.0}

    # Under / over the threshold.
    assert compare([base], [write(109.0)], only99) == 0
    assert compare([base], [write(111.0)], only99) == 1
    # An improvement is never a failure.
    assert compare([base], [write(80.0)], only99) == 0
    # min-of-N: the clean run wins, so head is 80 and not 111.
    assert compare([base], [write(111.0), write(80.0)], only99) == 0
    # ...and it applies to the base side too: base becomes 100, not 130.
    assert compare([base, write(130.0)], [write(111.0)], only99) == 1
    # A regression in ANY gated metric fails, even when the other is flat.
    assert compare([base], [write(100.0, p50=130.0)]) == 1
    # Per-metric thresholds are independent: +18% is a pass on p99 (limit 25)
    # and would be a failure on p50 (limit 10). This is the measured noise-floor
    # split; a single shared threshold cannot express it.
    assert compare([base], [write(118.0, p50=100.0)]) == 0
    assert compare([base], [write(100.0, p50=118.0)]) == 1

    # Unusable input fails the build rather than passing it. NaN and inf are in
    # this list because they are the quiet ones: json.load accepts the bare NaN
    # and Infinity tokens, every comparison against NaN is False, and a gate that
    # only checks `value <= 0` therefore prints "ok" and exits 0 on garbage.
    nan, inf = float("nan"), float("inf")
    for bad in ("/nonexistent/benchmark.json", write(0.0), write(nan), write(inf)):
        try:
            read_metric([bad], "p99")
        except SystemExit:
            pass
        else:
            raise AssertionError(f"{bad} should have raised SystemExit")

    # ...and the same through the full compare() path, which is how it would
    # actually reach the gate: every gated metric non-finite must fail the build.
    try:
        compare([base], [write(nan, p50=nan)])
    except SystemExit:
        pass
    else:
        raise AssertionError("all-NaN head should have raised SystemExit")

    print("selftest OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", nargs="+", default=[],
                        help="benchmark JSON from the merge base (repeatable)")
    parser.add_argument("--head", nargs="+", default=[],
                        help="benchmark JSON from the PR head (repeatable)")
    parser.add_argument("--metric", action="append", dest="metrics",
                        help="restrict to this latency_ns key (repeatable; "
                             f"default: {', '.join(DEFAULT_GATES)})")
    parser.add_argument("--threshold", type=float, default=None,
                        help="override the per-metric thresholds with one value, "
                             "percent. Read the noise-floor note above before "
                             "tightening anything.")
    parser.add_argument("--selftest", action="store_true",
                        help="run the built-in assertions and exit")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if not args.base or not args.head:
        parser.error("--base and --head are both required")

    metrics = args.metrics or list(DEFAULT_GATES)
    unknown = [m for m in metrics if m not in DEFAULT_GATES and args.threshold is None]
    if unknown:
        parser.error(f"no default threshold for {', '.join(unknown)}; pass --threshold")
    gates = {m: (args.threshold if args.threshold is not None else DEFAULT_GATES[m])
             for m in metrics}
    return compare(args.base, args.head, gates)


if __name__ == "__main__":
    sys.exit(main())
