# Project direction

## Objective

Build a deterministic C++20 engine that identifies pricing inconsistencies across
related Kalshi contracts, sizes portfolios against executable liquidity, and
eventually evaluates and controls their execution. The economic objective is to
capture repeatable net profit within explicit capital and risk limits.

Engineering improvements should increase the margin that can actually be captured,
the capacity that can be handled, or the reliability of the evidence. Latency,
throughput, memory use, execution quality, and operating cost are measured together.

Research includes public methods from quantitative trading firms generally,
economic and microstructure papers, execution models and inventory control.
Jane Street and Optiver are examples of relevant sources. Apply a method when its
assumptions fit the venue and its measured economic benefit justifies its complexity.
The [quantitative research supplement](../research/QUANT_RESEARCH.md) records evidence,
limitations and proposed experiments, including the September 2026 review.

This makes the project a structural-arbitrage and execution optimizer. It extends
the existing market-data and payoff-verification foundation.
[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) is the single source for work
order, current status and adoption of research proposals. The roadmap only maps
those capabilities to release milestones.

## Economic mechanism

Start from reviewed relationships between contract outcomes. If a portfolio's
minimum settlement across all valid outcomes exceeds its acquisition cost and
applicable fees, it has a positive terminal margin once every required position
has been acquired. The guarantee depends on the contract relationship and
settlement rules being correct.

The initial relationship families are implication and complement, which the core
already supports. Complete baskets of mutually exclusive and exhaustive outcomes
are a potential extension, subject to reviewed definitions and implementation.
Relationships must cover resolution rules, timing, and exceptional outcomes;
similar titles or correlated prices are insufficient.

For example, a hypothetical complete basket that costs $0.96 including acquisition
fees and pays $1 at settlement locks in a $0.04 terminal margin after it is fully
assembled. Available depth, changing prices, incomplete execution, and the time
until capital is released determine whether that opportunity is useful. This is
an illustration, not an observed result from the engine.

The evaluator must distinguish certified payoff, venue collateral mechanisms and
account/event eligibility. Measure peak funding along the acquisition path and
remaining capital-time exposure, including any eligible early collateral return.
An early cash release must not be counted twice as profit. Fee accounting must
preserve per-order rounding state across partial fills. These are requirements
for evaluation: an offline fee/funding reference is implemented, while eligible
collateral release and capital-time accounting remain extensions. Source details
are in [the supplement](../research/QUANT_RESEARCH.md).

## Intended decisions

The completed evaluation layer should explain:

1. **What to acquire:** the relationship, its provenance, the required positions,
   and the verified minimum payout.
2. **How much to acquire:** the quantity that maximizes net value within available
   depth, applicable fees, capital availability, and exposure limits.
3. **How to execute:** the expected trade-offs between execution policies,
   including the possibility and cost of acquiring only some positions.
4. **Where to allocate capital:** how opportunities compete for liquidity and
   capital, including concentration and time until funds become available again.
5. **What happened:** the observed decisions, executions, costs, positions, and
   reconciled outcome, with enough information to reproduce the analysis.

The policy objective is realized net performance under risk limits. A positive
quote-based margin is evidence for evaluation; it is not itself an executed profit.
Portfolio sizing and capital allocation are intended capabilities, not claims
about the current implementation.

## Current implementation and next layers

At the reviewed local baseline `7cf248f` on September 15, 2026, exact types,
books, generation-aware processing, decoding, journal and payoff verification
are implemented, together with metadata/session binding, incremental gross
candidates and general structured replay. The offline study adds costed depth,
exact fee accounting, conservative reservations and delayed IOC simulation.
The CLI can inspect metadata, pack/verify/import sessions, replay and study them.

The current sizing policy chooses the largest funded size, which can omit a
smaller profitable trade. The simulator does not yet provide response/hedge/
settlement accounting. There is no authenticated capture, order transport or
demonstrated realized profitability. See [OFFLINE_STUDY.md](../guides/OFFLINE_STUDY.md)
for the implemented contracts and limitations.

