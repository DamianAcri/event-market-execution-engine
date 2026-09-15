# Implementation plan

Updated: 2026-09-15. This is the **single execution plan** for the project: priorities,
dependencies, current status and acceptance criteria are maintained here.
[PROJECT_DIRECTION.md](PROJECT_DIRECTION.md) defines the product objective;
[ARCHITECTURE.md](ARCHITECTURE.md) defines component boundaries. Research documents
justify or propose methods; a proposal becomes scheduled work only when this plan
selects it. [ROADMAP.md](ROADMAP.md) is a release index, not a second work queue.

## Objective and decisions

Build a maintainable Kalshi structural-arbitrage and execution engine that can
capture repeatable net profit within capital, loss and operating-cost limits.
The first strategy acquires reviewed implication/complement portfolios. Profit
comes from the complete portfolio's payment exceeding all acquisition costs;
actual results also depend on completing its legs and recovering the capital.
Economic viability is undetermined. Engineering completion is an intermediate
requirement, not the commercial success criterion.

The chosen approach is to improve the existing engine in small vertical steps.
A broad multi-strategy platform would add uncalibrated models and maintenance;
continuing general infrastructure optimization alone would postpone the economic
question. We therefore retain one C++20 decision path and start with better sizing
and representative observations. The trade-off is narrower initial coverage in
exchange for a result that can be implemented, compared and maintained.

Fixed decisions for the initial strategy:

- Reviewed structural relationships; probabilistic forecasting does not establish
  their payoff guarantee. Metadata compilation remains outside per-update work.
- Exact price, quantity and cash types; the existing fee ledger and explicit
  financing assumptions remain the accounting reference.
- Aggressive acquisition first. Shared liquidity, partial portfolios and funding
  limits apply; no assumed multi-market atomicity or unverified collateral credit.
- The same decision functions serve offline studies and eventual live adapters.
  A slow independent oracle is a test tool, not a second production strategy.
- Low latency is part of each measured decision. New hardware, concurrency or
  model complexity requires evidence that it improves the relevant result.

## Verified starting point

