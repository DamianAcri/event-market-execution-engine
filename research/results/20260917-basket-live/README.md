# BTC basket observation, 17 September 2026

Capture: `basket-observe-20260917T145824.308090Z`. This is partial evidence from
real market messages. The planned 30-minute observation did **not** pass its
complete-window acceptance criteria. Original capture files are unchanged.

## What was observed

The frozen selection contained 20 three-leg BTC baskets across 46 markets,
selected from 126 conditional candidates in a 636-market catalog. The capture
recorded 140,104 book updates and 145 **public market** trades. The observer
performed 245,295 basket reevaluations and 24,054,100 quantity checks, with no
positive net quotes or episodes. These are correlated calculations, not
independent trials. There were no submitted orders or simulated fills, and
realized PnL is undefined (`null`).

The journal finalized and live/replay economic traces matched. The launcher
nevertheless reported `usable: false`, `planned_window_complete: false` and
`policy_window_expired_before_observation_finished`. Its locally configured
policy window elapsed; this does not establish a venue rule or fee change.

## Duration and the interruption

The journal spans 2,610.914 seconds. There were five connections with received
messages and a sixth connection attempt without any. Long intervals separate
the last received message from connection closure. The old observer integrates
the previous book state until the recorded closure and consequently reports
about 27m40s of `eligible_ns` per basket. **That is not verified continuous
coverage** and must not be used as the denominator of an opportunity rate.
`eligible_ns` also includes the `no_depth` state.

| Connection | First-to-last received-message envelope | Last receive to close |
| --- | ---: | ---: |
| 1 | 343.529 s | 305.865 s |
| 2 | 44.540 s | 298.875 s |
| 3 | 44.528 s | 298.958 s |
| 4 | 42.216 s | 251.697 s |
| 5 | 0.515 s | 30.011 s |
| 6 | No received messages | N/A |

The five receive envelopes sum to **475.328 seconds (about 7m55s)**. This is
only a description of when messages arrived: it includes initial snapshots
and does **not** certify continuous synchronized eligibility for every basket.
The final retry begins after a further 931.503-second gap. The monotonic and
wall clocks both record the long gaps; the journal alone does not identify
their cause.

A separate read-only inspection of the host's `pmset -g log` corroborated
system sleep. The following is a targeted transcription, with device and
process details omitted; it is not derived by `audit.py`:

| Local time, 2026-09-17 UTC+02:00 | OS event | Reported duration |
| --- | --- | ---: |
| 17:04:20 | Clamshell Sleep | 300 s |
| 17:09:20 | DarkWake | 46 s |
| 17:10:06 | Maintenance Sleep | 300 s |
| 17:15:06 | DarkWake | 46 s |
| 17:15:52 | Maintenance Sleep | 300 s |
| 17:20:52 | DarkWake | 45 s |
| 17:21:37 | Maintenance Sleep | 254 s |
| 17:25:51 | Wake, lid/UserActivity | 9 s |
| 17:26:36 | Clamshell Sleep | 925 s |
| 17:42:01 | DarkWake | 62 s |

The capture used `caffeinate -i`; it did not prevent these recorded sleep
events. The interruption and late closure need to be handled explicitly,
rather than interpreting carried-forward states as observed market time.

## Economic evidence that remains usable

All 18 recorded best-one-contract witnesses map to received messages in the
first connection, before its long terminal silence. Its largest interval
between received messages was 0.491 seconds. These witnesses remain useful
as point-in-time diagnostics under the declared conditional payoff and fill
cost model.

For example, basket 3 at offset 212.846 seconds had leg prices
`$0.50 + $0.63 + $0.85 = $1.98`. Against a **conditional** $2 payout floor,
that is $0.02 gross margin. The modeled fees were $0.05, producing **-$0.03
net margin** for one unit. The maximum book-update age/skew was 46.563 ms.
This is a concrete witness of fees consuming a gross discrepancy, not a
claim about the greatest gross margin across all prices or quantities.

The other best-one-contract diagnostics were also negative. Two baskets
(12 and 19) had no depth sufficient for one whole unit throughout their
recorded eligible states; basket 17 had depth only during part of its states.
Their original duration counters share the interruption caveat above. No
basket was rejected for insufficient capital or a crossed book.

The observer checked available whole sizes from 1 through 100 under its
configured $1,000 funding limit. A best-one-contract witness is not the best
negative quote across all sizes. The saved summary does not retain that
second statistic. The fill model assumes one fill per consumed price level;
there were no actual fills or execution-latency measurements. The payoff
model remains `conditional_common_scalar_observation_only`, with
`production_certificate: false`.

No profitability conclusion or extrapolation to the full venue follows from
this short, interrupted, selected observation. In particular, faster local
computation alone does not turn the example's negative net margin positive.

## Reproduce the evidence

`audit.py` uses only the Python standard library. It reads the provided
capture directory, verifies every journal record's CRC32 and framing,
checks record counts, manifest/hash bindings and witness mappings, and emits a
sanitized JSON report.
It reads no credentials, makes no network calls, and does not alter the
capture. Input artifact SHA-256 values and the audit script's hash are included.

```sh
python3 -B audit.py "$CAPTURE_DIRECTORY" > evidence-recomputed.json
cmp evidence.json evidence-recomputed.json
```

Run from this directory with `CAPTURE_DIRECTORY` set to the original capture
directory. `evidence.json` was generated from that capture with this script.
Absolute source paths and the raw OS log are deliberately absent from the
public artifact. The OS corroboration above is separate from the reproducible
journal audit.

Validation regenerated the report identically, checked the known record and
witness counts, and confirmed rejection of a corrupted journal CRC and a
changed manifest SHA-256 using temporary copies. Results are recorded in
`validation.json`; the original capture was never edited.
