"""Closed forms of Budish, Cramton & Shim (2015), the theory the simulator must reproduce.

Source: Budish, E., Cramton, P. & Shim, J. (2015). "The High-Frequency Trading
Arms Race: Frequent Batch Auctions as a Market Design Response." QJE 130(4),
1547-1621, doi:10.1093/qje/qjv027 (open-access version at
ericbudish.org/files/high_frequency_trading_arms_race.pdf). Every equation and
proposition number below was read off that published text; anything derived
here rather than in the paper says so.

Why this module exists: a reviewer asked that the simulator recover BCS's own
equilibrium before it is used to say anything BCS did not. That needs the
theory as code, so the replication can compare against numbers, not prose.

The model (BCS §VI.A). A public signal y jumps as a compound Poisson process
at rate lam_jump; the jump distribution is symmetric with mean zero and J is
the ABSOLUTE jump size. Investors arrive at rate lam_invest and buy or sell one
unit at market. N equally fast trading firms sort into 1 liquidity provider
(LP) and N-1 stale-quote snipers. The LP quotes one unit at y - s/2 and
y + s/2: BCS's s is the FULL bid-ask spread. This module works in the
half-spread h = s/2 throughout, because that is what a quote's distance from
y is and what the simulator sets.

Units: rates are per unit time (any unit, used consistently); h, J and the
jump scale b share one price unit (the engine's fixed-point ticks in the
simulator). Rents are price units per unit time.

The one integral everything rests on is the per-jump sniping prize

    prize(h) = Pr(J > h) * E[J - h | J > h] = E[(J - h)^+] = integral_h^inf Pr(J > x) dx,

computed from J's survival function, so the general solver takes any
scipy.stats distribution of J.

Not verified / deliberate departures, stated once here:
  - BCS assume F_jump has finite bounded support (§VI.A). The Laplace jumps
    the simulator uses have unbounded support. Equation (3) is still well
    posed (its LHS rises from 0, its RHS falls from lam_jump * E[J] > 0, both
    continuous), so the root exists and is unique; we have not checked that
    nothing else in BCS's proofs (Online Appendix B, not read) needs the bound.
  - Proposition 8(i) prints the CLOB spread as N* c_speed / lam_invest. By
    equation (7) that is s*/2, the half-spread, not s*. We read it as a
    half-spread slip in the proposition and follow equation (7).
  - fba_jitter_exposure's sniper-presence refinement and fba_half_spread are
    derived here, not in the paper (see their docstrings).
"""
from __future__ import annotations

import math

import numpy as np
from scipy import integrate, optimize, special, stats


def sniping_prize(h: float, jump) -> float:
    """Pr(J > h) * E[J - h | J > h]: the expected sniping gain per jump at half-spread h.

    `jump` is a frozen scipy.stats distribution of the ABSOLUTE jump size J,
    e.g. laplace_jump(b), stats.halfnorm(scale=sigma), stats.uniform(0, a).
    BCS Prop 1(v) and Prop 5 call lam_jump times this the arms-race prize.

    Integrates Pr(J > x) over [h, sup J] in units of E[J], so the same
    quadrature works whether prices are in ticks or units and whether the
    support is bounded (BCS's assumption) or not (Laplace).
    """
    if h < 0:
        raise ValueError(f"half-spread must be >= 0, got {h}")
    lo, hi = jump.support()
    if h >= hi:
        return 0.0
    scale = jump.mean()
    tail, _ = integrate.quad(lambda y: jump.sf(scale * y), max(h, lo) / scale, hi / scale, limit=200)
    return max(lo - h, 0.0) + scale * tail


def half_spread(lam_invest: float, lam_jump: float, jump) -> float:
    """Root h of lam_invest * h = lam_jump * prize(h), for any jump-size law.

    With the raw jump rate this is BCS equation (3), the equilibrium
    half-spread s*/2, at which a firm is indifferent between providing
    liquidity (equation 1) and sniping (equation 2). It does not depend on N.

    The same indifference with a scaled jump rate gives the other spreads the
    replication needs: lam_jump * (N-1)/N is the LP's own breakeven with no
    outside option, lam_jump * phi is residual sniping exposure phi under a
    batch auction. Solved with brentq; the bracket [0, lam_jump * E[J] /
    lam_invest] always holds the root because prize(h) <= prize(0) = E[J].
    """
    if lam_invest <= 0:
        raise ValueError(f"lam_invest must be > 0, got {lam_invest}")
    if lam_jump < 0:
        raise ValueError(f"lam_jump must be >= 0, got {lam_jump}")
    upper = lam_jump * jump.mean() / lam_invest
    if upper == 0.0:
        return 0.0
    def gap(h):
        return lam_invest * h - lam_jump * sniping_prize(h, jump)
    return optimize.brentq(gap, 0.0, upper, xtol=1e-12 * upper, rtol=1e-14)


