# All-size costs and passive-entry hypotheses — 17 September 2026

**Decision:** retain the aggressive baseline as a control; the next development
gate is a bounded passive-entry feasibility study with trade/queue support and
delayed hedge costs. Do not implement a basket executor or repeat the same long
capture merely because a static passive quote looks profitable. The single
execution plan is [IMPLEMENTATION_PLAN.md](../../../project/IMPLEMENTATION_PLAN.md).

This analysis reuses `basket-observe-20260917T145824.308090Z`. Its interrupted
coverage is documented in the [capture audit](../20260917-basket-live/README.md).
Here we evaluate received book states, without estimating opportunity rates,
continuous eligible duration, fills or PnL. Original capture files are unchanged.

## Result

The replay reproduces 140,286 records, 140,104 book updates, 145 public trades,
245,295 basket reevaluations and 24,054,100 native quantity checks. The frozen
cohort contains 20 conditional BTC baskets across 46 markets. Each quote has an
independent fictional $1,000 funding limit, whole sizes 1–100 and the original
fee/rounding policy. Funding is not allocated jointly across baskets.

| Acquisition assumption | Baskets with some positive net quote | Meaning |
| --- | ---: | --- |
| All three legs bought from available depth | 0 / 20 | No positive conditional floor margin under the archived cost model. |
| One passive leg at the best bid, zero maker fee | 12 / 20 | Assumes that leg fills completely; not a fill prediction. |
| One passive leg, same fee coefficient as taker | 8 / 20 | Fee sensitivity scenario; not a verified maker schedule or execution bound. |

The aggressive baseline has 145,851 gross-positive quantity evaluations, all
non-positive after fees. Those evaluations share books, quantities and time;
they are not distinct opportunities or independent statistical trials. Two
baskets never have enough aggressive depth for even one whole unit.

The least-negative unit margin across available sizes is about **−2.233 cents
per basket unit**: −$1.63 for 73 units. There are tied witnesses. Increasing size
amortizes cent rounding but does not make this sample profitable. For example,
basket 3 at record 61,219 costs $1.98 before fees for one unit: +$0.02 gross,
$0.05 fees, −$0.03 net. Four units at that same state give +$0.08 gross, $0.18
fees and −$0.10 net. Quantities beyond available levels walk actual deeper prices.

## Why the apparent passive upside is not evidence of profit

The largest hypothetical unit margin occurs in basket 20 at record 42,639:

- Buy YES of the lower threshold at $0.94 and NO of the upper threshold at $0.11.
- Assume a NO purchase in the range market at its $0.12 best bid, with 299
  contracts already displayed there. The best available NO sale price is $0.98.
- The zero-maker-fee arithmetic for 86 units yields $70.45 net conditional margin.
  **This number assumes someone fills our bid; no such execution is modeled.**
- That range market has just two recorded public trades, both after the witness
  in the same connection, both at a NO price of $0.94. Neither supports assuming
  a fill at $0.12. These messages do not establish that a future fill is impossible.

Six of the eight positive stress-scenario baskets have **no public trades at
all in the selected passive market in this capture**. The remaining two have
one and two respectively. This is a market-level count for each best-per-unit
witness, not a complete enumeration of trade-supported entries. It does not
prove zero future activity. It does show why ranking this cohort by static
passive margin would prioritize unsupported assumptions.

We have not assigned fills from book reductions, assumed cancellations ahead
of us, used subsequent prices as contemporaneous hedges, or summed these
hypothetical margins as revenue. The passive scenarios are neither executable
profit estimates nor universal upper/lower bounds on future execution.

## Method and validation

[`basket_frontier.cpp`](../../tools/basket_frontier.cpp) is an offline diagnostic.
It uses production journal replay, fee ledger and funding reservation, caches
depth costs per updated market, and reevaluates only dependent baskets. It keeps
the three fixed modes above and checks all whole quantities, including negative
quotes. It performs 245,095 parity checks against the native basket sizer on
states with all three books valid; the other 200 reevaluations lack a valid book.
The original observer's quantity and update counts match exactly.

The passive leg is one hypothetical full fill at the displayed bid; the other
legs consume contemporaneous depth. The aggressive legs assume one fill per
occupied price level, retaining fractional quantities and order-level rounding.
Cash reservation separately allows centicontract fragmentation conservatively.
Neither case models latency, market impact or order completion. Fees in the
passive scenarios are expressly hypothetical. The conditional payoff still has
`production_certificate: false`; unresolved settlement exceptions remain a gate.

[`validate_frontier.py`](../../tools/validate_frontier.py) first runs the original
journal integrity audit, then checks all 58 nonempty best-per-unit witnesses with
the separate Python rational fee oracle. It checks all 100 sizes at each saved
witness for the specified passive leg, and rejects a one-microdollar corruption
of each witness. This independently validates saved arithmetic; it is **not** a
second independent reconstruction of every passive state or a fill-model test.
It also extracts public-trade context without inferring order executions.

The strict-warning C++ build passes and a second full replay is byte-identical.
See [validation.json](validation.json) and [provenance.json](provenance.json).
The first offline run took about 6.46 seconds on this host. That includes extra
diagnostics/parity work and is **not a live-engine latency benchmark**. Production
code, runtime launcher and dependencies are unchanged.

## Reproduce

From the repository root, use an existing Release build of the CLI/session
libraries. Set `BUILD_DIR` to its directory and `JSON_INCLUDE` to the existing
nlohmann include directory; these are operator paths, not captured credentials.
The following is the POSIX/Clang or GCC linking recipe (Windows needs its native
library names). On macOS select an SDK compatible with the compiler if needed.

```sh
c++ -std=c++20 -O3 -DNDEBUG -pthread \
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
  -Iinclude -Isrc -I"$JSON_INCLUDE" docs/research/tools/basket_frontier.cpp \
  "$BUILD_DIR/libeme_session.a" "$BUILD_DIR/libeme_kalshi_gateway.a" \
  "$BUILD_DIR/libeme_core.a" -o /tmp/eme-basket-frontier
/tmp/eme-basket-frontier "$CAPTURE_DIRECTORY" > /tmp/frontier.json
cmp docs/research/results/20260917-economic-frontier/frontier.json /tmp/frontier.json
python3 -B docs/research/tools/validate_frontier.py "$CAPTURE_DIRECTORY" \
  docs/research/results/20260917-economic-frontier/frontier.json
```

The validator requires Python 3.9+ standard library only. The source baseline and
binary/library hashes are retained in provenance. No account endpoints, API
settings, private keys or network requests are used. Zero orders are submitted.

Documentation relocation: the archived JSON manifests retain their original
paths and source hashes from this analysis. Current reproduction commands use
`docs/research/`; the validator's path resolution was updated for that move, so
its current source hash and emitted artifact paths differ from the archived
validation report. The frontier data and C++ diagnostic are unchanged.

## Boundaries of the conclusion

This was a short interrupted sample, with 20 of 126 conditional candidates
selected under an operational budget. No optimal market selection or optimal
capital allocation has been established. Point observations may be used for
diagnosis; the old integrated duration counters must not be used as evidence of
continuous exposure. All passive findings are exploratory and need future whole
expiry cohorts for confirmation. The source-to-decision mapping is in
[the applied research review](../../ECONOMIC_STRATEGY_RESEARCH.md#revisión-aplicada-de-colas-costes-y-tamaños--17-de-septiembre-de-2026).
