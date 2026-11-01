#!/usr/bin/env python3
"""Seed corpus for fuzz_gateway_engine: native-gateway byte streams.

Each seed is what one client sends: length-prefixed V1 or V2 frames carrying
OrderRequest (include/MatchingEngine.h, packed, 122 bytes). They give the
fuzzer every order type and request type in a valid frame to mutate from, so
it does not have to discover the framing first.

Usage: gen_gateway_seeds.py <out-dir>
"""
import os
import struct
import sys

# type symbolId orderId participantId side price qty orderType stopPrice
# displayQty tif expiryTime stopLimitPrice pegType pegOffset trailAmount minQty
# hidden newPrice newQty ingressTsc
REQ = struct.Struct("<BIQQBqQBqQBQqBqqQBqQQ")
assert REQ.size == 122
HDR = struct.Struct("<IHHIIQ")  # magic version headerSize payloadSize flags clientRequestId
MAGIC = 0x4F424757

NEW, CANCEL, MODIFY, REPLACE, KILL = 1, 2, 3, 4, 5
LIMIT, MARKET, IOC, FOK, STOP, STOPLIMIT, ICEBERG, POSTONLY, PEGGED, TRAIL, HIDDEN, MIT, MOC, LOC = range(14)
GTC, GTD, DAY = range(3)
BUY, SELL = 0, 1


def req(kind: int = NEW, sym: int = 0, oid: int = 1, pid: int = 1, side: int = BUY,
        px: int = 10000, qty: int = 10, ot: int = LIMIT, stop: int = 0, disp: int = 0,
        tif: int = GTC, exp: int = 0, stoplim: int = 0, peg: int = 0, pegoff: int = 0,
        trail: int = 0, minq: int = 0, hidden: int = 0, newpx: int = 0,
        newqty: int = 0) -> bytes:
    return REQ.pack(kind, sym, oid, pid, side, px, qty, ot, stop, disp, tif, exp, stoplim,
                    peg, pegoff, trail, minq, hidden, newpx, newqty, 0)


def v1(r: bytes) -> bytes:
    return struct.pack(">I", len(r)) + r


def v2(r: bytes, cid: int = 1) -> bytes:
    body = HDR.pack(MAGIC, 2, HDR.size, len(r), 0, cid) + r
    return struct.pack(">I", len(body)) + body


def book(pid: int = 1) -> list[bytes]:
    """A two-sided book: bids 9990/9980, asks 10010/10020."""
    return [req(oid=101, pid=pid, side=BUY, px=9990), req(oid=102, pid=pid, side=BUY, px=9980),
            req(oid=103, pid=pid, side=SELL, px=10010), req(oid=104, pid=pid, side=SELL, px=10020)]


SEEDS = {
    "cross": [req(oid=1, side=BUY, px=10000), req(oid=2, pid=2, side=SELL, px=10000, qty=4)],
    "market_minqty": book() + [req(oid=3, pid=2, side=BUY, px=0, qty=15, ot=MARKET, minq=5)],
    "stop_market": book() + [req(oid=3, pid=2, side=SELL, px=0, qty=30, ot=STOP, stop=9990),
                             req(oid=4, pid=3, side=SELL, px=9990, qty=1)],
    "stop_family": book() + [req(oid=3, pid=2, side=BUY, ot=STOPLIMIT, stop=10010, stoplim=10015),
                             req(oid=4, pid=2, side=SELL, px=10000, ot=TRAIL, trail=50),
                             req(oid=5, pid=2, side=SELL, ot=MIT, stop=10030),
                             req(oid=6, pid=3, side=BUY, px=10010, qty=1)],
    "resting_types": [req(oid=1, ot=ICEBERG, qty=100, disp=10), req(oid=2, side=SELL, px=10050, ot=HIDDEN),
                      req(oid=3, side=SELL, px=10040, ot=POSTONLY, hidden=1),
                      req(oid=4, pid=2, side=SELL, px=9990, qty=60)],
    "lifecycle": book() + [req(kind=MODIFY, oid=101, newqty=4),
                           req(kind=REPLACE, oid=102, newpx=9995, newqty=12),
                           req(kind=CANCEL, oid=103)],
    "expiry": [req(oid=1, tif=GTD, exp=1_000), req(oid=2, side=SELL, px=10010, tif=DAY, exp=2_000),
               req(oid=3, tif=GTD, exp=0)],
    "ioc_fok": book() + [req(oid=3, pid=2, side=BUY, px=10020, qty=15, ot=IOC),
                         req(oid=4, pid=2, side=SELL, px=9980, qty=50, ot=FOK)],
    "auction_types": [req(oid=1, ot=PEGGED, peg=1), req(oid=2, ot=MOC, px=0), req(oid=3, ot=LOC),
                      req(oid=4, sym=1, side=SELL, px=10005)],
    "kill_switch": book() + [req(kind=KILL, pid=1), req(oid=5, side=SELL, px=9990)],
}


def main() -> None:
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, reqs in SEEDS.items():
        with open(os.path.join(out, f"v2_{name}"), "wb") as f:
            f.write(b"".join(v2(r, i + 1) for i, r in enumerate(reqs)))
        with open(os.path.join(out, f"v1_{name}"), "wb") as f:
            f.write(b"".join(v1(r) for r in reqs))
    print(f"wrote {2 * len(SEEDS)} seeds to {out}")


if __name__ == "__main__":
    main()
