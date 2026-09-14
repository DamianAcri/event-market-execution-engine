# Evidence required to continue the economic hypothesis

This is the decision plan for the existing two-market implication/complement
acquisition model. It complements [implementation deliverables](IMPLEMENTATION_PLAN.md)
and [the research supplement](QUANT_RESEARCH.md). Engineering completion and
economic evidence are separate: a synthetic positive example, a fast benchmark,
or a clean replay is not evidence of realized profit.

## Current position

The project can validate metadata, bind session artifacts, process normalized
books and track deterministic **gross candidates** from their best prices.
Tests independently check cash/payoff examples and repeat candidate lifecycle
events through the existing journal/decoder/state pipeline with explicit fixture
controller actions. See [CANDIDATES.md](CANDIDATES.md).

Recorded controller history and a general structured CLI replay remain unfinished.
The costed detector, realistic execution simulator and observed opportunity study
do not exist yet. We have not measured venue opportunity frequency, achievable
net returns, capacity or economic latency sensitivity.

## Evidence gates

| Gate | Required evidence | Decision |
|---|---|---|
| 1. Correct calculation | Independently calculated cash/position examples; gross and net margins; fractional quantities, fee rounding and partial fills; incremental output equal to a complete reference; repeatable replay with explicit controller history. | Fix discrepancies before interpreting financial output. Synthetic fixtures establish behavior only. |
| 2. Observed opportunity supply | Reviewed contract semantics and representative recorded sessions; book validity checks; count, duration, quantity and funding of candidates; a costed immediate-execution upper bound with assumptions stated. | If even this optimistic bound cannot cover the chosen costs in the tested scope, change or stop that hypothesis. Insufficient coverage or unreliable data is inconclusive. A positive upper bound only justifies further evaluation. |
| 3. Execution realism | A fixed policy on held-out periods/event families; order-arrival delays, disappearing/shared liquidity, partial-leg acquisition, rejects/cancels and funding constraints; exact cash and inventory conservation. | Continue only if the result remains useful after declared operating costs and plausible adverse execution assumptions. Reject a result dependent on optimistic fills or unbounded intermediate exposure. Otherwise revise and evaluate on new held-out data. |
| 4. Operational observations | Demo transport, reproducible decision traces, reconnect/failure exercises, centralized limits and reconciliation; observed timings compared with simulator assumptions. | Repair operational gaps and recalibrate the model. Demo fills are not production execution evidence. |
| 5. Separately authorized execution evidence | Recorded actual fills, fees, settled cash, committed capital, drawdowns and operating costs, with production controls enabled explicitly. | Assess realized results and capacity before scaling. Earlier gates do not authorize order submission. |

Before an observed experiment, freeze the relationship set, account/fee/funding
profile, capital budget, decision policy, chronological split and evaluation
criteria. Record exclusions and every attempted variant. Do not choose the
acceptance threshold after inspecting the held-out results. Calendar duration
alone is not adequate coverage: document event families, market conditions,
candidate counts and concentration. Uncertainty remains visible when the sample
cannot support a useful conclusion.

The gate-2 bound must not sum simultaneous candidates that consume the same
liquidity or exceed the same capital budget. Gross quote margin, modeled net
margin, simulated execution result and realized net cash are separate columns.
Unknown account eligibility cannot silently enable collateral-dependent funding.
Use explicit hypothetical profiles until their configuration has been observed.

## The first useful report

For each recorded session and fixed policy, report:

- coverage, rejected input, gaps, recovery intervals and reviewed metadata identity;
- candidate openings, updates, invalidations, durations and limiting quantities;
- gross margin, fees, costed margin and reasons candidates were rejected;
- completed portfolios, incomplete-portfolio losses and simulated net results;
- peak funding, capital-time exposure and overlapping liquidity consumption;
- sensitivity to feed/decision/order/response delays and adverse fill assumptions;
- operating costs and results by chronological period and event family.

The first two lines require complete replay integration; the remaining lines need
the costed evaluator and simulator. The current tracker does not produce this
economic report. Its events have to be wrapped with recorded causal indices and
times before measuring duration or attributing performance.

## Where infrastructure work stops being the immediate priority

Finish the replay/control-history boundary needed for the first report, then
implement the costed evaluator and simulator. Keep correctness tests and portable
benchmarks with each change. Add infrastructure when a measured limitation or a
specific validity requirement blocks that chain.

Do not postpone observed data collection until the rest of the product is
finished: once recording and replay can preserve the needed inputs, transport
for collection becomes useful. Offline fixtures and simulation need no keys;
observed venue data is needed to test supply and execution assumptions.

Use the [economic latency experiment](PERFORMANCE.md#economic-latency-experiment)
to prioritize later low-level optimizations: compare net result and exposure under
controlled additional delays at equal inputs and limits. A local microsecond gain
can be technically real and still have no measurable economic value.
