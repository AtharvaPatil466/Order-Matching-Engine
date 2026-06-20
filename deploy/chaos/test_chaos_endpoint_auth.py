"""
Scenario: /chaos/order rejects unauthenticated requests when a
token is configured.

The /chaos/order endpoint is gated by:
    1. OB_CHAOS_INJECT=1 must be set at process start
    2. If OB_CHAOS_TOKEN is also set, requests must carry a matching
       X-Chaos-Token header

This test pins the second gate in place. Without it, anyone with
network reach to the admin port could inject orders the moment
OB_CHAOS_INJECT got enabled (intentionally or not).
"""
from __future__ import annotations

import pytest
import requests

from cluster import PRIMARY, _ADMIN_PORTS


def _chaos_url() -> str:
    return f"http://localhost:{_ADMIN_PORTS[PRIMARY]}/chaos/order"


def test_chaos_order_rejects_missing_token() -> None:
    """No header at all → unauthorized."""
    r = requests.get(
        _chaos_url(),
        params={"orderId": 800001, "participantId": 1, "price": 100000, "qty": 1},
        timeout=2.0,
    )
    assert r.status_code == 200, r.status_code
    body = r.json()
    assert body.get("enabled") is True, body
    assert body.get("accepted") is False, body
    assert body.get("error") == "unauthorized", body


def test_chaos_order_rejects_wrong_token() -> None:
    """Wrong header value → unauthorized (constant-time compare not required for chaos suite, just correctness)."""
    r = requests.get(
        _chaos_url(),
        params={"orderId": 800002, "participantId": 1, "price": 100000, "qty": 1},
        headers={"X-Chaos-Token": "not-the-secret"},
        timeout=2.0,
    )
    body = r.json()
    assert body.get("accepted") is False
    assert body.get("error") == "unauthorized"


def test_chaos_order_accepts_correct_token() -> None:
    """Valid header → injection proceeds."""
    r = requests.get(
        _chaos_url(),
        params={"orderId": 800003, "participantId": 1, "price": 100000, "qty": 1},
        headers={"X-Chaos-Token": "chaos-suite-shared-secret"},
        timeout=2.0,
    )
    body = r.json()
    assert body.get("enabled") is True
    # Either accepted (ingress queued ok) or accepted=false with a
    # legit reject reason (e.g. duplicate orderId from a prior run).
    # The auth gate passed — that's what this test verifies.
    assert "error" not in body or body.get("error") != "unauthorized"
