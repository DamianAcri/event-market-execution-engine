# Structured replay and offline execution studies

The CLI now connects verified sessions, explicit controller history, gross
candidate events, costed depth and a deterministic aggressive IOC simulator.
It has no order transport or credentials. The replay and simulator use the same
decoder, market state, constraint metadata and fixed-point types as the engine.

The legacy policy remains reproducible. Policy schema 2 adds exact, bounded
net-profit sizing for the same two-leg portfolios; see [NET_SIZING.md](NET_SIZING.md)
for its required budget, fee-bound proof and explicit incomplete-search status.

```sh
event-engine session import metadata.json capture.json new-session
event-engine session replay new-session new-session/replay.json > replay.jsonl
event-engine session study new-session new-session/replay.json policy.json > study.jsonl
```

`import` is optional: an existing session can be paired with an explicit replay
plan. It must not be reconstructed by silently assuming connections/recoveries.
Keep inputs immutable during use. Import creates a new directory and leaves failed
artifacts for inspection. Session integrity and replay validity are separate: a
session manifest may be complete even if importing its controller plan fails.
Re-run `session replay` to establish semantic validity.

## Replay plan, version 1

Session manifest v1 and journal v2 remain unchanged. `replay.json` binds the exact
manifest bytes, which bind the metadata and journal. Reports bind the exact plan
and policy with SHA-256 too. These are reproducibility fingerprints, not signatures
or proof that a source declaration is authentic.

```json
{
  "schema_version": 1,
  "source_kind": "synthetic",
  "provenance": "Synthetic fixture, not observed trading data",
  "manifest_sha256": "<64 lowercase hex characters from manifest.json>",
  "use_yes_price": true,
  "controls": [
    {"before_record": 0, "time_ns": 0, "action": "open", "target": 1},
    {"before_record": 5, "time_ns": 3500, "action": "recover", "target": 2},
    {"before_record": 6, "time_ns": 4000, "action": "close", "target": 1}
  ]
}
```

`before_record` is a zero-based raw-record ordinal; the record count denotes EOF.
Control array order breaks ties. `target` is a generation for open/close and market
ID for recover. An initial open before record zero is required. Times are
nonnegative nanoseconds in the same monotonic clock domain as `received_at`.
No clock regression, automatic generation change or implicit recovery is allowed.
Source kinds are `synthetic`, `observed_ws`, and `observed_rest_samples`.

The strict parser rejects duplicate/unknown fields, unsupported versions, excessive
depth, invalid references, oversized files and manifest mismatches. Malformed
payloads or generation/metadata mismatches abort replay. Book-level rejections
(including gaps) are emitted and refresh candidate validity; a later snapshot
cannot silently recover a stale book. The controller must explicitly recover it.

Output is JSONL: `replay_start`, ordered `control`/`market` records with causal
indices/times and candidate events, then `replay_complete`. Candidate reason codes
follow `InvalidationReason` in `candidate_tracker.hpp`. **Output is provisional
until completion and a successful exit code**. Study output additionally requires
`study_complete`. Check the process status when piping or redirecting output.

## Capture adapter envelope

The offline importer accepts up to 64 MiB/100,000 records. Its outer fields are
`schema_version`, `source_kind`, `provenance`, `use_yes_price`, `controls` and
`records`. The first five have the same meanings as the plan above; it computes
the manifest binding after packing the records. Each record is:

```json
{
  "generation": 1,
  "time_ns": 1000,
  "observed_at_ns": 1800000000000001000,
  "sequence": 1,
  "payload": {
    "type": "orderbook_snapshot", "sid": 1, "seq": 1,
    "msg": {"market_ticker": "A", "yes_dollars_fp": [["0.7000", "5.00"]],
            "no_dollars_fp": [["0.7500", "5.00"]]}
  }
}
```

Payload objects are re-serialized, so this importer does not preserve original
network bytes. Archive raw source responses and their request times/hashes beside
the study. For byte-exact WS capture, use the raw journal writer directly.

**Price convention:** under `use_yes_price: true`, wire NO-side prices are already
YES asks. REST returns NO bids in NO prices: its adapter must convert `1 - no_bid`
exactly once, label the result `observed_rest_samples`, and retain the original
response. Locally assigned REST sequence numbers are ordering labels, not exchange
sequences. HTTP completion times and asynchronous snapshots do not establish quote
age at the exchange or the state between samples.

## Fixed simulation policy, version 1

All fields are mandatory; extra fields, missing market fees and implicit defaults
are rejected. Prices/quantities/cash use exact integers. Policies are read once per
study; fee configuration never runs inside a market decoder.

```json
{
  "schema_version": 1,
  "strategy": "one_attempt_per_constraint_v1",
  "fee_provenance": "Hypothetical general taker coefficient; direct-member precision",
  "capital_micro_usd": 100000000,
  "operating_cost_micro_usd": 0,
  "quantity_cap_centicontracts": 500,
  "quantity_step_centicontracts": 100,
  "min_margin_micro_usd": 0,
  "max_book_age_ns": 1000000000,
  "leg_latency_ns": [1000000, 1000000],
  "reject_legs": [false, false],
  "available_liquidity_bps": 10000,
  "fees": [
    {"market_id": 1, "coefficient_ppm": 70000, "balance_quantum_micro": 100},
    {"market_id": 2, "coefficient_ppm": 70000, "balance_quantum_micro": 100}
  ]
}
```

- `1 USD = 1,000,000` cash units; `1 contract = 100` quantity units. This is USD,
  not an implicit EUR conversion. Budgets are capped at $1 billion, per-attempt
  quantity at 1 million contracts. Supported quantity steps: 1 or 100 raw units.
