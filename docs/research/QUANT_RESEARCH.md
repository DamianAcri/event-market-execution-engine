# Quantitative research and execution methods

Follow-up: [Applied economic research, September 15, 2026](ECONOMIC_STRATEGY_RESEARCH.md)
reviews the later `7cf248f` offline-study baseline, additional primary papers and a
reproduced sizing counterexample. Both research documents supply evidence and
candidate experiments; [IMPLEMENTATION_PLAN.md](../project/IMPLEMENTATION_PLAN.md) alone
selects their priority and entry conditions. The dated implementation references
below remain historical, not a list of work still to do.

This supplement connects public quantitative-trading research to the engine's
economic objective: repeatable net profit within capital, execution-risk and
operating-cost limits. Jane Street and Optiver are examples of relevant firms;
the research scope includes quantitative firms generally, market microstructure,
economic models, optimal execution, inventory control and efficient engineering.
Select techniques by their applicability and measured contribution to the result.

Sources were reviewed on September 14, 2026. Venue documentation is a dated input
to implementation and must be checked again when integrating the service. No
Kalshi account configuration or production execution was inspected in this review.
Statements marked **Evidence** describe sources; **Application** describes proposed
work in this engine. A research model or simulation is not evidence of its live
profitability here.

The review retains the C++20 structural-arbitrage direction. It adds explicit
funding-path requirements, refines fee accounting and execution experiments, and
qualifies historical maker-return evidence. The priorities and acceptance criteria
are maintained in [IMPLEMENTATION_PLAN.md](../project/IMPLEMENTATION_PLAN.md); low-level
experiments remain in [PERFORMANCE.md](../engineering/PERFORMANCE.md).

## Implementation baseline and decisions

