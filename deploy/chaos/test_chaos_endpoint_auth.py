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

from cluster import ADMIN_HEADERS, PRIMARY, _ADMIN_PORTS


def _chaos_url() -> str:
    return f"http://localhost:{_ADMIN_PORTS[PRIMARY]}/chaos/order"


def _admin_url(path: str) -> str:
    return f"http://localhost:{_ADMIN_PORTS[PRIMARY]}{path}"


def test_admin_endpoints_require_bearer_token() -> None:
    """Non-probe admin endpoints 401 without the bearer token; probes stay open."""
    assert requests.get(_admin_url("/role"), timeout=2.0).status_code == 401
    assert (
        requests.get(
            _admin_url("/role"),
            headers={"Authorization": "Bearer not-the-secret"},
            timeout=2.0,
        ).status_code
        == 401
    )
    assert (
        requests.get(_admin_url("/role"), headers=ADMIN_HEADERS, timeout=2.0).status_code
        == 200
    )
    # k8s probes must work without credentials.
    assert requests.get(_admin_url("/health"), timeout=2.0).status_code == 200
    r = requests.get(_admin_url("/readyz"), timeout=2.0)
    assert r.status_code in (200, 503), r.status_code


def test_chaos_order_rejects_missing_token() -> None:
    """Bearer auth passes but no X-Chaos-Token → chaos-gate unauthorized."""
    r = requests.get(
        _chaos_url(),
        params={"orderId": 800001, "participantId": 1, "price": 100000, "qty": 1},
        headers=ADMIN_HEADERS,
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
        headers={**ADMIN_HEADERS, "X-Chaos-Token": "not-the-secret"},
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
        headers={**ADMIN_HEADERS, "X-Chaos-Token": "chaos-suite-shared-secret"},
        timeout=2.0,
    )
    body = r.json()
    assert body.get("enabled") is True
    # Either accepted (ingress queued ok) or accepted=false with a
    # legit reject reason (e.g. duplicate orderId from a prior run).
    # The auth gate passed — that's what this test verifies.
    assert "error" not in body or body.get("error") != "unauthorized"
