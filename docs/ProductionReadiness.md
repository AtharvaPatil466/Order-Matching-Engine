# Production Readiness Notes

This project is a low-latency matching-engine implementation for research and portfolio use. It intentionally models many exchange-system concerns, but production deployment would require more work than passing the local test suite.

## Semantics To Specify Before Production

- Public submit semantics now distinguish ingress acceptance from final order-book outcome in async mode:
  `SubmitResult::Accepted` means the request passed ingress validation and was queued, while order-level
  rejects that occur on the worker are delivered through order-update/event channels.
- Self-match prevention policy variants: cancel taker, cancel maker, decrement-and-cancel, and participant hierarchy rules.
- Cancel/replace priority rules for every amendment type, including hidden and iceberg orders.
- Market order protection, price collars, auction states, halt/resume transitions, and reject-code contracts.
- Iceberg refresh priority and hidden-order allocation rules for the exact venue model.
- End-to-end sequence guarantees for trades, order updates, market data, replay, and gap recovery.
- Risk-limit policy for open orders, filled positions, rejected orders, cancels, and cross-symbol exposure.

## Validation Required

- Sanitizer runs: AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer.
- Long-running randomized soak tests with fixed seeds saved on failure.
- Concurrency tests for shutdown, backpressure, rate limiting, kill switch, snapshot reads, and checkpoint/replay.
- Crash tests covering partial journal writes, checkpoint replacement, corrupted records, and replay idempotency.
- Benchmark runs with repeated samples, hardware details, compiler version, power settings, and variance.

## Validation Already Covered In This Repo

- CRC-validated journal replay stops at truncated or corrupted records.
- Checkpoint replay restores active orders after journal rewrite.
- Randomized command streams are replayed and compared against the original final book state.
- Ingress submit APIs return explicit reject reasons for stopped engines, unknown symbols, bad orders, rate limits, and queue backpressure.
- Gateway requests surface ingress rejects as error responses instead of unconditional ACKs.
- CI runs project tests under Release, ASan/UBSan, and ThreadSanitizer configurations; Ubuntu sanitizer builds also run an E2E benchmark smoke check.

## Benchmark Artifact Workflow

Run `./scripts/run_benchmarks.sh` on an idle machine. The script writes a timestamped artifact directory containing:

- `machine.txt` with OS, compiler, and CPU metadata.
- `manual_benchmark.txt` for processing-latency measurements.
- `e2e_benchmark.txt` for ingress-to-completion measurements.

README latency tables should be updated only from saved artifacts, preferably using the median of repeated runs.

## Operational Work Required

- Structured logging with stable event schemas.
- Metrics contracts for order flow, rejects, queue depth, latency histograms, journal health, and gateway sessions.
- Configuration files for symbols, risk limits, gateway ports, admin endpoints, rate limits, and persistence.
- Deployment and recovery runbooks.
- Backward-compatible protocol versioning for gateway and market-data consumers.
