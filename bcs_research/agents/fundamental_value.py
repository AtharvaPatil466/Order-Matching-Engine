"""FundamentalValueProcess — the exogenous "true value" V(t).

V evolves as a discrete random walk. In Phase 2 HFTs observe V with a small
latency and market makers with a larger one; the gap between what each sees is
the BCS arbitrage. Values live in the engine's fixed-point price space
(price 100.00 == 100 * PRICE_PRECISION) so agents derive integer order prices
directly.

Volatility is specified PER STEP (dv = sigma * N(0,1)); dt_us is the wall clock
the scheduler advances by and is retained only as metadata. Per-step sigma
keeps the random-walk variance independent of the chosen tick size.
"""
from __future__ import annotations

import bisect

import numpy as np


class FundamentalValueProcess:
    def __init__(self, v0, sigma, dt_us=1000, seed=0):
        if dt_us <= 0:
            raise ValueError("dt_us must be positive")
        if sigma < 0:
            raise ValueError("sigma must be non-negative")
        self.v0 = float(v0)
        self.sigma = float(sigma)
        self.dt_us = int(dt_us)
        self.current_v = float(v0)
        self._rng = np.random.default_rng(seed)
        self._times = [0]
        self._values = [float(v0)]

    def step(self, t):
        """Advance V by one step, recording it at time t. Returns the new V."""
        dv = self.sigma * self._rng.standard_normal()
        self.current_v += float(dv)
        self._times.append(int(t))
        self._values.append(self.current_v)
        return self.current_v

    def observe(self, t, latency_us=0):
        """Most recent V at or before (t - latency_us)."""
        target = int(t) - int(latency_us)
        if target <= self._times[0]:
            return self._values[0]
        idx = bisect.bisect_right(self._times, target) - 1
        if idx < 0:
            idx = 0
        return self._values[idx]

    @property
    def value(self):
        return self.current_v
