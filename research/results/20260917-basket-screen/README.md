# Conditional BTC basket screen — 17 September 2026

The native screen and public preparation pipeline are implemented locally on
`feat/structural-basket-screen`, based on `ef63210`. This is a conditional
three-leg observation model, with no orders or simulated fills. Its mathematical
and contractual limits are in [contract-review.md](contract-review.md).

## Public acceptance observation

At 14:09 UTC, complete KXBTC/KXBTCD catalogues contained 636 markets and yielded
312 compatible nearest-enclosure baskets. The deterministic cohort, frozen
before fetching books, contained 20 baskets and 50 unique markets. The remaining
292 were budget exclusions; six tail contracts did not match the interval model.

- Eight baskets lacked sufficient displayed depth to buy one whole portfolio.
- Twelve had depth but no positive net margin at any tested funded whole
  quantity from 1 through 100. The native solver evaluated 1,200 quantities.
- For one unit per leg, the closest basket cost $2.03 including modelled fees
  against its conditional $2 minimum payout. No positive quote or P&L is claimed.
- An independent exhaustive Python `Fraction` oracle matched all 20 baskets,
  including individual legs, fee rounding, cash reservation and one-unit
  diagnostics. Re-running the native screen reproduced its entire saved output.

One snapshot does not measure opportunity frequency, duration, execution or the
unobserved cohort. This result neither establishes profitability nor rejects the
family across other times or expiries. It is retained as explored data, not a
future confirmation sample.

The initial public preparation took 8.437 seconds, including 19 JSON requests
and one terms download. It exposed an HTTP response-lifecycle defect which
caused six unnecessary reconnections; all requests completed. That defect was
subsequently fixed and independently tested. Do not treat this initial duration
as the final transport benchmark, or compare it as a speedup against an earlier
global catalogue scan with different coverage.

## Reproduction and provenance

[public-preflight.json](public-preflight.json) contains counts, scope, timing,
selected identities and source/executable fingerprints. Public normalized books
and fee assumptions are retained in [screen-input.json](screen-input.json);
[screen-output.json](screen-output.json) is the exact native result. With a
compiled CLI, from the repository root:

```sh
build/event-engine basket screen research/results/20260917-basket-screen/screen-input.json
```

This replays fixed snapshot arithmetic; its saved `as_of_ms` intentionally does
not assess whether old data could be used today. Raw public replies and the
terms PDF remain in the original local run archive; their hashes are retained.
No credential file or account data was read.

Synthetic native performance measurements are in
[native-cli.json](../../../benchmarks/results/20260917-basket-screen/native-cli.json).
They include process startup and are not feed-to-decision latency measurements.

All 44 local CTest entries pass. The loopback TLS transport fixture required a
repeat outside the filesystem/network sandbox after its local `bind()` was
blocked; that repeat passed. After the HTTP lifecycle correction, its 20 pool
checks and 10 preparation checks were rerun successfully. The native screen's
52 checks also pass under AddressSanitizer and UndefinedBehaviorSanitizer. These
are local macOS results; this branch has not yet run remote portability CI.

## Next evidence gate

Use a cohort fixed before outcomes and record distinct margin episodes across
declared market windows and expiries. Shared depth, feed age, missing data and
later confirmation sessions must remain visible. Only if positive independent
episodes exist should a three-leg completion-risk model and executor be built.
The existing two-leg paper collector is unchanged. The single task order remains
in [IMPLEMENTATION_PLAN.md](../../../IMPLEMENTATION_PLAN.md).