- An applied update evaluates only its dependent constraints, ordered by ID.
  Each constraint may submit once per run. Legs are ordered by market ID;
  latency/reject settings refer to that order, not YES/NO order.
- Walk asks to buy YES, bids to buy NO at `1 - bid`, best price first, without
  copying book depth. Floor each level's available quantity to the policy grid.
  Choose the largest common funded size up to the cap, then require a positive
  costed margin strictly above `min_margin`. This first policy **does not search
  all smaller quantities for maximum profit** and can miss a smaller profitable
  trade. It is a reference policy, not an optimal allocator.
- Reserve both legs before queuing orders. Each reservation includes limit-price
  notional, peak curvature fee and worst-case per-grid-fill rounding. Partial
  fills must fit it; release the unused reservation after IOC completion.
  No collateral credit or premature reuse of settlement proceeds is assumed.
- Pending arrivals are processed against earlier state. At equal timestamps,
  observations precede simulated fills. All depth at a leg's arrival is traversed
  atomically; there is no passive queue model. Book validity, receive-age limit,
  generation, limit price, partial liquidity and forced rejects apply at arrival.
  Latency is decision-to-arrival only; response/ack latency is not modeled.
- A depletion ledger shared across constraints subtracts simulated consumption
  by market/outcome/price. It persists across later snapshots and generations,
  so displayed liquidity cannot repeatedly manufacture profit. This can understate
  real replenishment and does not model counterfactual market impact.
- One simulated fill represents each consumed price level. Actual same-price
  maker fragmentation is unknown and can change rounding; validate against real
  fills before treating the fee result as venue-calibrated execution.
- Orders arriving beyond the observation horizon are reported as unobserved and
  left unfilled. Their reservations are released for reporting. Actual fills
  remain as held positions; there is no forced exit or invented settlement.

Fee calculation uses the declared quadratic coefficient including any multiplier.
Trade fees are ceiled to one microdollar, balances rounded using the declared
account quantum, and per-order rounding credits rebated only when that fill's
net fee remains nonnegative. Accumulators can carry between coefficients for
maker/taker transitions, although this simulator submits aggressive orders only.
Current sources: [fee rounding](https://docs.kalshi.com/getting_started/fee_rounding),
[July 7, 2026 fee schedule](https://kalshi.com/docs/kalshi-fee-schedule.pdf).
Profile precision is 100 microdollars for direct members or 10,000 for non-direct
members. Other broker/funding charges must be declared separately; a general
coefficient is not automatically valid for every product or date.

`study_complete` reports attempts, completed/unbalanced pairs, unobserved orders,
decline counts, available cash, notional, net fees, spent cash and a settlement
lower bound. For each two-leg portfolio its payout floor is the smaller filled
quantity times $1. Unmatched quantities contribute zero to this bound. Subtract
actual simulated debits and declared operating overhead. Correlated portfolios'
individual floors remain a conservative sum; liquidity is shared before fills.
`realized_pnl_micro_usd` is always null. Positive synthetic output is explicitly
`synthetic_validation_only`; observed replay also cannot prove profitability.

## Verification and first observed pilot

Tests cover independent fee examples, a generated price/quantity grid, portable
large-number arithmetic, depth, funding, partial/rejected legs, stale data,
equal-time arrival, EOF, shared liquidity, gaps/recovery, hash bindings and strict
parsing. CLI tests exercise import → replay → study and invalid-policy exit codes.
The generated fixture can be reproduced without credentials:

```sh
build/eme_study_tests --fixture /tmp/new-study-fixture
build/event-engine session study /tmp/new-study-fixture/session-0 \
  /tmp/new-study-fixture/session-0/replay.json /tmp/new-study-fixture/policy.json
```

On the fixture, five paired contracts cost $4.8049 including $0.1549 in fees and
have a $5 settlement floor: simulated bound **+$0.1951**. Delaying arrival until
one book's quotes disappear leaves only one leg costing $1.5735, with a zero
guaranteed payout: bound **−$1.5735**. These are regression examples, not returns.

On 2026-09-14 a local, read-only public REST pilot sampled 8 KXBTCD contracts for
`KXBTCD-26SEP1517`, 12 rounds/96 books over 74.998111 seconds. The 28 reviewed
implications compare higher/lower thresholds for the same BRTI average, time and
rules. Both contracts resolving NO when data is missing preserves implication;
see [BTC contract terms](https://assets.kalshi.com/contract_terms/BTC.pdf).
The public series response declared quadratic fees with multiplier 1.

There were **zero gross candidate events** and **zero simulated trades**. Each
run evaluated 672 dependency-triggered decisions: 28 lacked a fresh second book
during startup and 644 had no positive costed margin. The same result held for
USD budgets 100/1,000/10,000 and symmetric delays 0/1/10/100 ms (12 scenarios).
No operating cost was assumed in this short pipeline pilot; zero trading PnL
does not mean an operating business breaks even.

Raw captures, policies, twelve output traces, response hashes and the sampling
script remain in local research artifacts, outside Git. This is **inconclusive
economic evidence**: one event, around 75 seconds, asynchronous REST samples,
no exchange fills and no held-out calibration. It proves neither profitability
nor the absence of opportunities in other periods. Public REST requires no keys;
[authenticated WS data](https://docs.kalshi.com/getting_started/quick_start_market_data)
is needed to progress toward representative timing/sequence observations.

Work order is maintained in [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md):
P1 improves sizing, P2 owns observed capture/coverage, and P3 extends this simulator
through response, residual-position and settlement accounting. Netting, passive
queues and other models are conditional extensions, not prerequisites to P1.
The implemented policy and format limitations described above remain unchanged.
