# Implementation plan

Updated: 2026-09-14. Direction: [PROJECT_DIRECTION.md](PROJECT_DIRECTION.md).
Milestone ownership remains in [ROADMAP.md](ROADMAP.md). Performance methodology
and public research are in [PERFORMANCE.md](PERFORMANCE.md).
The [quantitative research supplement](QUANT_RESEARCH.md) explains the funding,
accounting, model-selection and execution evidence behind the requirements below.
The [economic validation plan](ECONOMIC_VALIDATION.md) defines the evidence needed
to continue, revise or stop the hypothesis; completed engineering steps alone
do not answer that question.

## Objective and working rule

Deliver a deterministic path from a recorded market session to an explainable,
costed portfolio opportunity, then validate execution under realistic constraints.
Optimize time and resources spent making a useful decision. Local processing,
transport, exchange response, and capital holding time are separate measurements.

Each change to a measured path carries its correctness checks, a before/after
comparison on identical workloads, and a statement of what was not measured.
New functionality can legitimately cost CPU; its incremental cost must be visible.
An optimization is retained only if equivalent results and a repeatable benefit
justify its memory, complexity, and maintenance costs.

## 0. Establish the performance baseline

Merged in PR #2: offline workloads, comparison runner,
compatibility checks and the first measured CRC/book optimizations. Results and
their limits are recorded in [PERFORMANCE.md](PERFORMANCE.md). Metadata and session
binding have since been merged in PRs #3 and #5.

Deliverables:
- Optional offline Release benchmarks for journal CRC/codec, multi-market book
  updates and churn, Kalshi decode/normalize/apply, and verified in-memory replay.
- A runner that alternates prebuilt baseline/candidate processes, preserves raw
  results and executable hashes, and refuses incompatible workload/output digests.
- Independent CRC reference and generated record/truncation checks.
- A measured first optimization that preserves existing wire and state contracts.

Acceptance:
- Core-only build works without JSON or any additional benchmark dependency.
- Full tests and sanitizer checks pass; smoke tests validate fixtures in CI.
- Both binaries use the same benchmark source, compiler, flags and inputs.
- Batch averages are explicitly distinguished from individual-message tails.
- No performance threshold is enforced on shared CI hosts.

## 1. Finish the reproducibility contract (v0.2)

Steps 1–2 were merged in PR #3, including offline CLI
inspection and generated payout checks. See [METADATA_FORMAT.md](METADATA_FORMAT.md).
Step 3 was merged in PR #5: new-directory session
publication, exact artifact fingerprints, count validation and offline pack/verify.
See [SESSION_FORMAT.md](SESSION_FORMAT.md) for its integrity and durability scope.
Step 4 was merged in PR #6: deterministic identity, incremental updates and
opened/updated/invalidated gross events. Step 5 now has a general offline CLI,
manifest-bound controller plans and deterministic structured output.
[OFFLINE_STUDY.md](OFFLINE_STUDY.md) documents the first depth/fee/funding evaluator
and aggressive IOC simulation, including a small observed REST pilot. Live
controller capture and venue sequence-scope validation still require transport.

Implement in this order:
1. Define a strict, versioned snapshot format for stable market IDs, tickers and
   curated implication/complement definitions, including provenance.
2. Load and validate the entire snapshot into temporary state. Reject duplicate
   IDs/keys, unknown market references and unsupported versions before publishing.
3. Persist that immutable snapshot beside the journal, with a session manifest
   identifying the exact artifact. Define crash/incomplete-session detection.
   As economic policies are introduced, bind their exact fee, rounding, funding
   and account/event assumptions to the session; mark hypothetical profiles.
4. Add deterministic opportunity identity using definition version, direction
   and market dependencies; model opened/updated/invalidated lifecycle events.
   Quote changes update an identity instead of generating a new one every tick.
5. Connect an offline CLI replay to the session manifest and structured output.
   Persist explicit connection/recovery actions and subscription conventions,
   including the gateway's required `use_yes_price: true`; never infer the price
   scale from a payload or silently repair a missing control history.

Acceptance: registration order cannot change identity; mismatched or incomplete
sessions fail closed; two runs produce identical ordered output; generated small
worlds agree with an independent exhaustive oracle; stale books invalidate all
dependent opportunities. Metadata loading and compilation stay outside delta
processing. This phase requires no credentials.

## 2. Make the first detector economically meaningful (v0.3)

Before sizing, define independently calculated cash/position examples for full
and partial acquisition, collateral eligibility and per-order fee accumulation.
Use explicit synthetic account profiles until observed configuration is available.

Start with the existing two-market implications and complements:

1. Activate the compiled market-to-constraint dependency index so a delta visits
   only affected constraints. Compare outputs with a full-scan reference.
