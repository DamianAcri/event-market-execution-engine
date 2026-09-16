# Simulated execution lifecycle

P3 has a bounded offline acquisition-to-settlement loop and an optional residual
sale policy. Both use the same study engine and fee/sizing functions. P3 economic
acceptance remains open: parameters need suitable observations; external cancel
races, persisted operational reconciliation and restart recovery remain pending.
P4 remains subsequent work. No live order adapter is included.

## Policy and causal timeline

Policy schema 3, strategy `execution_lifecycle_v3`, retains schema 2's exact funded
sizing and adds a required `lifecycle` object. Existing schema 1/2 policies remain
supported and preserve their transcript. The strict parser lives separately in
`src/session/study_policy.cpp`; replay, fills and accounting share one simulator.

| Field | Contract |
|---|---|
| `execution_policy` | `parallel_hold` submits both legs; `sequential_complete` waits for the first response before completing. |
| `first_leg` | `0` starts with the lower market ID, `1` with the higher; latency/reject arrays index this chosen execution order. This is explicit ordering, not a trained fill predictor. |
| `response_latency_ns` | Two nonnegative response delays after simulated IOC arrival. Independent of the existing decision-to-arrival delays. |
| `completion_timeout_ns` | Maximum elapsed time from initial decision through a new completion order's arrival. |
| `completion_loss_limit_micro_usd` | Largest prospective total acquisition cost minus paired payout floor allowed for a new completion attempt. Not a guarantee against an already acquired leg losing money. |
| `maximum_completion_orders` | 1..4 attempts on the second leg; the first acquisition order is additional. |
| `settlements` | Scenario records `{market_id, yes_wins, time_ns, provenance}`; missing resolutions leave held positions incomplete. |

Settlement timestamps use the same monotonic nanosecond clock as the replay.
They are not Unix timestamps; observed resolution times require an explicit clock
mapping before creating a policy.

Initial sizing still maximizes fully acquired margin at existing funding/quantity
limits. It does not maximize expected execution profit. Initial acquisition reserves
both legs in either policy. Partial first-leg fills determine the sequential target;
a rejected/empty first leg does not trigger an unnecessary second-leg purchase.
Each completion attempt rechecks currently visible depth, freshness, connection,
available funds, elapsed time, count and the prospective loss bound. It may acquire
a smaller available quantity when that improves the paired bound within the budget.
If completion is unavailable or disallowed, the residual is explicitly held through
settlement. The model never invents an exit or an automatic hedge.

Each order has an intent, separate arrival and response. IOC fills charge the
existing exact ledger, consume shared liquidity and cancel unfilled quantity at the
simulated venue; cash remains reserved until the response. Completion decisions
use fills only after that order's response. Market observations at a timestamp
precede simulated arrivals/responses at that timestamp. Settlement at a timestamp
is terminal before a new decision or arrival at the same time. Unrelated simultaneous
order events have a stable order by order ID. These are explicit tie assumptions,
not measured exchange ordering.

The event scheduler uses a min-heap: no scan of all pending orders per market
update, O(log N) insertion/removal for due events. The one-attempt-per-constraint
rule and fixed retry count bound event, order and lot storage; capacity is allocated
before replay starts. Pure market decoding and capture retain their own measured
path. The simulator still emits detailed JSON for offline analysis.

## Accounting and incomplete observations

Schema 3 checks after every relevant transition:

```text
available + reserved - executed_debit_awaiting_response
    = initial_capital - acquisition_debit + settlement_cash
```

This separates money actually spent from money the decision process may reuse.
End of observation does not turn an unobserved order or response into a cancellation.
Pending reservations remain and `accounting_complete` is false. Future settlement
annotations may account for already simulated fills; they never manufacture fills
or acknowledgements beyond the observation horizon.

Settlement outcomes are validated against the reviewed relationships and accessed
by the execution loop only when their declared time is reached. Changing future
labels preserves the preceding order/fill/response transcript. Cash payouts close
actual acquired lots. When all lots settle and all orders reconcile, the report
includes `simulated_net_pnl_micro_usd`, after operating cost once. It remains null
for incomplete accounting; `realized_pnl_micro_usd` remains null for every simulation.
The original paired-floor bound and acquisition counts are retained separately;
`unbalanced_pairs` describes acquisition history, not an open position after settlement.

Paid acquisition capital holding is accumulated exactly in microUSD-seconds plus
a base-1e9 fractional remainder. Each lot contributes its debit times the interval
from fill to settlement (or EOF for an unresolved lot). This excludes unspent order
reservations and does not silently apply an assumed financing APR. Aggregate
arithmetic overflow fails the study. Unknown orders or mixed censored/resolved lots
must not be compared as complete realized capital efficiency.

## Reproduction and evidence

Generate the existing synthetic fixture, then run the supplied schema-3 policy:

```sh
./build/eme_study_tests --fixture /tmp/eme-lifecycle-example
./build/event-engine session study /tmp/eme-lifecycle-example/session-0 \
  /tmp/eme-lifecycle-example/session-0/replay.json examples/policy.lifecycle.synthetic.json
```

This fixture buys NO A at 0.30 and YES B at 0.60/0.65 before B's cheap depth
vanishes. At five contracts, full acquisition costs $4.8049 including fees, against
a $5 paired floor. The following outcomes are **synthetic regression scenarios**,
with arbitrary nanosecond fixture clocks, not market observations or income estimates:

