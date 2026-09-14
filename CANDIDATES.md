# Deterministic gross candidate tracking

`eme::opportunity::CandidateTracker` is an offline-capable core component. It
observes the existing normalized `MarketState` and emits opened, updated and
invalidated events for the guaranteed two-leg templates compiled from reviewed
implications and complements. It submits no orders and makes no net-profit claim.

## Calculation

For each compiled template, both books must be valid and the connection open.
BUY YES consumes the best YES ask. BUY NO consumes the best YES bid at `1 - bid`,
with the quantity available at that original bid. This follows the existing
normalized YES book representation. The common quantity is the smaller available
quantity across the two required top levels.

Kalshi wire input must follow the existing `use_yes_price: true` subscription
contract. [Kalshi's order-direction documentation](https://docs.kalshi.com/getting_started/order_direction)
distinguishes that unified scale from legacy NO-leg pricing. The decoder cannot
infer the subscription setting from an individual payload. The integration
fixture uses unified pricing explicitly; future capture/control history must
retain the subscription and normalization policy. Artifact integrity alone does
not establish the price convention of an imported journal.

At construction, the existing exhaustive payoff oracle verifies the one-contract
template and its one-dollar minimum payout. During updates, exact price/quantity
integer products give acquisition cost in the existing six-decimal cash units;
the quantity-to-settlement conversion bounds arithmetic before multiplication.
The candidate is active only when minimum payout minus acquisition cost is
strictly positive. Overflow invalidates it instead of clipping quantity silently.

Example, **synthetic**: A implies B; BUY YES(B) at 0.60 and BUY NO(A) at 0.30,
with 2.50 common contracts. Acquisition cost is 2.25, minimum payout is 2.50 and
gross margin is 0.25. Fees or incomplete acquisition can erase that margin.

Only the compiled templates are covered: implication BUY YES(consequent) plus
BUY NO(antecedent), and complement BUY YES(left) plus BUY YES(right). This does
not enumerate every direction or every possible portfolio.

## Identity and lifecycle

Identity is a structural tuple containing metadata version, constraint ID,
semantic version, acquisition direction and the two market/outcome legs sorted
by market ID. It has no hash collision and does not depend on registration order,
prices, quantities, connection generation or discovery time. Exact source metadata
is bound by the session manifest; a reused version number alone is not content
authentication.

Construction owns a compact projection of the registry and checks templates
outside the update path. Subsequent source-registry changes do not alter the
tracker. A metadata change requires a new tracker. Events are ordered by ascending
constraint ID within each refresh, including connection-wide invalidation.

| Prior state | Current result | Event |
|---|---|---|
| Inactive | Positive gross quote | `opened` with the quote |
| Active | Changed positive gross quote | `updated`, same identity |
| Active | Identical quote | No event |
| Active | Disconnected, unavailable book/side, nonpositive margin or overflow | `invalidated`, reason and no quote |
| Inactive | Still ineligible | No event |

A later reopening keeps the identity. For separate occurrence tracking, consumers
must also retain the ordered opening event position. A sequence advance that
leaves the economic quote unchanged does not emit another update.

## Caller contract and incremental behavior

After every market-state transition, call `refresh(changed_market, state)`, even
when an update was rejected and made the book stale. Connection or generation
changes detected at refresh trigger a full refresh automatically. Explicit
`refresh_all(state)` is available for initial scans and controller-wide changes.
Callers must not mutate multiple unrelated books while reporting only one change.
Refresh cannot discover omitted transitions or an incorrect changed-market ID.

The tracker must use the matching session metadata and one state history. It has
single-owner sequencing. The returned span refers to a reusable event buffer and
expires at the next refresh or destruction; copy events to retain a history.
The caller supplies causal record indices and times. There is no new persisted
event schema or general CLI replay in this change.

Startup builds canonical entries and market-to-entry indices. A normal refresh
visits only the changed market's dependencies; an unrelated market evaluates
nothing. `refresh_all` retains a full-scan reference for differential validation.
Entry and event storage are reserved at startup; space scales with definitions
and their dependencies. No JSON, SHA-256, world enumeration or registry copying
is added to each market update.

The existing book validity state is checked. A time-based quote-age policy,
multi-level depth, fees, capital limits, shared-liquidity allocation, execution
risk and collateral eligibility belong to the costed evaluator. Gross candidates
must not be summed into achievable portfolio profit.

## Decision and verification

This implementation follows the architecture guide's small, shared-core approach:
reuse normalized books and reviewed payoff compilation; introduce no new service,
parser or runtime dependency. A startup projection trades bounded additional
storage for stable ownership and less work during updates. Full scanning is the
correctness/performance baseline; the dependency index is retained only with
equivalent ordered output and measured benefit.

Tests include hand-calculated implication cash flow, an independent complement
settlement/cash grid with fractional quantities and endpoint prices, identity
invariance, overflow, stale/recovery/disconnect cases and 2,000 generated market
transitions compared with the full scan. A verified-session fixture passes raw
records through the existing decoder, normalizer and processor, then checks that
two repeated runs and the full scan produce identical lifecycle events. Its
controller actions are explicit test inputs; it does not establish that an
arbitrary journal contains recoverable live control history.

See [PERFORMANCE.md](PERFORMANCE.md) for measurements and
[ECONOMIC_VALIDATION.md](ECONOMIC_VALIDATION.md) for the evidence gates ahead.
