# Roadmap

The development order is correctness, usability, observability, measurement, and
only then optimization. Milestones describe engineering capability, not expected
profitability.

## v0.1 — Deterministic market-data foundation

Completed: fixed-point price and quantity, normalized order books, strict sequence
and stream handling, generation-aware multi-market state, explicit recovery,
Kalshi decoding/normalization, raw replay, tests, and CI.

## v0.2 — Versioned constraint core

Current milestone. Implemented:

- stable market IDs under an explicit metadata version;
- checksummed journal schema 2 and envelope/payload sequence validation;
- separate cash and contract-quantity types;
- versioned constraint definitions with provenance;
- compilation to canonical valid worlds and payoff leg templates;
- finite-world minimum-payoff verification;
- operator journal verification.

Remaining before declaring the milestone complete:

- ingest a reviewed contract-metadata snapshot into the explicit registry;
- deterministic opportunity identity and lifecycle events;
- persist the constraint metadata snapshot alongside a capture session;
- property-based tests over generated worlds and malformed journal frames.

## v0.3 — Executable opportunity evaluation

- evaluate available depth, fees, slippage, and partial fills;
- maintain incremental dependency-driven opportunity updates;
- emit explainable opportunities without submitting orders;
- measure replay latency and state-transition throughput before optimization.

## v0.4 — Demo connectivity and controls

- authenticated Kalshi demo transport;
- reconnect, backoff, subscription, and snapshot orchestration;
- centralized position/notional limits and kill switch;
- read-only operator/debugging interface and audit events.

## Later — Explicitly gated execution

Order submission is not implied by the earlier milestones. Demo execution must be
separately reviewed and disabled by default. Production access requires another
explicit gate, operational runbooks, reconciliation, and evidence that all risk
controls fail closed.
