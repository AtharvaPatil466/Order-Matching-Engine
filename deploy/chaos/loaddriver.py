"""
ChaosLoadDriver — synthetic order injection via the chaos-gated
/chaos/order admin endpoint.

The engine's real order flow is FIX over TCP via the separate
GatewayServer binary; wiring a Python FIX client into the chaos
topology is the proper-but-expensive path. This driver takes the
cheap-but-meaningful path: hit the admin endpoint that's only
enabled when OB_CHAOS_INJECT=1, parse the JSON response, and book-
keep ACKed clOrdIDs locally.

Why this is meaningful despite skipping FIX:
    * The endpoint calls engine.submitOrder directly, so the
      acceptance/rejection semantics are identical to the FIX path.
    * The same journal commit hook fires on accepted orders, so
      replication ships them to backup the same way.
    * Bilateral correctness invariants (NoCommittedLoss,
      NoDuplicateExecution) are observable end-to-end.

What this CAN'T verify that real FIX would:
    * Wire-format correctness (FIX-specific tags, checksums).
    * Session-layer behavior (Logon, Heartbeat, ResendRequest).
    * Backpressure path through TCP framing.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterator, List

import requests

from cluster import ADMIN_HEADERS


@dataclass
class OrderSubmission:
    order_id: int
    side: int       # 0=Buy, 1=Sell
    price: int
    qty: int
    participant_id: int
    symbol_id: int = 0
    accepted: bool = False
    sequence_id: int = 0
    reject_reason: int = 0


# Must match the OB_CHAOS_TOKEN env var on the engine container.
# Defined in docker-compose.chaos.yml; treated as a shared secret
# for the lifetime of the cluster instance.
DEFAULT_TOKEN = "chaos-suite-shared-secret"


@dataclass
class ChaosLoadDriver:
    base_url: str   # e.g. "http://localhost:18080"
    token: str = DEFAULT_TOKEN
    submissions: List[OrderSubmission] = field(default_factory=list)

    def submit(
        self,
        order_id: int,
        side: int,
        price: int,
        qty: int,
        participant_id: int = 1,
        symbol_id: int = 0,
        timeout_s: float = 1.0,
    ) -> OrderSubmission:
        params = {
            "orderId": order_id,
            "side": side,
            "price": price,
            "qty": qty,
            "participantId": participant_id,
            "symbolId": symbol_id,
        }
        headers = dict(ADMIN_HEADERS)
        if self.token:
            headers["X-Chaos-Token"] = self.token
        sub = OrderSubmission(
            order_id=order_id, side=side, price=price, qty=qty,
            participant_id=participant_id, symbol_id=symbol_id,
        )
        try:
            r = requests.get(
                f"{self.base_url}/chaos/order",
                params=params,
                headers=headers,
                timeout=timeout_s,
            )
            if r.status_code == 200:
                body = r.json()
                if not body.get("enabled"):
                    raise RuntimeError(
                        "/chaos/order returned enabled=false — "
                        "set OB_CHAOS_INJECT=1 in the engine container"
                    )
                sub.accepted = bool(body.get("accepted"))
                sub.sequence_id = int(body.get("sequenceId", 0))
                sub.reject_reason = int(body.get("rejectReason", 0))
        except requests.RequestException:
            # Connection died mid-flight (e.g. primary was just
            # SIGKILLed). Leave accepted=False; the test's invariant
            # is over the SET of orders the engine ACKED, which by
            # definition excludes orders whose response never arrived.
            pass
        self.submissions.append(sub)
        return sub

    def burst(
        self,
        count: int,
        *,
        side: int = 0,
        base_price: int = 50000,
        qty: int = 1,
        starting_order_id: int = 1,
        participant_id: int = 1,
    ) -> Iterator[OrderSubmission]:
        """
        Submit `count` orders sequentially. Yields each submission
        after it returns so the caller can interleave faults (e.g.
        kill primary midway through the burst).

        Orders are priced WELL below the warmup band so they never
        match — they all rest in the book. This makes the surviving-
        node assertion straightforward: every accepted orderId must
        appear in the surviving node's book.
        """
        for i in range(count):
            yield self.submit(
                order_id=starting_order_id + i,
                side=side,
                price=base_price + i,  # unique prices, far below mid
                qty=qty,
                participant_id=participant_id,
            )

    def acked_order_ids(self) -> set[int]:
        return {s.order_id for s in self.submissions if s.accepted}

    def rejected_count(self) -> int:
        return sum(1 for s in self.submissions if not s.accepted)