| Scenario | Simulated net result before external operating cost |
|---|---:|
| Both policies complete before depth changes; A/B settle YES | +$0.1951 |
| First acquisition rejected, other leg settles worthless; parallel | -$3.2314 |
| Same rejected first leg; sequential skips the second | $0 |
| Slow first response, zero-loss completion budget, first leg settles worthless | -$1.5735 |
| Same slow response, $1 prospective loss budget allows remaining partial depth | -$0.5713 |
| Responses remain outside observation | Incomplete; no final PnL |

These cases demonstrate both the benefit and cost of waiting; they do not select a
production winner. Tests include 180 independently calculated single-level ledgers
across prices, quantity caps, fee coefficients/quanta, partial/rejected fills and
valid settlement worlds. Additional cases cover delayed responses, EOF, missing or
contradictory labels, first-leg selection, completion budgets, operating cost,
limited capital, deterministic replay and no future-label influence. Existing
multi-level and independent core fee/sizing tests remain in place.

[PERFORMANCE.md](PERFORMANCE.md) contains complete-study timing and preservation
of the previous policy's output. [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md)
remains the sole plan: collect/calibrate the observations, compare frozen policies
under equal limits, then address broader exit and operational behavior where the
results justify them. Same-price fill fragmentation, impact, queue behavior and
response latency are declared assumptions, not calibrated venue behavior.

## Schema 4: one bounded exit of unmatched holdings

`residual_exit_v4` retains all schema-3 lifecycle fields and requires a root
`residual_exit` object. Schema 1/2/3 retain their exact transcripts. Use
`examples/policy.residual.synthetic.json` with the same fixture above.

| Field | Contract |
|---|---|
| `mode` | `hold` or `reduce_once`; at most one exit order per attempt. |
| `arrival_latency_ns` / `response_latency_ns` | Separate delays for the exit order and its reply. |
| `timeout_ns` | Maximum initial-decision-to-exit-arrival time. |
| `minimum_price_1e4` | Minimum eligible sale price, in the held outcome's price scale. |
| `reject` / `available_liquidity_bps` | Explicit sale rejection/liquidity stress assumptions. |

Exit decisions wait for **every acquisition response** for that attempt. In
sequential mode they follow completion or its declared stopping condition. The
policy can sell only the excess quantity of one acquired leg over the other,
using that attempt's still-owned lots. It preserves the matched portfolio. No
additional buy or sale can race another exit from the same attempt.

The quote uses current depth, account fees, the quantity grid, freshness and
connection generation. It selects visible unmatched quantity at/above the price
floor, and submits an IOC limit at the worst included quote price. Arrival checks
the book and ownership again; deterioration can yield a partial or zero fill.
No retry or fictitious hedge occurs. Rejection, missing depth, stale books,
resolution or exhausted time can leave exposure held through settlement.

Selling YES consumes the same physical bids as buying NO; selling NO consumes
the same asks as buying YES. Both directions share the existing depletion keys.
An earlier simulated acquisition cannot leave its consumed liquidity available
for a later exit. This mapping follows the venue's
[direction and book-pricing contract](https://docs.kalshi.com/getting_started/order_direction)
(checked 2026-09-16).

Sale fees reuse the buy fee model and per-order rounding accumulator. Positive
revenue less the microdollar-ceiled fee is floored to the declared account grid;
rebates remain capped by that fill's fee. This applies Kalshi's
[signed-revenue rounding contract](https://docs.kalshi.com/getting_started/fee_rounding)
(checked 2026-09-16). Fee coefficients remain explicit scenario assumptions.
Each visible price level is one assumed fill; fragmentation is still uncalibrated.

Positions are stored once, with preallocated per-attempt/per-leg FIFO lot links.
Each acquisition order creates one cost lot, aggregating its price-level fills;
FIFO ordering is between acquisition orders. Exits traverse only their own lots.
Cost basis is allocated proportionally in
integer microdollars; retained quantity keeps the rounding remainder. Full-lot
closure avoids division. Sold quantity stops accruing paid capital holding time
at arrival; retained quantity continues until settlement or the observation end.
An outstanding response still prevents a complete accounting claim. The metric
does not measure extra waiting time before confirmed proceeds become reusable.

The cash invariant now includes both sale proceeds and unknown sale responses:

```text
available + reserved - unconfirmed_buy_debit + unconfirmed_sale_credit
    = initial_capital - acquisition_debit + sale_credit + settlement_cash
```

Proceeds become available only on response. EOF never manufactures a response or
an exit beyond observation. Settlement wins equal-time order arrivals and pays
only the retained quantity. Fully exited lots need no settlement annotation.
`residual_exit` reports sold quantity, net proceeds, fees, released cost basis and
proceeds awaiting acknowledgement. Top-level fees include buys and sells; the
paired-floor bound includes actual simulated sale proceeds. Acquisition counts
remain historical counts, not a current inventory report. Final simulated PnL
requires all orders reconciled and all retained lots settled, after operating cost.

Synthetic example: reject the first leg, acquire five YES B contracts for
$3.2314, then sell them at $0.55 with $0.0867 exit fees. Net proceeds are $2.6633,
so the result is **-$0.5681**. Holding produces **-$3.2314** if B loses, or
**+$1.7686** if B wins. The exit decision is identical under either future label:
it reduces exposure but can sacrifice the eventual winning payoff. No production
winner or profitability conclusion follows. Other fixtures cover partial sales,
matched portfolio preservation, vanished/shared depth, stale books, delayed
responses, unknown EOF events, settlement races and independent generated ledgers.

This is an incremental comparison policy, not a new strategy framework: hold and
bounded reduction share one event scheduler and accounting implementation. The
trade-off is deliberately one exit attempt, without adaptive re-entry, prediction
or external cancellation handling. Observations must justify broader policies.
