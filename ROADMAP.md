# Roadmap

The development order is correctness, usability, observability, measurement, and
only then optimization. Milestones describe engineering capability, not expected
profitability.

See [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) for ordered deliverables and
acceptance criteria. An optional offline benchmark baseline is now available
alongside v0.2 so later features have measurable costs from their first change;
methodology and research live in [PERFORMANCE.md](PERFORMANCE.md).

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
- operator journal verification;
- strict reviewed metadata snapshot loading and canonical CLI output;
- generated truth-table checks of loaded relationships and fractional payouts;
- finalized sessions binding canonical metadata, exact journal bytes and count;
- offline session pack/verify CLI with incomplete-session detection;
- deterministic gross candidate identity and lifecycle, updated by market dependency;
- structured CLI replay with manifest-bound explicit controller plans;
- strict offline capture import and reproducible candidate/event output.

Remaining before declaring the milestone complete:

- actual live subscription/controller-history recording and venue sequence-scope validation;
- property-based tests over generated worlds and malformed journal frames.

## v0.3 — Executable opportunity evaluation

First offline reference implemented: depth, exact fee rounding, conservative
funding reservations, scheduled IOC fills, shared liquidity, partial-leg exposure
and structured economic reports. See [OFFLINE_STUDY.md](OFFLINE_STUDY.md).
Observed profitability remains unestablished. Remaining: sizing/allocator policy
comparisons, calibrated execution/settlement and representative held-out data.

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
