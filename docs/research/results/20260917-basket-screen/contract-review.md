# BTC range/threshold observation contract review — 17 September 2026

## Decision and limits

Admit a bounded **conditional common-scalar observation model** for KXBTCD
thresholds and KXBTC numeric intervals. Do not label the result unconditional
arbitrage or enable orders. The implemented model covers ordinary settlement and
the contract's common all-NO missing-data outcome; it does not establish a joint
payoff bound for administrative review, contract modification, or independent
discretionary fractional payouts. Current Rulebook applicability remains unresolved.

The exact classification emitted by `scripts/basket_families.py` is
`conditional_common_scalar_observation_only`; `observation_only=true` and
`unconditional_certificate=false` cannot be overridden by source metadata.

## Primary sources actually checked

**BTC terms:** [official two-page PDF](https://assets.kalshi.com/contract_terms/BTC.pdf),
read in full and downloaded again on 17 September. The sixty-second BRTI average
and CF Benchmarks source are shared; ranges include both endpoints. Missing data
has a stated NO outcome. The document provides a dollar settlement value and
also references outcome review. Its permitted cent-valued strikes do **not**
establish that the averaged scalar is cent-rounded. The proof below therefore
covers all real-valued scalar outcomes. SHA-256 of fresh raw bytes:
`e7d857369971e75e9db14c5e2d91c29b94eb9a06e83e2acd9777991c4f2a0e2f` (24,445 bytes).

**Public event records:** fresh unauthenticated GETs for
[KXBTCD-26SEP1817](https://external-api.kalshi.com/trade-api/v2/events/KXBTCD-26SEP1817)
and [KXBTC-26SEP1817](https://external-api.kalshi.com/trade-api/v2/events/KXBTC-26SEP1817)
confirmed matching `strike_date=2026-09-18T21:00:00Z`, matching source, and
respectively false/true exclusivity. There were 50 threshold contracts and 50
range-family contracts; all 50 thresholds and 48 interior intervals passed the
exact rule grammar. Two range-family tail contracts were intentionally excluded.
All qualifying records shared one settlement metadata tuple. This is a semantic
check, not a price or fill observation. Raw hashes are in `contract-source-manifest.json`.

**Rulebook exception scope:** the current [venue Rulebook page](https://kalshi.com/regulatory/rulebook)
returned HTTP 429 through the direct downloader. A search-indexed v1.24 PDF URL
returned 404. The [July 2025 CFTC filing](https://www.cftc.gov/filings/orgrules/rules07012525155.pdf)
was readable through the web reader, but is **historical**, with draft version/date
markers, and cannot certify the operative September 2026 rule set. Relevant
pp. 57–60 describe discretionary allocations in unspecified contingencies,
outcome review and changes to source, underlying or timing. Direct archival
download returned 403; no raw-byte hash is claimed. The BTC terms explicitly
address missing data, so that case is not automatically replaced by the generic
fallback. Nevertheless, no reviewed provision establishes that every exceptional
allocation across our three distinct markets preserves their scalar inequality.

## Exact mathematical basis

For a **single common scalar** x, define A=1[x>a], B=1[x>b] and I=1[l≤x≤u].
Require `a < l ≤ u ≤ b`, not `a ≤ l`:

```
I ≤ A − B
YES(A) + NO(B) + NO(I) = 2 + A − B − I ≥ 2
```

When x=l, A must already be YES; equality a=l would invalidate the proof.
When x=u=b, B is still NO, so an inclusive upper bound is valid. Values in the
fractional gaps between cent-valued strikes remain covered. Under the common
missing-data state, (A,B,I)=(0,0,0), giving payout two.

Independent discretionary YES values such as (.1,.9,.9) give basket payout .3;
they illustrate why a scalar proof cannot certify arbitrary exceptional outcomes.
This is a model counterexample, not a prediction about venue behavior.

## Admission and provenance

The pure qualifier requires both complete series catalogs, freshly acquired
metadata, exact reviewed terms hash, full series/event records, and complete
published fee-change responses. It verifies active binary contracts, unit
notional, exact rule text and bounds, common reference minute/expiry metadata,
source identity, event reference date, and expected event exclusivity. Unknown
conditions, mismatched dates, changed terms, duplicate ticker identities,
ambiguous threshold strikes and incomplete pagination fail closed.

Each leg receives its own public quadratic taker coefficient after paired event
overrides. Unknown schedules, partial overrides, and announced changes within
the observation window exclude the affected records. Private fee tiers are not
queried. Fee arithmetic/rounding belongs to the native screen. Funding assumes
gross purchase outlay without cross-event collateral release.

The caller supplies the oldest metadata request start; age must be between zero
and 300 seconds by default. This protects cohort preparation freshness; the
streaming book-age limit is separate. Full raw responses and timestamps are
archived by the runner. The qualifier adds canonical parsed-record SHA-256 hashes
for each market, series, event and fee schedule, explicitly distinguished from
raw-response byte hashes.

## Deterministic bounded selection

For each eligible interval, binary search selects the nearest strictly lower
threshold and nearest threshold at or above its upper bound. This is an explicit
small-cohort policy, **not** exhaustive cross-product discovery or an economic
optimality result. Candidates rotate across ascending expiry times. Within each
expiry, priority is descending minimum leg volume from the already acquired
24-hour metadata, then smaller enclosure slack, then a stable hash key. Quotes,
later trades, settlement outcomes and simulated profits do not affect selection.

Default bounds are 20 baskets and 64 distinct markets; complete-input safety
bounds are 8,192 contracts and 128 events. Separate counts report semantic
exclusions and budget exclusions. Event response hashing/fee validation is cached
once per event/reference time, avoiding quadratic serialization of embedded
market arrays. Threshold lookup uses sorted integer cent strikes.

## Verification

`python3 tests/basket_families_tests.py` covers real-number boundaries, common
missing data, exceptional-outcome classification, strict enclosure, identity,
rule/source/date/precision mismatches, stale metadata, pagination, fee overrides
and scheduled changes, deterministic expiry rotation, bounded selection,
immutable input records, price-independent ranking and source hashes.

The fresh event semantic check above admits 98/100 records with one common
reference tuple. It does not establish profitable depth, fill probability,
current exception certification, or economic return.