Code reviewed at `7cf248f8fe96fd01091998a1fe642073d9c6bb9a`, the offline-study work
associated with [PR #7](https://github.com/DamianAcri/event-market-execution-engine/pull/7).
This is a local capability inventory, not a statement about current remote merge
status. Integrating completed work does not count as implementing another phase.

| State | Capability |
|---|---|
| Implemented | Fixed-point domain, books, validity/recovery, decoder, journal and payoff verification. |
| Implemented | Reviewed metadata, finalized sessions, incremental gross candidates and general structured replay. |
| Implemented reference | Costed depth, per-order fees, conservative reserves, separate IOC arrivals, shared depletion and partial-position bounds. |
| Implemented engineering checks | Independent arithmetic/generated/differential cases, CI configuration and portable benchmark tooling. Broader campaigns remain targeted work, not a reason to restart the foundation. |
| Implemented and locally measured | Exact bounded net-profit sizing, independent exhaustive oracle and versioned replay policy; see [NET_SIZING.md](NET_SIZING.md). |
| Pending | Read-only live capture, representative observations, response/hedge/settlement accounting and calibrated execution. |
| Pending | Operational order lifecycle, reconciliation and separately authorized execution. |

The first REST pilot (one event, eight markets, about 75 seconds) was inconclusive.
The later synthetic sizing example establishes a missed smaller-size opportunity,
not market profitability. Details and reproductions are in
[OFFLINE_STUDY.md](OFFLINE_STUDY.md) and
[ECONOMIC_STRATEGY_RESEARCH.md](ECONOMIC_STRATEGY_RESEARCH.md).

## Delivery order

**P1 is merged; P2 transport/controller implementation is tested locally.** Start P2 collection as soon as its
recorder is ready; it must not wait for all simulator or product features. P3 can
be developed against fixtures while data is collected, but passing its economic
acceptance requires suitable observations from P2. P4 follows P3. P5 requires
separate authorization and operational readiness.

| Phase | Outcome | Dependency | Credentials |
|---|---|---|---|
| P1 | Choose the best funded quantity for the existing two-leg strategy. | Current offline study. | None. |
| P2 | Identify which reviewed Kalshi families supply useful costed opportunities. | Existing recording/replay contracts; P1 for sizing comparison. | Public REST exploration needs none; authenticated WS capture needs market-data credentials. |
| P3 | Compare acquisition and recovery policies through cash settlement. | P1 decisions and ledger; P2 data for economic conclusions. | Offline development needs none; venue calibration has its own access requirements. |
| P4 | Run the same decisions with observed data and a reconciled operational lifecycle. | P3 policy and limits. | Data/demo access as needed; submission remains explicitly controlled. |
| P5 | Measure actual net results before considering scale. | P4 readiness, a frozen policy and explicit execution authorization. | Authorized account access. |

### P1 — Profit-aware sizing

**Status, 2026-09-15:** merged as [PR #8](https://github.com/DamianAcri/event-market-execution-engine/pull/8),
`e7b7644`, with portability and sanitizer CI passing. The documented
smaller-size opportunity is recovered through replay, generated cases match an
independent exhaustive fee ledger, and the old policy retains identical output.
Core sizing and whole-study comparisons are measured in [PERFORMANCE.md](PERFORMANCE.md).
This closes P1's two-leg quantity-selection scope. Portfolio allocation, uncertain
fills and operational execution remain in their separately defined phases below.

**Deliverable:** a versioned policy for existing two-leg implications/complements
that chooses the quantity with maximum fully acquired net settlement margin among
funded choices, including not trading. The current policy remains a reproducible
baseline. This is not yet optimization of expected profit under uncertain fills.

Implement in this order:

1. Move the smallest reusable sizing/cost decision into `eme_core`, using existing
   book access and fee types. Keep JSON, session I/O and simulated arrivals in
   `eme_session`. No generic strategy framework or new service is required.
2. Establish an independent exhaustive reference on the allowed quantity grid,
   including limit-price funding reserves, depth, exact fees and rounding.
3. Implement bounded exact selection, reusing cumulative depth/cost information
   where valid. Do not assume profit is monotone. Budget exhaustion must be
   explicit; a limited search is neither an optimum nor evidence of no opportunity.
4. Return quantity, leg limits, debit, payout floor, margin, peak reservation and
   decline/search status. Resolve ties deterministically: lower reservation first,
   then smaller quantity. Bind policy version and parameters into study output.

**Acceptance:** recover the documented two-contract example when the cap is five;
match the independent reference on generated books, grids, fees and funding
limits; never change cash from a quote alone; preserve freshness and shared-depth
semantics. Measure time and memory across quantity counts, levels and dependency
fan-out. Pure speed changes must preserve outputs; changing the policy must report
both the decision improvement and its computational cost. No market return claim
follows from synthetic cases.

**First implementation branch:** `feat/net-profit-sizing`. Finish this block and
its measured comparison before starting another major strategy feature.

### P2 — Opportunity supply and read-only capture

**Status, 2026-09-15:** optional authenticated read-only TLS/WS transport,
strict one-market subscription controller, reconnect/recovery, bounded background
recording and manifest-bound controller replay are implemented on
`feat/readonly-market-capture`. Local TLS fixtures verify signing, peer validation,
control/data history and recovery; the decoder avoids redundant per-frame parsing.
**P2 remains data-dependent, not complete:** real authenticated sequence/subscription
acceptance and representative multi-event collection have not been validated.
[READONLY_CAPTURE.md](READONLY_CAPTURE.md) owns the actual interface and limits.

**Deliverable:** reproducible observed sessions and a report showing where net
margin exists, at what quantities, and which constraints prevent acquisition.
This phase owns actual subscription/controller history and venue sequence-scope
validation that older documents placed ambiguously between v0.2 and v0.4.

Start with the reviewed threshold family already used by the pilot. Add a second
family only after its resolution rules and exceptions support the relationship;
prefer a structurally different family rather than many copies of one event.
An initial collection batch should span at least three event dates and distinguish
quiet, active and near-resolution windows. These are collection targets, not a
statistical sufficiency threshold. Reserve complete later events/dates before
policy tuning, and expand coverage if it remains too concentrated or sparse.

Build read-only WS orchestration around the existing decoder/journal/state path:
explicit price convention, subscription identity, raw frames, clocks, connection
and recovery actions, session finalization and bounded resource handling. Resolve
actual sequence scope at the gateway; do not weaken core validity checks to make
an incompatible feed appear valid. No order writer is needed for capture.

Summarize economic episodes causally, with partial/censored observation windows
identified. Report P1 versus baseline at equal capital and fees, depth and funding
constraints, operating-cost assumptions, exclusions and concentration by event.
Do not choose an episode's best timestamp retrospectively as an executed trade.
Do not infer replenishment or independent opportunities from repeated snapshots.

**Acceptance:** raw capture and controller history replay to identical state and
decisions; rules and economic assumptions are versioned; report coverage and
limits alongside costed supply. REST can support family exploration while keys
are deferred, but it does not complete timing/sequence acceptance for WS.

**Decision:** continue a family with useful costed supply into P3. Inconclusive
data requires a specified additional collection batch. Reject or revise a family
when adequate evidence shows its margin cannot cover costs in the declared scope.
A negative one-attempt result, limited search or sample is not such a proof. An
immediate-fill scenario is an optimistic benchmark; call it an upper bound only
if its bound over the defined search, liquidity and capital scope is justified.

### P3 — Complete the economic execution loop

**Deliverable:** compare the aggressive baseline with one sequential completion
policy, including what happens when only part of the portfolio is acquired.
Reuse P1 sizing and the existing ledger; extend the simulator that already exists.

Add explicit order intent, arrival, response, fills, rejects, cancellation races
and reconciliation events. Add a bounded residual-position policy: complete or
reduce only with available depth and cash, stop at declared exposure/time limits,
and represent inability to exit. Track settlement cash and capital holding time
without using the future outcome or settlement time in earlier decisions.
Keep simulated settlement results distinct from observed realized cash.

Test correlated book changes, late responses, partial/rejected legs, stale data,
EOF, duplicate messages and restarts. Fix policy/limits before evaluating reserved
sessions. Account for same-price fill fragmentation and unresolved impact/queue
assumptions explicitly. Model verified collateral return only when a chosen
funding scenario needs it; otherwise keep full funding through settlement.

**Acceptance:** conserved cash and positions across the complete lifecycle;
reproducible causal decisions; net results after declared costs, residual losses,
capital-time and event concentration. Compare policies under equal risk limits.
Sensitivity to plausible latency/liquidity assumptions must be visible. A policy
is selected for better economic results within those limits, not more fills alone.
If its useful margin disappears under supported assumptions, revise that policy
or the selected family before building more execution machinery for it.

### P4 — Operational integration

**Deliverable:** an adapter around the same decision core, running in shadow mode
on observed data, plus order lifecycle/reconciliation exercised with deterministic
fixtures and appropriate demo access. Add centralized limits, kill switch,
reconnect/restart handling and an operator procedure before enabling submission.

**Acceptance:** recorded live inputs reproduce decisions; observed timings inform
simulator assumptions; uncertain order state stops new exposure; outstanding
orders, fills and balances reconcile. Demo execution requires explicit authorization
and is not evidence of production fills. P4 does not introduce a second strategy
implementation or use a replay model as an exchange transport.

### P5 — Actual economic evidence

With separately authorized execution, use a declared capital/loss budget and
freeze the selected policy, fees and operating-cost accounting. Record attempts,
actual fills, settlement cash, drawdowns, capital holding time and concentration.
Compare observations with P3 assumptions and correct discrepancies before scaling.
Success is repeatable net benefit within the stated constraints over a disclosed
sample, not one positive trade. No income target or completion percentage is
inferred from papers or engineering milestones.

## Optional extensions: entry conditions, not automatic tasks

| Extension | When to evaluate it | Boundary |
|---|---|---|
| Bounded baskets of three or more legs | P2 identifies reviewed multi-outcome groups where pair coverage is insufficient and a cost/depth screen supports a useful opportunity hypothesis. | Separate payoff/metadata extension; reuse the same sizing, ledger and execution machinery. Do not promise atomic acquisition. |
| Joint capital allocation | P2/P3 shows competing candidates losing useful margin through shared funds or depth. | Compare a bounded allocator with a small offline oracle; no assumed reuse of unsettled proceeds. |
| Passive/FOK/inventory-model policies | P3 identifies acquisition/hedging cost as the limit and suitable observations can support the model. | One policy experiment at a time, same loss/capital limits; passive fills require queue/adverse-selection treatment. |
| Specialized threshold index, SIMD, PGO, concurrency or hardware | Profiling and economic sensitivity identify a relevant bottleneck. | Portable reference, equivalent behavior and measured maintenance/memory cost. |
| Predictive or cross-venue strategies | A separate economic hypothesis justifies the added risk and scope. | Outside the initial strategy; no implicit inclusion because a paper mentions it. |

A selected extension updates this plan with its scope, prerequisite, acceptance
and next branch before implementation. Insufficient pair opportunities are not
an instruction to implement every optional technique. They require a bounded
choice of which hypothesis to test next.

## Architecture and performance rules throughout

Keep one owner for mutable market/decision state. Replay, simulation and transport
supply explicit events and time; they share business logic. Extract interfaces
when reuse requires them, not as empty abstractions for possible future models.
Keep venue/JSON concerns out of `eme_core`; preserve fixed-point arithmetic,
versioned semantics, stale handling and exact replay.

Each changed measured path has correctness checks, representative cost measurements
and preserved workload/build provenance. Compare performance-only variants with
identical outputs. New behavior can cost CPU: compare its economics and timing
separately rather than reporting a misleading speedup. Use per-event distributions
and scheduled arrivals when measuring backlog. CI checks portability and behavior;
shared CI hosts do not establish latency rankings. Hardware recommendations follow
measurements on intended deployment targets, including operating cost.

[PERFORMANCE.md](PERFORMANCE.md) owns benchmark procedures/results.
[ECONOMIC_VALIDATION.md](ECONOMIC_VALIDATION.md) owns evidence definitions, not
another implementation sequence. Research stays in
[QUANT_RESEARCH.md](QUANT_RESEARCH.md) and
[ECONOMIC_STRATEGY_RESEARCH.md](ECONOMIC_STRATEGY_RESEARCH.md).

## Reconciliation of the previous documents

| Earlier ambiguity | Decision now |
|---|---|
| Depth/fees/replay/simulator sometimes described as wholly future work. | They are implemented reference capabilities; P1 and P3 extend them. |
| Live history required by v0.2, but transport and all credentials deferred to v0.4. | Read-only capture belongs to P2; order integration belongs to P4. |
| Research sequence appeared to require baskets and advanced models next. | P1 is committed next work; extensions require the entry conditions above. |
| Largest funded size versus the intended best net size. | The former is a baseline; P1 implements the latter for full acquisition. |
| Generic pending generated tests despite existing generated/differential checks. | Preserve existing checks and add cases for changed behavior or identified gaps. |
| Infrastructure/research completion presented as the goal. | Net economic performance is the objective; correctness and measurements support it. |

These were differences in status, priority and model scope, not evidence that the
core strategy requires incompatible architectures. The chosen consolidation follows
incremental delivery and outcome ownership, explicit contracts, existing stack and
adding complexity when needed. P2 now adds one bounded writer thread at the
persistence boundary; mutable market state still has one owner and the optional transport uses Boost.Beast/Asio and OpenSSL, without adding these
dependencies to the offline core.

## Delivery discipline

Use Conventional Commits and branches such as `feat/net-profit-sizing`,
`feat/readonly-market-capture` and `perf/costed-sizing`. Preserve a working baseline
and integrate completed work after its review/checks. For each completed block,
update status here with the revision, behavior delivered, economic observation and
remaining uncertainty; update format/architecture documents only where their
contracts change. Report phases as implemented, measured, pending or data-dependent.
Do not calculate a global percentage from document count or completed PR count.