def laplace_half_spread(lam_invest: float, lam_jump: float, b: float) -> float:
    """Closed form of half_spread for Laplace(0, b) signed jumps.

    |X| is exponential with mean b, so Pr(J > h) = exp(-h/b) and, being
    memoryless, E[J - h | J > h] = b: prize(h) = b * exp(-h/b). Equation (3)
    becomes lam_invest * h = lam_jump * b * exp(-h/b); with u = h/b that is
    u * e^u = lam_jump / lam_invest, so h = b * W(lam_jump / lam_invest) with
    W the principal Lambert W. Derived here from equation (3).
    """
    if lam_invest <= 0 or lam_jump < 0 or b < 0:
        raise ValueError("need lam_invest > 0, lam_jump >= 0, b >= 0")
    return b * float(special.lambertw(lam_jump / lam_invest).real)


def laplace_jump(b: float):
    """J = |Laplace(0, b)|, which is exponential with mean b."""
    return stats.expon(scale=b)


def race_loss_probability(n_firms: int) -> float:
    """(N-1)/N: the LP's cancel loses a tie broken uniformly over N messages (Prop 1(iii))."""
    if n_firms < 1:
        raise ValueError(f"need at least one firm, got {n_firms}")
    return (n_firms - 1) / n_firms


def race_win_probability(lag: float, eps: float, n_snipers: int) -> float:
    """P(an LP lagging the snipers by `lag` cancels before all n_snipers snipe).

    Each message gets an independent U[0, eps] jitter on top of its fixed
    latency (BCS footnote 21's "small random latency"). The LP wins iff
    lag + U_LP < min_i U_i, which integrates to ((eps - lag)/eps)^(k+1)/(k+1)
    for 0 <= lag <= eps and 0 beyond. At lag 0 it is BCS's 1/N with
    N = k + 1. Derived here, not in BCS; eps = 0 means exact ties, broken
    uniformly, so the LP wins 1/N at lag 0 and never otherwise.
    """
    if lag < 0 or eps < 0 or n_snipers < 0:
        raise ValueError("need lag >= 0, eps >= 0, n_snipers >= 0")
    if eps == 0:
        return 1.0 / (n_snipers + 1) if lag == 0 else 0.0
    if lag >= eps:
        return 0.0
    return ((eps - lag) / eps) ** (n_snipers + 1) / (n_snipers + 1)


def depth_half_spreads(lam_invest: float, lam_jump: float, jump, qty_probs) -> list[float]:
    """Half-spread of the k-th unit of depth, BCS equation (4) / Proposition 2.

    Investors want k units with probability qty_probs[k-1] and take them all at
    once; the k-th unit only earns from investors wanting >= k units but is
    sniped in full, so lam_invest * Pr(q >= k) * h_k = lam_jump * prize(h_k).
    Strictly increasing in k (Prop 2(ii)): the book is thin.
    """
    probs = np.asarray(qty_probs, dtype=float)
    if probs.ndim != 1 or probs.size == 0 or np.any(probs <= 0) or not math.isclose(probs.sum(), 1.0):
        raise ValueError("qty_probs must be positive and sum to 1")
    tails = probs[::-1].cumsum()[::-1]
    return [half_spread(lam_invest * tail, lam_jump, jump) for tail in tails]


def free_entry_n(rent_rate: float, c_speed: float, integer: bool = False) -> float:
    """Equilibrium number of fast firms, BCS equation (7): N* c_speed = lam_invest s*/2.

    rent_rate is lam_invest * h* (= the arms-race prize, equation 3). BCS let N
    be real; with integer N it is the largest N whose firms still break even,
    floor(rent / c) (§VI.D). BCS assume N* >= 2, i.e. c_speed < rent/2
    (footnote 26); below that their equilibrium does not apply, so we refuse.
    """
    if c_speed <= 0:
        raise ValueError(f"c_speed must be > 0, got {c_speed}")
    if c_speed > rent_rate / 2:
        raise ValueError(f"c_speed={c_speed} > rent/2={rent_rate / 2}: N* < 2, outside BCS (footnote 26)")
    n_star = rent_rate / c_speed
    return float(math.floor(n_star)) if integer else n_star


def laplace_equilibrium(lam_invest: float, lam_jump: float, b: float, n_firms: int,
                        c_speed: float | None = None) -> dict:
    """Every number the replication spec predicts, for Laplace jumps and N firms.

    n_firms counts the LP: one LP plus n_firms - 1 snipers. Per-jump figures
    are per signal jump of any size. Rates are per unit time. With c_speed
    the free-entry block (equations 5-7, Prop 3-4) is added; its waste is
    BCS's social cost of the race, N* c_speed per unit time.
    """
    h = laplace_half_spread(lam_invest, lam_jump, b)
    p_race = math.exp(-h / b)                       # Pr(J > h): the jump leaves a snipe-able quote
    prize = b * p_race                              # per jump
    lose = race_loss_probability(n_firms)
    rent_rate = lam_jump * prize                    # = lam_invest * h, equation (3)
    stale_trades = lam_jump * p_race * lose
    out = {
        "half_spread": h,
        "spread": 2 * h,
        "race_prob_per_jump": p_race,
        "race_loss_prob": lose,
        "snipe_profit_per_success": b,              # memoryless: E[J - h | J > h]
        "prize_per_jump": prize,
        "lp_loss_per_jump": prize * lose,
        "sniper_profit_per_jump_each": prize / n_firms,
        "rent_rate": rent_rate,
        "lp_profit_rate": lam_invest * h - rent_rate * lose,    # equation (1)
        "sniper_profit_rate_each": rent_rate / n_firms,         # equation (2)
        "stale_volume_share": stale_trades / (stale_trades + lam_invest),  # §VI.E.3
        "lp_breakeven_half_spread": laplace_half_spread(lam_invest, lam_jump * lose, b),
    }
    if c_speed is not None:
        n_star = free_entry_n(rent_rate, c_speed)
        n_int = free_entry_n(rent_rate, c_speed, integer=True)
        out.update(n_star=n_star, waste_rate=n_star * c_speed,
                   n_star_integer=n_int, waste_rate_integer=n_int * c_speed)
    return out


