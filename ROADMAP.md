# Roadmap

Release index updated 2026-09-15. **Work order and acceptance are maintained only
in [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md).** Version labels describe
capabilities, not a strict sequence of projects or a profitability percentage.
The package version remains 0.2.0; this documentation update does not release code.

Capability status refers to the local `7cf248f` baseline described in the plan,
not an assertion about remote PR integration.

| Milestone | Capability status | Remaining work owner |
|---|---|---|
| v0.1 — Deterministic market data | Foundation implemented: exact types, books, validity, recovery, decode, journal and checks. | Preserve invariants as later phases change behavior. |
| v0.2 — Versioned constraints and sessions | Reviewed metadata, payoff verification, session binding, candidate lifecycle and structured offline replay implemented. | Actual live controller history and sequence-scope validation: P2. |
| v0.3 — Economic opportunity evaluation | Depth, fees, funding and delayed IOC reference implemented. | Profit-aware sizing: P1. Representative supply: P2. Complete execution/settlement comparison: P3. |
| v0.4 — Operational integration | Pending. Read-only capture can arrive earlier through P2. | Shared live/shadow path, lifecycle, limits and reconciliation: P4. |
| Later — Authorized execution | Pending; no submission authority follows from earlier milestones. | Actual economic evidence and scale decision: P5. |

Generated payoff, journal and differential candidate checks already exist.
Extend them for a concrete behavior or coverage gap; there is no separate blanket
requirement to rebuild the testing foundation before P1.

Research proposals and specialized optimizations are candidates. Their entry
conditions are in the implementation plan; this index does not schedule them.
