"""Experiment 12's search machinery on synthetic games, and its market on the engine.

The best response and the Nash iteration are pure functions of a payoff
sampler, so they are pinned on games whose answer is known: a payoff table
with one optimum, a tie that must be reported as indifference rather than
picked, a game with dominant strategies (converges), and matching pennies
(no pure equilibrium: must be reported as a cycle). The market itself is
checked for determinism, closed accounting and the F / no-quote rules.
"""
import sys
from pathlib import Path

_R = Path(__file__).resolve().parents[1]
for _d in ("build", "simulation", "agents", "metrics", "analysis", "calibration", "experiments"):
    if str(_R / _d) not in sys.path:
        sys.path.insert(0, str(_R / _d))

import numpy as np  # noqa: E402

import exp12_endogenous_makers as E  # noqa: E402
from strategic_maker import MakerPolicy  # noqa: E402

SCREEN, CONFIRM = tuple(range(8)), tuple(range(100, 116))


def table_sampler(payoff, noise=0.1):
    """Per-seed payoffs: the table value, a shock common to every candidate on
    a seed (common random numbers), and a small candidate-specific shock."""
    names = sorted(payoff)

    def sample(cands, seeds):
        common = np.array([np.random.default_rng([s, 0]).normal(0, 5.0) for s in seeds])
        return [payoff[c] + common + np.array(
            [np.random.default_rng([s, 1 + names.index(c)]).normal(0, noise) for s in seeds])
            for c in cands]
    return sample


def br_on(payoff, incumbent, shortlist=3):
    cands = tuple(sorted(payoff))
    return E.best_response(table_sampler(payoff), cands, incumbent, SCREEN, CONFIRM, shortlist)


# --- best response ---------------------------------------------------------

def test_best_response_finds_the_known_optimum_on_a_synthetic_table():
    # Arrange: c is best by 0.5, far above the 0.1 candidate noise; the common
    # shock (sd 5) would swamp it without paired comparison.
    payoff = {"a": 0.0, "b": 1.0, "c": 3.0, "d": 2.5, "e": -1.0, "out": 0.0}

    # Act
    r = br_on(payoff, incumbent="a")

    # Assert
    assert r["best"] == "c" and r["policy"] == "c" and r["deviates"]
    assert r["incumbent_gap"]["upper"] < 0
    assert r["indifferent"] == []


def test_a_tie_is_reported_as_indifferent_and_the_incumbent_stays():
    # Arrange: b (the incumbent) and c are equal; a single noisy mean would pick one.
    payoff = {"a": 0.0, "b": 2.0, "c": 2.0, "out": 0.0}

    # Act
    r = br_on(payoff, incumbent="b")

    # Assert
    assert not r["deviates"] and r["policy"] == "b"
    assert {r["best"], *r["indifferent"]} == {"b", "c"}


def test_the_incumbent_is_confirmed_even_when_the_screen_cuts_it():
    # Arrange: shortlist 1 keeps only the screen's top; the incumbent must
    # still be measured on the confirm seeds to decide whether to switch.
    payoff = {"a": 0.0, "b": 1.0, "c": 3.0}

    # Act
    r = br_on(payoff, incumbent="a", shortlist=1)

    # Assert
    assert [row["policy"] for row in r["table"]] == ["c", "a"]
    assert r["deviates"] and r["policy"] == "c"


def test_best_response_is_deterministic():
    payoff = {"a": 0.0, "b": 1.0, "c": 1.05, "out": 0.0}
    assert br_on(payoff, "a") == br_on(payoff, "a")


# --- Nash iteration ----------------------------------------------------------

def game_br(payoffs):
    """br(i, profile) for a finite game: payoffs[profile][i]."""
    actions = sorted({a for prof in payoffs for a in prof})

    def br(i, profile):
        table = {a: payoffs[profile[:i] + (a,) + profile[i + 1:]][i] for a in actions}
        return br_on(table, profile[i])
    return br


def test_nash_iteration_converges_on_a_game_with_dominant_strategies():
    # Arrange: a prisoner's dilemma; D dominates, so (D, D) from anywhere.
    pd = {("C", "C"): (3, 3), ("C", "D"): (0, 5), ("D", "C"): (5, 0), ("D", "D"): (1, 1)}

    # Act
    r = E.nash_iteration(game_br(pd), ("C", "C"))

    # Assert: two switches in round 0, a quiet round 1.
    assert r["status"] == "equilibrium" and r["profile"] == ("D", "D") and r["rounds"] == 2
    assert [e["deviates"] for e in r["log"]] == [True, True, False, False]


def test_nash_iteration_reports_a_cycle_on_matching_pennies():
    # Arrange: player 0 wants to match, player 1 to mismatch: no pure equilibrium.
    pennies = {(a, b): ((1, -1) if a == b else (-1, 1)) for a in "HT" for b in "HT"}

    # Act
    r = E.nash_iteration(game_br(pennies), ("H", "H"))

    # Assert
    assert r["status"] == "cycle"
    assert r["cycle"] == [("H", "H"), ("H", "T"), ("T", "T"), ("T", "H")]


def test_nash_iteration_stops_at_max_rounds():
    pennies = {(a, b): ((1, -1) if a == b else (-1, 1)) for a in "HT" for b in "HT"}
    r = E.nash_iteration(game_br(pennies), ("H", "H"), max_rounds=1)
    assert r["status"] == "max_rounds"


# --- the market ----------------------------------------------------------------

def test_search_space_offers_no_quote_and_post_only_only_with_rivals():
    alone, rivals = E.search_space(0), E.search_space(2)
    assert alone[-1] == rivals[-1] == E.NO_QUOTE
    assert not any(p.post_only for p in alone)
    assert len(rivals) - 1 == 2 * (len(alone) - 1)
    assert len(set(rivals)) == len(rivals)


POLICY = MakerPolicy(half_spread=4, qty=1_000, post_only=True)
LAT = (50_000, 50_000)
HORIZON = 20.0


def test_simulate_is_deterministic_closes_the_accounting_and_post_only_never_self_crosses():
    # Act
    a = E.simulate(E.facts.CFG, (POLICY, POLICY), LAT, 7, HORIZON)
    b = E.simulate(E.facts.CFG, (POLICY, POLICY), LAT, 7, HORIZON)
    other = E.simulate(E.facts.CFG, (POLICY, POLICY), LAT, 8, HORIZON)

    # Assert
    assert a == b and a != other
    assert sum(a["maker_fills"]) > 0
    assert abs(a["identity_residual"]) < 1e-9
    assert a["maker_on_maker_fills_per_s"] == 0
    assert a["half_spread_ticks"] >= 4 - 0.5


def test_sampler_charges_F_only_when_quoting_and_never_runs_no_quote():
    # Arrange
    ev = E.Evaluator(E.facts.CFG, HORIZON)
    sample = E.maker_sampler(ev, 0, (POLICY, POLICY), LAT, cost=0.25)

    # Act
    quoting, out = sample([POLICY, E.NO_QUOTE], (7,))
    sample([POLICY], (7,))                       # cached: no second run

    # Assert
    row = ev.cache[((POLICY, POLICY), LAT, 7)]
    assert len(ev.cache) == 1
    assert out.tolist() == [0.0]
    assert quoting.tolist() == [row["maker_net"][0] - 0.25]