# --- Frequent batch auctions (BCS §VII) ---------------------------------------
# With symmetric fast traders and batch interval tau > 0 there is no sniping:
# the spread is 0 and rents are 0 (Prop 7). What survives is exposure from
# latency differences and jitter, below.

def fba_slow_lp_exposure(delta: float, tau: float) -> float:
    """Share of jumps that leave a SLOW LP snipe-able: delta / tau, delta = slow - fast (Prop 6(i))."""
    if tau <= 0 or delta < 0:
        raise ValueError("need tau > 0 and delta >= 0")
    return min(delta / tau, 1.0)


def fba_jitter_exposure(eps: float, tau: float, n_firms: int | None = None) -> float:
    """Share of jumps after which a FAST LP's stale quote meets a sniper in the batch.

    Footnote 34: each firm sees a jump after delta_fast plus U[0, eps]; the LP
    is vulnerable when the jump lands within eps of the batch cutoff and after
    its own draw, a share eps / (2 tau). That is n_firms=None.

    With n_firms given we also require at least one of the n_firms - 1 snipers
    to make the batch the LP missed (derived here, not in BCS): with the jump
    x/eps into the window, LP misses w.p. 1-x/eps and some sniper makes it
    w.p. 1-(1-x/eps)^(N-1), which integrates to (eps/tau)(N-1)/(2(N+1)) and
    tends to footnote 34's eps/(2 tau) as N grows.
    """
    if tau <= 0 or eps < 0 or eps > tau:
        raise ValueError("need 0 <= eps <= tau")
    base = eps / (2 * tau)
    if n_firms is None:
        return base
    return base * (n_firms - 1) / (n_firms + 1)


def fba_half_spread(lam_invest: float, lam_jump: float, jump, exposure: float) -> float:
    """LP indifference spread when only a share `exposure` of jumps can be sniped.

    Equation (3) with lam_jump scaled by the exposure. Not in BCS, who treat
    only exposure 0 (Prop 7: spread 0) and the delta/tau case qualitatively
    (footnote 38); this is the replication's prediction for an FBA arm with
    jitter. Exposure 1 recovers the continuous-book spread.
    """
    if not 0 <= exposure <= 1:
        raise ValueError(f"exposure must be in [0, 1], got {exposure}")
    return half_spread(lam_invest, lam_jump * exposure, jump)


def fba_max_slow_depth(c_speed: float, tau: float, delta: float, lam_jump: float,
                       mean_jump: float) -> float:
    """Largest depth Q slow LPs can offer at zero spread without inviting a fast sniper.

    Equation (8): (delta/tau) * lam_jump * E[J] * Q < c_speed. This is BCS's
    small-delta approximation; Prop 8 states the exact form with p_jump / tau
    and the total jump J' over a delta window in place of lam_jump and J.
    """
    if c_speed <= 0 or tau <= 0 or delta <= 0 or lam_jump <= 0 or mean_jump <= 0:
        raise ValueError("all arguments must be > 0")
    return c_speed * tau / (delta * lam_jump * mean_jump)


# The replication spec's operating grid (paper/bcs_replication_spec.md §3): rates
# per second, prices in engine ticks (PRICE_PRECISION 10000, so b = 0.10).
SPEC_LAM_INVEST = 10.0
SPEC_JUMP_RATIOS = (0.25, 1.0, 4.0)
SPEC_B = 1000.0
SPEC_N = (2, 3, 5, 10)

if __name__ == "__main__":
    cols = ("half_spread", "lp_breakeven_half_spread", "race_prob_per_jump", "race_loss_prob",
            "lp_loss_per_jump", "rent_rate", "lp_profit_rate", "stale_volume_share")
    print("| lam_jump/lam_invest | N | " + " | ".join(cols) + " |")
    print("|---" * (len(cols) + 2) + "|")
    for ratio in SPEC_JUMP_RATIOS:
        for n in SPEC_N:
            eq = laplace_equilibrium(SPEC_LAM_INVEST, ratio * SPEC_LAM_INVEST, SPEC_B, n)
            print(f"| {ratio} | {n} | " + " | ".join(f"{eq[c]:.4g}" for c in cols) + " |")