At the reviewed `main` baseline `9c2d2fa`, exact domain types, local books,
generation-aware processing, a checksummed journal, replay, compiled payoff
constraints and portable performance experiments are implemented. Reviewed
metadata loading is separate work in
[PR #3](https://github.com/DamianAcri/event-market-execution-engine/pull/3), reviewed
at `938da804a8a73299d21c6682d8b14ba855c03fc0`. These are dated reference points, not
a claim that the economic evaluator or authenticated execution already exists.

| Decision | Reason | Evidence needed from this project |
|---|---|---|
| Keep structural payoff constraints as the initial strategy | They match the implemented foundation and permit independently checked guarantees. | Executable depth and complete acquisition cost below the certified minimum payoff. |
| Model funding before ranking by capital efficiency | Final collateral alone does not describe a feasible acquisition path. | Cash and positions through each intermediate execution state. |
| Use an explicit fee policy and per-order accounting | Fragmented execution can carry accounting state between fills. | Hand-calculated examples and reconciliation against observed fees. |
| Begin with a conservative aggressive-execution reference | It gives later passive policies a concrete comparison. | Partial-leg losses, costs and completion rates under the same limits. |
| Share decision logic across simulation and production | Differences between implementations weaken research conclusions. | Identical decisions from the same normalized inputs and state. |
| Evaluate low latency economically | CPU speed is only one contributor to execution outcomes. | Sensitivity of net results to delays and infrastructure cost. |
| Defer sophisticated binary market-making policies | Relevant models still require calibration and a reliable baseline. | Out-of-sample improvement over a simpler policy with equal risk constraints. |

## Funding and collateral eligibility

**Evidence.** Kalshi documents early collateral return for certain mutually
exclusive and directional market groups. It is disabled by default for new users.
Event-level eligibility is fixed when the first order is placed, including an
unfilled or cancelled order. Receiving collateral can restrict subsequent sales
of the affected positions.[^1] The event API exposes `collateral_return_type`,
`mutually_exclusive`, settlement information and fee overrides.[^2] An authenticated
endpoint reports subaccount netting settings.[^3]

**Application.** Keep three concepts separate: a mathematical payoff certificate,
the venue mechanism available for the event, and the account/event state that
permits its use. Current account settings alone must not be assumed to reconstruct
the eligibility established by earlier orders.

Define funding scenarios before detector sizing: unavailable, verified eligible,
and unknown. Unknown state may support a labelled hypothetical scenario, but must
not silently permit executable sizing that depends on returned collateral. The
offline scenarios can use explicit synthetic account profiles without credentials.

Track maximum funding required during acquisition, remaining committed capital,
available cash and the cost or feasibility of unwinding. Include capital-time
exposure, for example dollar-hours, alongside absolute profit and worst losses.
A high return divided by a small final collateral figure is insufficient if an
earlier state cannot be funded.

Collateral release is an accounting transition: recognize its effect on remaining
rights and obligations so that the same value is not counted again as terminal
profit. Validate the ledger independently of the opportunity-ranking policy.

## Exact fees across partial fills

**Evidence.** Current Kalshi documentation expresses fees in six-decimal dollars.
Target balance precision differs between direct members ($0.0001) and non-direct
members ($0.01). Fills may have a trade fee, rounding adjustment and accumulated
rounding rebate. The accumulator belongs to the order and persists when an order
changes from taking liquidity to resting as a maker.[^4]

**Application.** Preserve exact `Cash`, quantity and price types. A stateless
`fee(price, quantity)` calculation is insufficient for all execution paths.
Separate the applicable fee model from its rounding/accounting rules and retain
both versions in the session's economic assumptions.

Use a per-order ledger in the simulator. The detector can emit a conservative
cost estimate or bound; both layers must use compatible rules and explain the
difference between estimated and realized charges. Test a single fill versus
equivalent fragmented fills, maker/taker transitions, fractional quantities,
cancelled residual quantity and both account precision profiles. Expected results
must come from independent arithmetic examples rather than the implementation.

## Execution policy and historical maker evidence

**Evidence.** Bürgi, Deng and Whelan document favorite-longshot bias and differences
between makers and takers in Kalshi. Their sample ends in April 2025 and their
analysis uses the historical regime without maker fees. It excludes markets open
for less than 24 hours and applies activity filters. They discuss limited depth,
unmatched orders and substantial risk relative to average returns.[^5]

**Application.** These results justify hypotheses, not present-day maker-profit
forecasts. Compare aggressive and passive execution under current fee policies,
equal capital limits and the actual contract families studied by the engine.
Selecting completed trades from a historical dataset does not establish what a
new participant could have filled.

**Evidence.** Lehalle and Mounjid combine AstraZeneca data from NASDAQ OMX in 2013
with a control model linking order-book imbalance, adverse selection and latency.
Their model shows how delay erodes the benefit of cancelling and repositioning
orders.[^6] Kalshi exposes a queue-position endpoint for an existing order, reporting
preceding quantity under price-time priority.[^7]

**Application.** Study passive fills jointly with subsequent price movement and
the cost of completing the remaining portfolio. Independent random fill draws
can hide adverse selection and correlated changes across legs. Later observations
of our own queue positions can help calibrate assumptions; they do not reveal the
complete historical order flow of competitors.

Keep a conservative aggressive-execution policy as the reference. A later passive
policy must specify when to wait, cancel, complete a hedge or stop attempting it,
and account for residual exposure. Compare policies on the same reserved sessions.

**Evidence.** Kalshi's batch API reports fills and remaining quantities per order;
the referenced page does not promise atomic completion of a multi-market
portfolio.[^8] **Application.** Model partially acquired portfolios explicitly.
Submitting instructions together is not a substitute for intermediate-state risk
and funding controls.

## A candidate model for binary contracts

**Evidence.** Feil and Nendel's July 2026 preprint models prediction-market prices
as conditional probabilities, binary settlement and inventory-sensitive quoting
through stochastic control. Its numerical experiments use illustrative parameters;
section 4.1 explicitly says they are not calibrated to a particular contract. The
objective includes inventory risk during trading and at settlement.[^9]

**Application.** Retain this as a candidate for residual-inventory management and
later passive quoting. It addresses a different problem from the initial
cross-market payoff detector. Compare any implementation with a simple policy
under equal risk bounds before using its greater complexity.

If warranted, an engineering experiment could compute a policy offline and use
small lookup tables online. Validate discretization error and parameter
sensitivity separately from lookup latency. No model solver is proposed for the
current per-delta path, and simulated paper results are not a return estimate.

## Public firm engineering methods

**Evidence: Optiver.** Daniel B.'s July 2026 account describes successive trading
stacks using visual graphs, compilation and native code. Compiler maintenance and
the boundary between research and engineering became costs as the system grew.
The article emphasizes inexpensive abstractions, iteration speed and consistent
behavior between simulation and production. Its reported outcomes are qualitative,
without performance figures transferable to this engine.[^10]

**Application.** Use one decision core with explicit inputs and outputs. Replay,
simulation and live adapters can differ while invoking the same business logic.
Exploratory analysis may use other tools; avoid maintaining independent production
and simulated strategy implementations. Favor small modules for contract rules,
market state, accounting, evaluation and execution policy.

**Evidence: Jane Street.** Yaron Minsky describes how Incremental developed from
self-adjusting computation research into a practical library.[^11] Doug Patti's
account of testing the Aria messaging system covers state machines, simulated time
and network behavior, generated event sequences, fuzzing and fault testing.[^12]
Tudor Brindus demonstrates diagnosing latency variation from Linux and
microarchitectural effects with per-observation distributions and timelines.[^13]

**Application.** Activate the existing market-to-constraint index before adding a
general computation framework. Compare affected-constraint evaluation with a full
scan, including duplicate dependencies, stale inputs and deterministic output
ordering. Measure dependency fan-out as well as total market count.

Make time, pending messages and exchange responses explicit in the simulator.
Retain seeds and reproducing sequences for failures. A correct book replay alone
does not establish correctness across late cancels, reconnects and process restarts.

Measure individual-event latency, queue age and bursts in addition to service-cost
batch means. Firm talks provide methods; their example hardware and latency figures
do not determine this project's targets. Portability and complexity remain part of
each optimization decision.

## Observability and simulator limits

**Evidence.** Aquilina, Budish and O'Neill use London Stock Exchange messages to
study latency races, including failed immediate orders and failed cancellations
that ordinary book data does not fully reveal.[^14] HftBacktest documents that
historical replay is not changed by simulated orders and can give unrealistic
liquidity-taking fills; aggregated depth also requires queue assumptions.[^15]
ABIDES provides an academic example of event-driven market simulation with
configurable participant-to-exchange delays, inspired by equity protocols.[^16]

**Application.** Extend recording from market inputs to order intent, send,
acknowledgement, fill, rejection, cancel and reconciliation events as those
capabilities are introduced. Include attempts that capture nothing. Our own
messages support evaluation of our decisions without reconstructing every race
between other participants.

The simulator must prevent double consumption of its assumed available liquidity.
This prevents one class of replay error but does not model other participants'
counterfactual responses. Label observed data, assumed fills and missing impact
separately. A small simulator with explicit limitations is a useful initial tool.

Prioritize disappearing liquidity, one filled leg with another rejected, fills
during cancellation, duplicates, stream gaps, disconnects, restarts and incomplete
sessions. Check cash, positions, orders and validity state after each event. Use
local monotonic durations appropriately; timestamps from unrelated clocks do not
establish precise one-way network delay without known synchronization semantics.

## Economic latency and capacity experiments

The following is a proposed experiment, not an existing benchmark or measured
exchange latency. Fix a policy and replay reserved sessions with controlled extra
delays for feed reception, decision processing, outbound orders and responses.
Keep inputs and random seeds paired between comparisons.

An exploratory added-delay grid could be 0, 0.1, 1, 5, 20 and 100 milliseconds.
Refine it around observed behavior after capture is available. Report the same
scenario under different assumptions about liquidity, fees and collateral.

| Output | Purpose |
|---|---|
| Quote-based margin | Identify the starting discrepancy. |
| Completed portfolios and acquired quantity | Measure whether the policy captures it. |
| Fees and partial-execution losses | Explain where apparent margin disappears. |
| Net profit and worst event loss | Compare results within risk limits. |
| Peak funding and capital-time exposure | Establish feasibility and capacity. |
| Stage latency and queue age | Locate costs that change decisions or outcomes. |
| Incremental operating expense | Evaluate whether faster infrastructure is worth its cost. |

The result is a simulated economic sensitivity curve with uncertainty, conditional
on the fill model. It should guide targeted profiling and eventually hardware
comparisons. Optimizing checksum cost remains a valid engineering result; its
economic value requires this additional measurement.

Capital scenarios of EUR 100, EUR 1,000 and EUR 10,000 are research assumptions,
converted to the trading currency under an explicit dated assumption. Compare
feasible acquisition paths, concentration and depth limits rather than generating
three income forecasts. Capacity need not scale proportionally with capital.

## Validation, research ownership and stopping criteria

**Evidence.** Bailey and Lopez de Prado's Deflated Sharpe Ratio addresses inflation
from selecting among multiple trials and non-normal returns.[^17] Gebele, Mutzel
and Matthes distinguish payoff identities from executable position transformations
on Polymarket; their reported estimates include imputed position values.[^18]

**Application.** Record all policy experiments, including negative results. Reserve
chronological periods and event families before tuning. Nearby ticks from the
same event do not provide independent economic evidence merely because they are
numerous. Any statistical resampling must respect relevant event dependencies.

Keep certified fully acquired payoff margins, simulated profits, actual fills and
settled cash distinct. Holding an uncovered leg introduces outcome exposure even
when the intended complete portfolio has a payoff guarantee. Statistical tools do
not remove poor fill assumptions or insufficient independent observations.

An economics collaborator can independently review payoff/cash examples, capital
constraints, contract-family selection and economic comparison criteria. Jointly
define hypotheses before evaluating results. Engineering owns reproducibility,
state transitions and measured implementation costs; economic review challenges
the assumptions that make those measurements meaningful.

Reject a policy when its apparent edge requires impossible fills, future
information, duplicated collateral or unverified eligibility; when realistic costs
erase the margin; or when residual losses violate the agreed limits. Reconsider
infrastructure expenditure when its plausible incremental benefit cannot cover
its additional cost.

The implementation sequence is maintained in
[IMPLEMENTATION_PLAN.md](../project/IMPLEMENTATION_PLAN.md). Sessions, the initial detector,
accounting reference and first execution simulator have since been implemented;
their presence does not establish profitability. Further papers should answer
specific gaps in the selected experiment rather than expand an automatic backlog.

## Sources

[^1]: Kalshi, [Collateral Return](https://help.kalshi.com/en/articles/13823816-collateral-return), May 17, 2026. Venue documentation.
[^2]: Kalshi, [Get Event](https://docs.kalshi.com/api-reference/events/get-event). API documentation, reviewed September 14, 2026.
[^3]: Kalshi, [Get Subaccount Netting](https://docs.kalshi.com/api-reference/portfolio/get-subaccount-netting). API documentation, reviewed September 14, 2026.
[^4]: Kalshi, [Fee Rounding](https://docs.kalshi.com/getting_started/fee_rounding). Documentation, reviewed September 14, 2026; overview, rounding mechanics and fee accumulator.
[^5]: Constantin Bürgi, Wanying Deng and Karl Whelan, [Makers and Takers: The Economics of the Kalshi Prediction Market](https://www2.gwu.edu/~forcpgm/2026-001.pdf), January 2026 working paper. Sections 2, 3.1 and 6 specify fees, sample selection and economic limits.
[^6]: Charles-Albert Lehalle and Othmane Mounjid, [Limit Order Strategic Placement with Adverse Selection Risk and the Role of Latency](https://arxiv.org/html/1610.00261), first circulated in 2016. Sections 2, 4 and 5; empirical equity data and a control model.
[^7]: Kalshi, [Get Order Queue Position](https://docs.kalshi.com/api-reference/orders/get-order-queue-position). API documentation, reviewed September 14, 2026.
[^8]: Kalshi, [Batch Create Orders (V2)](https://docs.kalshi.com/api-reference/orders/batch-create-orders-v2). API documentation, reviewed September 14, 2026.
[^9]: Dominik Feil and Max Nendel, [Optimal Market Making in Prediction Markets](https://arxiv.org/html/2607.17991v1), July 20, 2026 preprint. Sections 4.1–4.4 describe illustrative parameters and numerical experiments.
[^10]: Daniel B., Optiver, [Designing for latency and research iteration](https://www.optiver.com/insights/technology-blog/designing-for-latency-and-iteration/), July 20, 2026. First-party engineering account; qualitative outcomes.
[^11]: Yaron Minsky, Jane Street, [Seven Implementations of Incremental](https://www.janestreet.com/tech-talks/seven-implementations-of-incremental/). Official talk and transcript; no date displayed on the reviewed page.
[^12]: Doug Patti, Jane Street, [Getting from tested to battle-tested](https://blog.janestreet.com/getting-from-tested-to-battle-tested/), December 3, 2025. First-party Aria testing account. It discloses Jane Street's investment in Antithesis; this plan proposes testing methods, not a product purchase.
[^13]: Tudor Brindus, Jane Street, [System Jitter and Where to Find It: A Whack-a-Mole Experiencer](https://www.janestreet.com/tech-talks/system-jitter-and-where-to-find-it/). Official talk and transcript; no date displayed on the reviewed page.
[^14]: Matteo Aquilina, Eric Budish and Peter O'Neill, [Quantifying the High-Frequency Trading Arms Race](https://academic.oup.com/qje/article/137/1/493/6368348), The Quarterly Journal of Economics 137(1), 493–564, 2022; online publication in 2021. Sections II–III and conclusion discuss failed messages.
[^15]: HftBacktest, [Order Fill](https://hftbacktest.readthedocs.io/en/latest/order_fill.html). Project documentation, reviewed September 14, 2026; replay, liquidity and queue-model limits.
[^16]: David Byrd, Maria Hybinette and Tucker Hybinette Balch, [ABIDES: Towards High-Fidelity Market Simulation for AI Research](https://arxiv.org/abs/1904.12066), April 26, 2019 preprint. Simulation architecture, not Kalshi calibration.
[^17]: David H. Bailey and Marcos López de Prado, [The Deflated Sharpe Ratio: Correcting for Selection Bias, Backtest Overfitting and Non-Normality](https://www.davidhbailey.com/dhbpapers/deflated-sharpe.pdf), authors' July 31, 2014 version for The Journal of Portfolio Management.
[^18]: Jonas Gebele, Timm Mutzel and Florian Matthes, [Executable Arbitrage and Market Efficiency in Prediction Markets](https://arxiv.org/html/2608.00666v1), August 1, 2026 preprint. Previously included research, revisited for execution mechanisms and imputed results.