2. Walk executable depth on all required legs and find feasible quantities.
3. Apply an explicit, versioned fee policy with exact cash units and account
   balance precision. Define per-order accumulation across partial maker/taker
   fills; the detector emits a cost bound and the simulator applies each fill.
4. Emit cost, minimum payout, margin, limiting leg, committed capital and a
   concrete reason for each acceptance/rejection. Include book freshness policy.
   Model peak acquisition funding and capital-time exposure. Distinguish payoff
   guarantees, venue collateral mechanisms and account/event eligibility; unknown
   eligibility cannot silently enable sizing that depends on returned collateral.
5. Measure work per changed market across growing numbers of unrelated markets
   and constraints; add realistic dependency fan-out and burst workloads.

Acceptance: hand-calculated examples and a slower independent evaluator agree;
rounding cannot turn a non-positive margin into a positive one; shared liquidity
is not counted twice; missing/stale inputs never produce executable suggestions.
Collateral release conserves economic value rather than creating duplicate profit;
intermediate acquisition states fit the funding limit. Ledger examples cover
partial fills, maker/taker transitions, cancelled residuals and rounding profiles.
All of this can be implemented and tested without credentials. Synthetic data
does not establish how frequently such opportunities exist on Kalshi.

## 3. Model execution and calibrate with observations

Add an event-driven replay simulator with scheduled arrival times, configurable
outbound/response delays, disappearing liquidity, partial fills, rejected orders,
timeouts and recovery actions. Charge losses from incomplete portfolios and
include funding and eligible collateral release through exit/settlement. Separate
observed fills from simulated fills in every report. Fix parameters before
evaluating a held-out session.

Share the decision core across replay, simulation and eventual live adapters.
Make time and pending responses explicit, retain seeds and reproducing sequences,
and record failed order/cancel attempts as well as fills when execution is added.
An aggressive-execution reference precedes passive-policy comparisons, which must
model queue uncertainty, adverse selection and correlated partial-leg outcomes.
Reserve chronological periods and event families before tuning, and retain all
experiments. Historical replay must label its missing counterfactual market impact.

Acceptance: no future information affects a decision; conservation of cash and
positions holds; adverse execution cases can erase the quoted edge; slowdowns
increase measured queue delay rather than silently slowing the event generator.
Report how accepted opportunities change under latency and liquidity stress.
Use the [economic latency experiment](PERFORMANCE.md#economic-latency-experiment)
to compare complete portfolios, partial-leg losses, net results and peak funding
at equal risk limits. Advanced binary inventory models require calibration and
out-of-sample improvement over a simpler policy before adoption.

Offline simulation needs no keys. Authenticated market-data collection should
begin once this pipeline is ready enough to retain and replay sessions; delaying
it beyond that point delays validation of both opportunity frequency and latency.

## 4. Optimize the measured limiting stages

Run one experiment at a time, in the order justified by the complete path:
- Remove redundant searches/work and per-message allocation; preserve ownership.
- Compare tree, sorted contiguous and tick-indexed books across density, churn,
  market count and memory footprint, including cold/large working sets.
- Compare reusable JSON parsing/buffers with the existing strict decoder using
  malformed input, escaping, numeric limits and buffer-lifetime differential tests.
- Cache immutable payoff certificates and update only affected calculations.
- Decouple journal I/O only after defining loss, durability, queue-full and
  shutdown behavior. Measure CPU and actual I/O separately.
- Introduce bounded concurrency only for measured contention or blocking; verify
  ordering, overflow and fail-closed behavior under producer/consumer imbalance.
- Try representative PGO/LTO builds; assess held-out workload and target hardware.

Acceptance: no behavior/format divergence; repeated comparisons improve the
relevant path; memory cost and worst observed behavior are reported. Hardware-
specific acceleration follows a portable fallback and deployment evidence.

## 5. Demo integration and operational controls (v0.4)

Add authenticated transport, subscription/snapshot orchestration, reconnect and
backoff. Validate the venue's actual sequence scope for multi-market subscriptions
before relying on it. Add centralized limits, freshness checks, reconciliation,
kill switch and metrics for queue age, losses and recovery.

Acceptance: long demo sessions and disconnect/restart/failure injection preserve
validity; recorded sessions reconstruct the same decisions; local processing and
network latency are reported separately. Keys are required at this phase.

## 6. Evidence from execution

Separately authorized demo execution precedes any production order submission.
Production requires explicit enablement, risk limits, reconciliation and an
operational runbook. Evaluate realized net results against costs, committed
capital and drawdowns; neither a simulation nor a fast benchmark substitutes for
fills and settled cash. Scale only after measuring economic capacity.

## Change delivery

Use small branches such as `perf/journal-crc-benchmarks`, `feat/metadata-snapshot`
and `feat/incremental-opportunities`, with Conventional Commits. Each PR names
the delivered capability, behavior checks, applicable measurements and remaining
limitations. Preserve a working baseline; finish one verifiable block before
expanding the implementation scope.
