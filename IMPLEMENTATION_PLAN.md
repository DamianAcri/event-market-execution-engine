# Implementation plan

Updated: 2026-09-17. This is the **single execution plan** for the project: priorities,
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

## Current decision — 2026-09-17, after interrupted observation and size research

The initial observation was attempted and is **not accepted as a complete
window**: host sleep interrupted receipt and the policy expired. See the
[capture audit](research/results/20260917-basket-live/README.md). The runner now
detects long scheduling gaps, but the old observer's integrated duration counters
do not certify uninterrupted coverage. No automatic repeat capture is scheduled.

The [all-size diagnostic](research/results/20260917-economic-frontier/README.md)
reproduces the original 24,054,100 correlated quantity checks, with no positive
all-taker margin at sizes 1–100. Hypothetical one-passive-leg quotes can be
positive, but their best examples have little or no public-trade support; the
largest assumes a 12-cent fill while the ask is 98 cents. This is an execution
feasibility question, not demonstrated profit or a reason to deploy a maker bot.

**Next deliverable: a bounded offline passive-entry feasibility probe**, using
the same conditional baskets and aggressive acquisition as the control.
This is an explicit research subgate before step 4's executor decision below;
it does not mark step 3's prospective evidence complete. Its literature mapping
and limitations are in the [applied review](ECONOMIC_STRATEGY_RESEARCH.md#revisión-aplicada-de-colas-costes-y-tamaños--17-de-septiembre-de-2026).

| Order | Work selected now | Acceptance / stop condition |
| --- | --- | --- |
| A — completed locally | Exhaust whole sizes with the original depth, fee and funding model; retain negative outcomes and compare one passive leg under two declared fee hypotheses. | Native parity and independent rational witnesses pass. No fill or PnL is imputed. Artifacts and source hashes are retained. |
| B — next implementation | Align public trades, book changes and candidate entry states within contiguous connection segments. Retain displayed queue ahead, depth of the two hedge legs, partial quantities and censoring; prevent look-ahead. | Synthetic cases distinguish trades from cancellations and invalidate on gaps/reconnects. No book reduction alone creates a fill. Cancellation ahead/behind and ambiguous message ordering are explicit scenarios, not claimed observations. A real-data report may correctly say execution is unidentified. |
| C — before another operator run | Specify one passive-entry policy and an aggressive control, verified public maker-fee terms or explicitly unresolved fees, conditional delayed hedge-cost scenarios and residual inventory limits. Predeclare cohort selection and future expiry splits. | No independent Bernoulli-fill shortcut, no shared-liquidity double counting, no claimed network/order latency from CPU timings. Delay grids are sensitivity assumptions until measured. If available trades cannot support the model, report what data is missing before asking for more collection. |
| D — prospective confirmation, not started | The user runs a declared read-only window that collects the required trades/books and continuity diagnostics, with later whole expiries reserved for confirmation. | Report coverage and distinct supported episodes, including zero/negative cohorts. A fixed duration alone is not sufficient. Stop or revise this scoped hypothesis if positive results rely on unobserved fills or optimistic-only assumptions. |

The existing operational cohort cap is not a research-derived optimum. Any new
market ranking must use prior information and retain a control/exclusion report;
do not select on profits measured in its evaluation window. Capital allocation
remains full gross funding with shared-liquidity constraints, not an optimized
investment policy. Settlement exception certification is still required before
calling these baskets guaranteed or enabling an executor.

Engineering work serves this gate: reuse the existing incremental cost kernel,
test decision parity, then measure backlog and tail latency under real bursts
and how delay affects hedge margin. Do not rewrite the production data structures
or adopt a general queue-reactive simulator without evidence of a bottleneck or
modeling need. No actual/demo orders, credentials or new long run are needed for
the immediate offline implementation.

## Continuous observation delivery — 2026-09-17 (delivery record)

Branch `feat/basket-observation` implements the tooling for step 3 below. The
native basket cost kernel is shared by REST screening and a streaming
`ReplayObserver`. The existing read-only collector records the feed and computes
conditional quote episodes concurrently, then compares the live result with
offline replay. Cache/dependency updates avoid reconstructing all books or
parsing a screen JSON on every tick. No basket executor is introduced.

The prospective runner registers the cohort, policy, collection window and source
hashes before streaming. It retains zero-margin cohorts, reports missing depth,
invalid-feed coverage and censored episodes, and does not sum shared-liquidity
quotes as independent profits. Quiet books stay valid in a contiguous stream;
policy expiry has its own deterministic clock. Full collection evidence over
multiple expiries remains pending; implementing these tools does not complete
the economic acceptance of step 3.

The originally planned initial window has now been attempted; its result and the
superseding next action are recorded above. The
[continuous observation command](READONLY_CAPTURE.md#continuous-conditional-basket-observation)
remains available. Its default 30-minute duration is an operational bound, not a
statistical sufficiency claim or an instruction to repeat it now.

**Local verification:** all 47 current CTest entries pass (46-suite run plus
the added callback benchmark smoke). The 28 local TLS/WebSocket
scenarios include basket observation with quiet books, sequence gaps, bursts,
public trades and policy expiry without a new price. Live and replay episode
records match exactly. The final simultaneous-count correction passes its
targeted rerun; 31 observer checks and 59 shared-sizing/REST checks also pass
AddressSanitizer and UndefinedBehaviorSanitizer. Evidence is in
[validation.json](research/results/20260917-basket-observation/validation.json).
Remote portability CI for this branch remains pending.

## Research decision — 2026-09-17, after the economic capture

**Preceding delivery at `2856a88`:** `feat/structural-basket-screen` implements the bounded
public preflight and native cost screen described in steps 1–2 below. Exact BTC
source/terms/boundaries and ordinary/all-NO payoff arithmetic are checked;
operative general-review outcomes remain unresolved, so every basket is explicitly
conditional and observation-only. The cohort freezes before books are fetched.
Fractional depth is retained; exact whole quantities 1–100 reuse the existing fee
ledger with incremental cumulative costs. A Python rational oracle checks the
native arithmetic. Persistent connections and bounded independent preparation
requests are implemented with isolated response/archive ownership. No basket
executor, continuous opportunity counter or new paper fill model is delivered.
See [READONLY_CAPTURE.md](READONLY_CAPTURE.md#btc-basket-public-preflight).

**Observed acceptance result:** the 14:09 UTC public snapshot qualified 312
nearest-enclosure baskets from 636 BTC markets. The frozen budget selected 20
baskets / 50 markets: eight lacked required depth; twelve had no positive margin
over whole quantities 1–100 after fees. All 20 matched an independent exhaustive
rational oracle and native offline replay. This single snapshot does not measure
episode duration or reject the remaining 292 candidates. The initial preparation
took 8.437 seconds; its HTTP reuse defect was subsequently fixed. Detailed data,
provenance and limits are retained in the
[acceptance record](research/results/20260917-basket-screen/README.md).
All 44 local CTest entries pass, including the new screen and preparation checks;
the native basket checks also pass AddressSanitizer/UndefinedBehaviorSanitizer.

Source/code baseline: local `ef63210` (`feat/economic-market-selection`). This is
not a new remote merge claim. The completed two-hour economic session observed
18 BTC/ETH markets, 108 implications, 455,436 book updates and 448 public trades.
It generated no paper orders. Independent reconstruction found no positive gross
guaranteed-floor margin even after relaxing freshness and whole-contract depth.
The configured selection budgets did not bind. These are two underlying events,
not millions of independent observations; both were hours from resolution.

**Current workstream: certify and observe a small BTC range/threshold three-leg
family, before implementing another execution policy.** Public preflight and
conditional arithmetic and step 3's tools are delivered. The first attempted
window provides partial point evidence only; the current passive feasibility
subgate above now precedes another operator run. This is a new payoff
relationship, not merely more of the existing nested pairs. Research rationale,
primary sources and the failed fresh-price example are in
[ECONOMIC_STRATEGY_RESEARCH.md](ECONOMIC_STRATEGY_RESEARCH.md).
Do not run another equivalent two-hour baseline solely because preparation or
replay is faster. No real/demo order submission is authorized.

| Order | Deliverable | Acceptance / stop condition |
|---|---|---|
| 1 | Scoped BTC range/threshold rule certificate. For lower threshold A, upper B and contained interval C, prove `YES(A)+NO(B)+NO(C) >= $2`. | Exact common scalar/source/window, boundaries, terms hashes and exception table. Normal settlement and stated all-NO fallback have a mathematical proof; operative Rulebook/review exceptions and production metadata certification remain pending. Unresolved exceptions block guaranteed-floor classification. |
| 2 | Bounded read-only economic screen, locally delivered on `feat/structural-basket-screen`. | Freeze an eligible cohort of up to 10–20 baskets before observing its outcome. Walk actual depth and enumerate whole sizes 1–100 within the existing fictional funding limit, using exact fee/rounding logic and an independent arithmetic oracle. Real-data acceptance matches the oracle; no positive quote in this snapshot. These are declared experiment budgets, not economically optimal settings. No general MILP platform or order adapter is required. |
| 3 | Prospective observation with a baseline cohort and declared quiet/active/near-resolution windows over several expiries. | Use existing recording/controller tools; record all constituent books, skew, ages and exact costs. Count distinct positive-margin episodes, shared liquidity and censoring, not repeated updates. Keep later whole expiries/days unused for confirmation. No fixed two-hour duration establishes economic sufficiency. |
| 4 | Economic decision before a basket executor. | If even optimistic executable-depth/fee margins never exceed zero, stop this family for the sampled scope and report exclusions. If independent positive episodes exist, test their duration, separate leg delays and incomplete acquisition using a bounded offline oracle; only then extend the shared production decision/execution path. |

The retained catalog's illustrative basket cost $1.97 against a conditional $2
floor. Cent-rounded fees remove all q=1 margin; larger quantities can amortize
rounding but had no synchronized depth evidence. Fresh public books cost $2.03
before fees. Thus there is **a concrete untested family, not a discovered
executable profit**. Full gross funding remains required; no cross-event collateral
offset is assumed.

**Passive comparator:** the offline static best-bid-join diagnostic is research,
not a delivered fill simulator. Both possible passive legs were checked on the
existing sample at q=1, zero maker fee and a contemporaneous hedge price level
holding at least one whole contract. The diagnostic does not aggregate fractional
depth across levels and does not exhaust all q=1 depth walks. Positive gross room
occurred only for one BTC relationship, at one cent; the taker fee removed it
under that convention. Larger quantities are not ruled out. The next
passive gate is a bounded size frontier plus trade/queue support and conditional
delayed hedge cost. Do not build a complex maker controller from this result.
Anonymous cancellation placement means fewer hypothetical fills are not a
mathematical lower P&L bound. No exact own-fill calibration has been obtained.

**Engineering within this block:** separate known-family readiness from the global
census; reuse HTTP connections and bound independent requests behind one limiter
and archive owner. Cursor chains remain sequential; bulk books already exist.
Fix task-owned response provenance before adding concurrency (`requests[-1]` is
not safe response identification). Compare identical archived inputs and report
fresh-subscription time, memory and selection age. Keep the incremental C++ path;
profile burst-driven tail delay before adding low-level machinery. Attribute an
economic improvement only when observed candidate value survives the reduced
delay, not from CPU speed alone.

This decision selects a bounded entry investigation for the existing basket
extension. It does not schedule general market making, a directional LLM trader,
cross-venue execution or joint allocation. Each would require its own edge
hypothesis. Earlier implementation/status entries below remain historical;
this section controls the immediate priority.

## Verified starting point (historical baseline)

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

**P1 and P2's transport/controller implementation are merged.** P2 representative collection remains open; short authenticated acceptance
now passes, including eight markets after the shared-subscription correction. P3's first offline acquisition-to-settlement
loop is delivered in [PR #11](https://github.com/DamianAcri/event-market-execution-engine/pull/11).
Use the economic discovery preflight below before starting another long P2 collection; do not repeat the fixed sample solely for a speed improvement. P3 can
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

**Current decision, 2026-09-17 — locally implemented and tested:** the two-hour capture
`btc-20260916T183127.659551Z` contains 1,947,001 updates, eight BTC markets in one
event and 28 implications. Replay and an independent reconstruction found zero
positive gross opportunities in those pairs, including relaxed freshness and
fractional sizing checks. No paper orders were generated. This identifies a
coverage question; it does not establish returns for other events or policies.

The previous local delivery, `feat/research-market-coverage`, extended the existing
capture/replay and aggressive paper path:

1. Add a frozen `research` selection: up to two BTC events with 16 evenly spaced
   strike ranks per event, including tails, plus up to 16 NFL observation markets.
   This is a bounded sample of up to 48 markets, not an economic optimum. Keep
   BTC implications within the same reviewed event and resolution rules.
2. Archive paginated public inputs and their hashes; write `coverage.json` with
   selected/excluded markets and certified versus observation-only coverage.
   NFL winner/spread terms allow fractional and discretionary fair-price payouts.
   A tie alone does not break the proposed payoff floor, but the Boolean oracle
   cannot represent it and no joint guarantee covers the exceptional payouts.
   NFL therefore contributes observations, not certified paper trades.
3. Record public trades on a separate subscription and replay them as observations,
   preserving direction, price, quantity and block status. They neither mutate
   books nor create simulated fills. Validate sequence handling and interleaving,
   legacy replay compatibility, reconnects and live/replay accounting agreement.
4. Precompile immutable dependency and leg ordering once. Require equivalent
   decisions and measured performance before reporting an optimization result.

Local Release validation passes all 35 CTest entries (including 23 TLS scenarios).
Focused feed/study/paper/residual tests pass ASan and UBSan; leak detection is not
supported by this macOS sanitizer runtime. The old two-hour study is byte-identical,
and the reproducible benchmark shows a 6.50% median study-time reduction on this
host; see [PERFORMANCE.md](PERFORMANCE.md). A 30-second authenticated read-only
preflight finalized 48 initial books on one connection with identical paper/replay
accounting. It received no deltas or public trades, so real trade-stream sequencing
and representative economic observations remain unverified. No remote CI or merge
claim is made by this local validation.
The research-to-decision mapping and primary sources are in the dated update to
[ECONOMIC_STRATEGY_RESEARCH.md](ECONOMIC_STRATEGY_RESEARCH.md).

**Current implementation — economic discovery:** `feat/economic-market-selection`
adds `--profile economic` before the next long capture. It traverses the open
non-MVE catalog with explicit page/byte limits and refuses incomplete discovery.
It refreshes reviewed series after the broad scan, verifies BTC/ETH threshold
semantics and event fee overrides/scheduled changes, acquires depth in batches,
and reuses the native funded sizing/fee ledger. Public non-block trades establish
recent activity; book refresh precedes the final frozen watchlist. The public
research frontier shows active excluded families and why rules still need review.

The selection policy prefers positive indicative net margins, then active pairs
nearest zero after one-contract costs, with deterministic subscription budgets.
This is an auditable observation policy, **not an estimated-profit optimizer**.
The 15-minute activity window, 64 subscription cap, 100-contract sizing cap and
USD 1,000 fictional funding are disclosed operational/scenario choices, not paper
recommendations or inferred optima. Zero eligible pairs produces a no-run report.
All inputs, exclusions, fees, selection code and executable are archived/hashed.

Acceptance before the next independent two-hour run: public discovery must
complete, rule/fee/depth validation must pass, and the report must justify an
active watchlist (or decline the run). Replay policy tests and native cost tests
must pass. Do not tune on that future run and relabel it a holdout; reserve later
whole events/dates for confirmation. Dynamic rotation and a calibrated model of
expected profit remain further work, as do passive execution and joint allocation.

Local acceptance completed on public data: the complete non-MVE catalog had
127,876 markets; 220 refreshed contracts passed semantics and fees. All 220 books
were examined; 62 markets had recent trades queried. The frozen result contained
20 markets / 69 certified relationships, with 48 non-block trades observed on
those markets in the preceding 15 minutes. No positive indicative margin was
found. Of the initially depth-qualified pairs, 389 failed two-leg recent activity.
All 39 local Release CTests pass, including the local TLS transport fixtures;
the new native screen passes ASan/UBSan, and the gateway-disabled build passes
11 CTests. This validated discovery and selection plumbing before the prospective
session described at the top of this plan; profitability remains unproven. Evidence is in
[public-preparation.json](benchmarks/results/20260917-market-selection/public-preparation.json).

**Earlier proposed gate, now secondary to the basket screen above:** after
capture/replay acceptance and sufficient usable trade/book observations, define
one bounded passive-versus-aggressive experiment in P3.
Specify arrival latency, queue ahead, uncertain cancellations, block exclusion,
fees, adverse selection and residual exposure; avoid counting trade/delta volume
twice. Aggregate depth and public trades cannot identify our exact hypothetical
queue position. Freeze assumptions and limits before using reserved later whole
events/dates; run sensitivity checks and disclose exclusions. The two-hour session
already inspected is exploratory data, not a holdout. Passive fills, optimal
universe selection and joint capital allocation are not delivered by this branch.

**Status, 2026-09-15:** optional authenticated read-only TLS/WS transport,
strict one-market subscription controller, reconnect/recovery, bounded background
recording and manifest-bound controller replay are merged as
[PR #10](https://github.com/DamianAcri/event-market-execution-engine/pull/10), `ae94071`.
Portability/sanitizer CI and TLS fixtures on Linux, macOS and Windows pass.
The fixtures verify signing, peer validation,
control/data history and recovery; the decoder avoids redundant per-frame parsing.
**Update, 2026-09-16:** real multi-market acceptance exposed same-channel
subscription merging and omitted empty snapshot sides. The corrected controller
uses an explicit shared subscription and stream sequence, with versioned replay.
A 45-second/eight-market capture passed with 2,613 updates and no rejected replay
updates; two legacy captures retain identical transcripts. The operator runner
now prepares reviewed metadata and records bounded sessions without orders.
**P2 remains data-dependent, not complete:** the representative multi-event/date
campaign and economic calibration still require collected observations.
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

**Status, 2026-09-16:** the first acquisition-to-settlement simulation is merged as
[PR #11](https://github.com/DamianAcri/event-market-execution-engine/pull/11), `898a7a4`.
Schema 3 compares parallel acquisition
with bounded sequential completion, explicit leg order, delayed responses,
partial/failed fills, conservative EOF reservations, exact settlement cash and
paid capital holding time. Independent synthetic ledgers validate accounting;
legacy policy output is preserved. See [LIFECYCLE_STUDY.md](LIFECYCLE_STUDY.md).
Schema 4 adds `hold` versus one bounded sale of confirmed unmatched holdings on
`feat/residual-position-exits`: shared book depletion across buy/sell directions,
exact exit fees, FIFO ownership/cost basis, delayed availability of sale proceeds,
and partial/failed/unknown exits. It preserves the matched portfolio and cannot
sell unowned contracts. No live submission is added.
This advances P3; it does not close all acceptance. Representative observations,
response/fill calibration, external cancellation races and
operational restart/reconciliation remain pending. No actual net returns have
been measured, and no production policy is selected from synthetic examples.

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

**Implemented paper adapter (2026-09-16):** optional live simulation and recording
reuse the existing sizing/lifecycle/residual-exit observer. Pending simulated
orders advance on recorded local clock events during quiet feeds; sparse output
is bounded and asynchronous. The finalizer compares economic events and final
accounting with replay before publishing `paper-summary.json`. The operator owns
long runs; no order submission or account endpoints are added. See
[LIVE_PAPER.md](LIVE_PAPER.md). This is an initial fixed simulation scenario,
not a calibrated production policy. Representative economic evidence, real fill
calibration and exchange order/restart/reconciliation remain open. Demo and real
orders remain outside current authorization.

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
| Research sequence appeared to require baskets and advanced models automatically. | P1 is complete. Extensions require the entry conditions above; the dated current decision selects only a bounded basket investigation. |
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
