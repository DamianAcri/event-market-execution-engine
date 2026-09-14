# Project direction

## Objective

Build a deterministic C++20 engine that identifies pricing inconsistencies across
related Kalshi contracts, sizes portfolios against executable liquidity, and
eventually evaluates and controls their execution. The economic objective is to
capture repeatable net profit within explicit capital and risk limits.

Engineering improvements should increase the margin that can actually be captured,
the capacity that can be handled, or the reliability of the evidence. Latency,
throughput, memory use, execution quality, and operating cost are measured together.

This makes the project a structural-arbitrage and execution optimizer. It extends
the existing market-data and payoff-verification foundation. It does not change
the development order or execution gates in the [roadmap](ROADMAP.md).

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

At the documentation baseline of September 14, 2026 (`main` at `1693eba`), the
repository contains exact fixed-point types, local order books, generation-aware
market state, Kalshi decoding and normalization, a checksummed raw journal,
deterministic replay tests, and versioned constraint/payoff verification.

The constraint registry already indexes dependencies by market. The order-book
processor does not yet use that index to emit economically evaluated opportunities.
There is no authenticated transport, order submission, or demonstrated trading
profitability. The CLI exposes status, version, and journal verification.

The next layers follow the existing milestones:

- **Finish v0.2:** reviewed metadata loading and persistence, deterministic
  opportunity identity and lifecycle, and generated correctness tests.
- **Build v0.3:** incremental evaluation using executable depth, fees, slippage,
  and partial-execution scenarios; explain opportunities and measure performance.
- **Build v0.4:** authenticated demo connectivity, recovery orchestration,
  operational visibility, centralized limits, and a kill switch.
- **Later:** separately gated execution, reconciliation, and evidence from actual
  operation. Earlier milestones do not authorize or imply production submission.

See [README](README.md), [Architecture](ARCHITECTURE.md), and [Roadmap](ROADMAP.md)
for the maintained implementation details and milestone boundaries.

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
