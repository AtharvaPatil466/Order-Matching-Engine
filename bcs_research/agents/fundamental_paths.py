"""Fundamental paths for the event model: diffusion, news jumps, a lead instrument.

The tick model's fundamental (fundamental_value.py) is a Gaussian random walk
stepped every 100 ms. BCS sniping opportunities come from discrete public
news and from a correlated instrument that moves first; a random walk in
100 ms ticks has neither. These paths supply both. Each is generated whole
at construction, seed-deterministically, so an event scheduler can merge
its updates into the event queue and the accounting can mark to the true V.

Units. Time is integer nanoseconds. Values are floats in the engine's
fixed-point price space (100.00 == 100 * PRICE_PRECISION), as in
fundamental_value.py. Physical parameters are converted once, in
`JumpDiffusionPath.from_calibration`, as fractions of v0:

    interval_ns = round(update_interval_ms * 1e6)
    sigma_step  = sigma_bp_per_sqrt_s * 1e-4 * v0 * sqrt(interval_ns / 1e9)
    jump_scale  = jump_scale_bp * 1e-4 * v0      (Laplace b; jump sd = sqrt(2) * b)

So over one second V moves by sigma_bp_per_sqrt_s bp of v0 (one sd) from
diffusion alone, and jumps add jump_intensity_per_s * 2 * b^2 of variance.
sigma_bp_per_sqrt_s must therefore be the DIFFUSIVE volatility, with jumps
excluded, or the jump variance is counted twice.

ponytail: moves are arithmetic in price space, not geometric. That keeps the
nesting with fundamental_value.py bit-exact, and it is accurate while total
moves stay << v0 (about 60 bp over an hour at 1 bp/sqrt(s)). Switch to
log-price steps for multi-day runs.
"""
from __future__ import annotations

import math

import numpy as np

from contracts import SignalUpdate

NS_PER_S = 1_000_000_000

CALIBRATION_KEYS = ("v0", "sigma_bp_per_sqrt_s", "jump_intensity_per_s",
                    "jump_size_dist", "jump_scale_bp", "update_interval_ms")

# Laplace is the default because the theory lane's BCS closed forms are derived
# for it; normal (Merton) jumps are the textbook comparison.
_JUMP_DRAWS = {
    "laplace": lambda rng, scale, n: rng.laplace(0.0, scale, n),
    "normal": lambda rng, scale, n: rng.normal(0.0, scale, n),
}


def _bp_of(v0: float, bp: float) -> float:
    return bp * 1e-4 * v0


def _value_index(times: np.ndarray, t_ns: int) -> int:
    """Index of the last event at or before t, -1 if none."""
    return int(np.searchsorted(times, t_ns, side="right")) - 1


