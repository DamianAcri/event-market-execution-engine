# Exact net-profit sizing

P1 implements `eme::core::size_buy_pair` for two independent buy legs whose
reviewed equal-quantity portfolio pays at least one dollar per contract. The
caller owns semantic verification, freshness and available unconsumed depth.
The function returns the best **fully acquired** net margin on the declared
quantity grid, after exact fees and account rounding, subject to conservative
funding. It does not estimate joint-fill probability or future prices.

## Contract

Buy prices are ascending, unique and expressed in the purchased outcome. The
study converts NO acquisition from YES bids once before calling the core. The
depth of each level is rounded down to the quantity grid. Supported grids are
one centicontract or one whole contract; maximum cap is 100 million
centicontracts and at most 10,001 distinct prices per leg. Fee coefficient is at
most 1.0 and account quantum is 100 or 10,000 microdollars, as in the fee ledger.

No quote changes cash, book depth or the fee ledger. A successful result contains
quantity, per-leg limit, notional, debit and reservation, paired payout floor and
net margin. Profit must strictly exceed the configured minimum; not trading is
always available. Ties choose lower reservation, then smaller quantity.

`search_budget_exceeded` returns **no quote**, even if a profitable candidate was
found before the search stopped. It must remain distinguishable from a completed
search with no profitable size. The evaluation budget limits candidate quantities,
not elapsed nanoseconds; depth validation and funding bounds also take work.

## Algorithm and bound

Two cursors merge the ordered liquidity breakpoints. Each caches quantity,
notional, actual debit and the per-order rounding accumulator for completed
levels. A candidate inside the current interval charges only the partial current
level on each leg, using a copy of that accumulator. Each completed level is
charged once when its cursor advances. Funding is monotone, so binary search may
trim an interval at the last funded grid point. Profit itself is not assumed
monotone.

The ledger identity for an order is:

`total debit = total notional + sum(ceiled trade fees) + final rounding accumulator`.

Completed levels are constants inside a price interval. Replacing the two active
partial-fill fee ceilings with their exact fractional values gives a linear
continuous margin which bounds actual integer margin from above: the remaining
rounding accumulator is nonnegative. Its slope identifies the endpoint with the
largest bound. Exact modular remainders give the floor of that endpoint's
continuous margin without floating point or compiler-specific wide integers.
Intervals whose bound cannot beat the best result, including the tie rule, are
pruned; otherwise the quantity interval is bisected. Exhausting the explicit
budget fails closed instead of returning an approximate optimum.

This bound is derived from this engine's fee contract; it is not a proprietary
firm technique or a claim that a cited paper proves this implementation. The
research principles applied are incremental computation, bounded work, data reuse
and comparing optimized decisions with a reference under the same economics.

The solver allocates no heap storage and uses logarithmically bounded recursion
(fewer than 28 quantity bisections at the supported cap). Study depth preparation
uses two reusable contiguous buffers and preserves shared depletion. There is no
native CPU tuning, SIMD, extra thread or external dependency in sizing.

## Versioned study policy

The existing `schema_version: 1`, `one_attempt_per_constraint_v1` policy retains
its largest-funded-size behavior and byte-identical study output. To select the
new policy, retain its fields and change/add:

```json
{
  "schema_version": 2,
  "strategy": "one_attempt_net_profit_v2",
  "max_sizing_evaluations": 100000
}
```

The budget is required and ranges from 1 to 1,000,000. The full policy is hashed.
Version two adds aggregate `sizing` counters to `study_complete`; incomplete
searches are counted separately in `declined.sizing_search_budget_exceeded`.
Both policies still make at most one accepted attempt per constraint,
reserve both legs before submission, and use the same IOC arrival/fill path.
Actual rejection, partial fills, shared depletion and staleness can make a quoted
profitable portfolio lose money. Per-price-level fill grouping remains an
assumption; arbitrary venue fill fragmentation is not inferred from level data.

## Validation and economic result

The generated oracle tests rebuild every quantity's complete orders and calculate
fees, rounding, rebates and reservation with an independent integer formula.
6,000 seeded books exercise both grids, multiple levels, zero/large coefficients,
both balance quanta, dust and funding limits. Additional cases cover failed search,
invalid inputs, missing depth and the full 100-million-choice cap.

The documented fixture now selects **two contracts and $0.137 simulated net
settlement margin** when a five-contract cap made the legacy policy decline. The
end-to-end test verifies actual simulated fills and a separate one-leg-rejection
loss. This establishes a specific decision improvement, not market profitability.
Both policies find zero attempts on the existing short REST pilot; its coverage
remains insufficient for an economic conclusion. See [PERFORMANCE.md](PERFORMANCE.md)
for timings and [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) for remaining work.