The next work is profit-aware sizing and representative read-only observations,
followed by execution-policy comparison and operational integration. Exact
dependencies, acceptance criteria and conditional extensions are maintained in
[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md), rather than duplicated here.

## Optimization policy

Preserve exact arithmetic, stable identities, deterministic replay, strict input
validation, and fail-closed behavior while optimizing. Retain a clear reference
implementation and compare optimized results after each event, including errors
and recovery.

Prioritized experiments include dependency-driven evaluation; compact order-book
layouts; fewer allocations and copies in decoding; SIMD parsing where beneficial;
buffer reuse and compatible checksum acceleration; and a single owner of mutable
market state with bounded queues only where the pipeline needs them.

Measure both individual components and the full processing path. Report latency
distributions under realistic arrivals and bursts, queueing, memory, and the
hardware/compiler configuration. Then measure whether improvements preserve more
executable opportunities or reduce execution losses. A faster microbenchmark alone
does not establish an economic improvement.

Advanced threading, specialized network I/O, or hardware-specific optimizations
require a measured bottleneck and a reproducible comparison. The current C++20
architecture is the starting point; implementation choices are experiments rather
than promises of a particular latency or return.

## Evidence and success criteria

Keep separate records for theoretical payoff guarantees, quote-based opportunities,
simulated executions, actual fills, and realized results. Evaluate costs, incomplete
portfolios, capital usage, and operating expenses explicitly. Record failed
experiments and evaluate on periods or events not used to tune the policy.

Success can be demonstrated at several levels:

- reproducible correctness and recovery across captured and generated inputs;
- measured performance improvements with equivalent outputs;
- execution-aware opportunities that survive conservative cost assumptions;
- eventually, reconciled positive net results over a stated period and capital
  base, with the included costs and uncertainty disclosed.

The project may establish strong engineering results before establishing economic
viability. A small observed profit does not by itself demonstrate a persistent
advantage, and a backtest must remain labeled as a backtest.

## Research basis

These sources support the direction and experimental methods. They are not the
recovered original project research, nor proof of current profitability on Kalshi.
Sources were reviewed on September 14, 2026.

- Saguillo et al., [Unravelling the Probabilistic Forest: Arbitrage in Prediction
  Markets](https://arxiv.org/html/2508.03474v1), 2025 preprint. Documents structural
  arbitrage and reconstructs participant activity on Polymarket. Its aggregation,
  pricing, and position-reconstruction assumptions limit direct replication claims.
- Gebele, Mutzel, and Matthes, [Executable Arbitrage and Market Efficiency in
  Prediction Markets](https://arxiv.org/html/2608.00666v1), 2026 preprint.
  Distinguishes terminal payoff identities from executable conversion and
  settlement mechanisms. Reported estimates include imputed position values;
  Polymarket conversion primitives must not be assumed to exist on Kalshi.
- Kroer et al., [Arbitrage-Free Combinatorial Market Making via Integer
  Programming](https://www.columbia.edu/~ck2945/papers/milp_market.pdf), ACM EC 2016.
  A reference for larger combinatorial outcome spaces. Its market-maker problem
  differs from the current detector and does not justify a solver on every update.
- Langdale and Lemire, [Parsing Gigabytes of JSON per
  Second](https://arxiv.org/abs/1902.08318), The VLDB Journal, 2019. Supports a
  reproducible SIMD-parser comparison on the engine's own messages.
- Thompson et al., [LMAX Disruptor](https://lmax-exchange.github.io/disruptor/disruptor.html),
  technical paper, 2011. Informs state ownership, bounded buffers, and contention
  experiments; its historical performance figures are not engine targets.
- Bailey et al., [The Probability of Backtest
  Overfitting](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2326253), 2015.
  Informs experiment tracking and evaluation of policies selected through repeated
  testing.

Venue behavior must additionally follow current Kalshi documentation, including
[order-book conventions](https://docs.kalshi.com/getting_started/order_direction),
[fee rounding](https://docs.kalshi.com/getting_started/fee_rounding), and
[batch-order responses](https://docs.kalshi.com/api-reference/orders/batch-create-orders-v2).
Neither a mathematical payoff identity nor a batch request establishes atomic
execution of a portfolio across markets.