class JumpDiffusionPath:
    """Brownian diffusion on a fixed grid, plus compound-Poisson news jumps.

    V is piecewise constant and every change is a public update: one Gaussian
    step at each grid point t = 0, interval, 2*interval, ... (as in the tick
    model, the first step lands at t = 0), and one jump at each Poisson
    arrival, anywhere in integer-ns time. Jump updates carry is_jump=True.

    The diffusion is drawn first, one normal per grid point, so for the same
    seed the diffusion is identical with or without jumps (common random
    numbers across jump settings) and, with no jumps, identical to
    FundamentalValueProcess(seed) stepped on the grid.

    ponytail: V is constant between grid points, so diffusion between two
    publications is invisible until the next one. Fine at 100 ms cadence;
    add a finer hidden grid if a strategy depends on intra-interval drift.
    """

    def __init__(self, v0: float, sigma_step: float, update_interval_ns: int,
                 duration_ns: int, seed: int = 0, jump_intensity_per_s: float = 0.0,
                 jump_scale: float = 0.0, jump_size_dist: str = "laplace"):
        if update_interval_ns <= 0 or duration_ns <= 0:
            raise ValueError("update_interval_ns and duration_ns must be positive")
        if sigma_step < 0 or jump_scale < 0 or jump_intensity_per_s < 0:
            raise ValueError("sigma_step, jump_scale and jump_intensity_per_s must be >= 0")
        if jump_size_dist not in _JUMP_DRAWS:
            raise ValueError(f"jump_size_dist must be one of {sorted(_JUMP_DRAWS)}")
        self.v0 = float(v0)
        self.update_interval_ns = int(update_interval_ns)
        self.duration_ns = int(duration_ns)

        rng = np.random.default_rng(seed)
        grid = np.arange(0, self.duration_ns, self.update_interval_ns, dtype=np.int64)
        steps = sigma_step * rng.standard_normal(len(grid))
        n_jumps = rng.poisson(jump_intensity_per_s * self.duration_ns / NS_PER_S)
        jump_t = np.sort(rng.integers(0, self.duration_ns, n_jumps))
        jumps = _JUMP_DRAWS[jump_size_dist](rng, jump_scale, n_jumps)

        t = np.concatenate([grid, jump_t])
        order = np.argsort(t, kind="stable")
        t = t[order]
        dv = np.concatenate([steps, jumps])[order]
        flags = np.concatenate([np.zeros(len(grid), bool), np.ones(n_jumps, bool)])[order]
        # Sequential cumsum from v0: the same float additions, in the same order,
        # as FundamentalValueProcess.step, which is what makes the nesting exact.
        v = np.cumsum(np.concatenate([[self.v0], dv]))[1:]

        # A jump landing exactly on a grid point would give two updates at one
        # t_ns; publish one, carrying the combined move, flagged as a jump.
        is_last = np.r_[t[1:] != t[:-1], True]
        starts = np.flatnonzero(np.r_[True, t[1:] != t[:-1]])
        self._t = t[is_last]
        self._v = v[is_last]
        self._is_jump = np.logical_or.reduceat(flags, starts)

    @classmethod
    def from_calibration(cls, d: dict, duration_ns: int, seed: int = 0) -> "JumpDiffusionPath":
        """Build from the DATA lane's calibration dict (physical units, see module doc).

        Extra keys (provenance, sample sizes) are ignored; a missing one raises.
        `v0` is the starting value in fixed-point price space, not a BTC price:
        every other parameter is relative to it.
        """
        missing = [k for k in CALIBRATION_KEYS if k not in d]
        if missing:
            raise ValueError(f"calibration dict is missing {missing}")
        v0 = float(d["v0"])
        interval_ns = round(float(d["update_interval_ms"]) * 1_000_000)
        return cls(v0=v0,
                   sigma_step=_bp_of(v0, float(d["sigma_bp_per_sqrt_s"]))
                   * math.sqrt(interval_ns / NS_PER_S),
                   update_interval_ns=interval_ns,
                   duration_ns=duration_ns,
                   seed=seed,
                   jump_intensity_per_s=float(d["jump_intensity_per_s"]),
                   jump_scale=_bp_of(v0, float(d["jump_scale_bp"])),
                   jump_size_dist=d["jump_size_dist"])

    def value_at(self, t_ns: int) -> float:
        i = _value_index(self._t, t_ns)
        return self.v0 if i < 0 else float(self._v[i])

    def updates(self) -> list[SignalUpdate]:
        return [SignalUpdate(int(t), float(v), bool(j))
                for t, v, j in zip(self._t, self._v, self._is_jump)]


class CorrelatedSignalPath:
    """A lead instrument L publishes; the traded asset's V is never published.

        V(t)      = v0 + beta * (L(t) - v0) + I(t)
        signal(t) = v0 + beta * (L(t) - v0)          (what updates() carries)

    L is any JumpDiffusionPath started at v0 (its diffusion and jumps are the
    public news). Updates carry L's move already translated into this asset's
    price space, i.e. E[V | L], so agents need not know beta. I is the
    idiosyncratic component: a driftless Gaussian walk on L's grid with
    `tracking_error_bp_per_sqrt_s`, the sd of (V - signal) after one second.
    Zero tracking error and beta = 1 make V equal the last signal exactly.

    The idiosyncratic draws use their own stream (entropy [seed, 1]), so
    passing the lead's seed here does not correlate I with L's diffusion.

    ponytail: I is a random walk (a factor model in returns), so the basis
    between signal and V never mean-reverts. Make it an OU process if a
    cointegrated pair (perp vs spot) is the target.
    """

    def __init__(self, lead: JumpDiffusionPath, beta: float = 1.0,
                 tracking_error_bp_per_sqrt_s: float = 0.0, seed: int = 0):
        if tracking_error_bp_per_sqrt_s < 0:
            raise ValueError("tracking_error_bp_per_sqrt_s must be >= 0")
        self.lead = lead
        self.beta = float(beta)
        self.v0 = lead.v0
        self.duration_ns = lead.duration_ns
        self._interval_ns = lead.update_interval_ns
        n_grid = -(-lead.duration_ns // lead.update_interval_ns)
        sigma_step = (_bp_of(lead.v0, tracking_error_bp_per_sqrt_s)
                      * math.sqrt(lead.update_interval_ns / NS_PER_S))
        rng = np.random.default_rng([seed, 1])
        self._idio = np.cumsum(sigma_step * rng.standard_normal(n_grid))

    def _signal(self, lead_value: float) -> float:
        return self.v0 + self.beta * (lead_value - self.v0)

    def value_at(self, t_ns: int) -> float:
        idio = 0.0 if t_ns < 0 else float(self._idio[min(t_ns // self._interval_ns,
                                                         len(self._idio) - 1)])
        return self._signal(self.lead.value_at(t_ns)) + idio

    def updates(self) -> list[SignalUpdate]:
        return [SignalUpdate(u.t_ns, self._signal(u.value), u.is_jump)
                for u in self.lead.updates()]
