# Live paper simulation and reproducible recording

The optional `--paper` runner mode evaluates the existing exact net-sizing and
residual-exit strategy as market data arrives. It submits **no orders**, in either
production or demo. The only application message sent over the authenticated WS
connection remains the reviewed orderbook subscription. Protocol Pong replies are
owned by Beast. No account, balance, trade or order endpoint is used.

## Run

Build the optional `eme-capture` target as described in [READONLY_CAPTURE.md](READONLY_CAPTURE.md).
With the operator binaries installed in `out/bin`, run from the repository:

```sh
python3 scripts/capture_readonly.py --paper --seconds 7200
```

On macOS, prefix the command with the built-in `caffeinate -i` and keep the lid
open. The process stops after the selected duration; Ctrl+C requests finalization.
The operator runs the session independently of this chat. `--prepare-only --paper`
archives public definitions and the policy without reading credentials or opening
an authenticated connection. Omitting `--paper` preserves capture-only behavior.

The existing storage guard now counts every file in the session directory. It
requests termination above the configurable `--max-mib` budget (default 1024 MiB)
or below 1 GiB free, checked every five seconds;
this is not a hard byte cap. It requires 3 GiB free initially. Sessions accumulate.

## Fixed first scenario

The runner chooses one existing policy before observing the stream. These values
are explicit experiment assumptions, not fitted parameters or verified venue
execution. They are archived and hashed; no tuning happens during the session.

| Parameter | Value |
|---|---|
| Selection | Baseline: eight BTC markets / 28 implications. Research profile: multiple BTC events plus NFL observation only; exact coverage archived. |
| Capital | USD 1,000 of fictional cash; no account balance lookup |
| Sizing | Existing exact funded net-profit sizing, whole contracts, maximum 100 per leg |
| Admission | Strictly positive costed margin; one attempt per constraint per session |
| Acquisition | Existing parallel two-leg IOC simulation |
| Unmatched inventory | Existing single bounded residual sale; paired holdings remain intact |
| Arrival / response delays | 100 ms each, including residual sale; assumptions, not measurements |
| Book age limit | 10 seconds since the last applied update for that market |
| Fill model | Available displayed depth at simulated arrival, with shared depletion and fees; no forced random rejects |
| Fees | General quadratic 0.07 with public series multiplier 1; cent-aligned balance scenario |
| Settlements | Empty: future outcomes are rejected by the live interface |
| Operating costs | Zero in this initial scenario; infrastructure costs are excluded |

Fee arithmetic reuses the existing ledger. The public
[rounding specification](https://docs.kalshi.com/getting_started/fee_rounding)
distinguishes direct-member and non-direct-member precision. The first run uses
cent alignment without assuming the user's account tier. The existing public
series selector rejects an unexpected fee type or multiplier. Fee changes during
the observation period and actual fill fragmentation remain model limitations.

This is not a continuously replenished trading policy: after an attempt, that
constraint is not retried in this session. Observed book updates do not magically
replenish simulated consumed liquidity. These existing restrictions are preserved.

## What is saved

Within the printed `captures/<profile>-<UTC>/session/` directory:

| File | Purpose |
|---|---|
| `market.journal` | Original WS messages and controller events; local paper clock events are explicitly labelled |
| `paper-policy.json` | Exact immutable starting policy for this run |
| `paper.jsonl` | Live decisions, simulated intents/fills/responses/exits, minute status, final holdings and timings |
| `paper-replay.jsonl` | Compact economic trace from verification after capture ends |
| `paper-summary.json` | Published only after live/replay economic traces and final accounting match; binds policy, manifest, replay plan and live trace hashes |
| `manifest.json`, `replay.json` | Existing integrity and controller-replay artifacts |

The parent directory also contains reviewed metadata, public source documents,
selection, provenance and `result.json`. `paper_verified: true` confirms agreement
between two input modes, not profitability or authentic exchange fills. Abnormal
termination remains visible in the exit code and stop reason even if trace
comparison succeeds. Preserve incomplete directories for diagnosis.

Unsettled positions stay open. They are not assigned invented settlement outcomes
or closed at the last quote. Simulated net P&L stays null when the accounting is
unresolved; the payout lower bound is a separate metric, not realized revenue.
Realized P&L is always null. Zero opportunities is a valid observation.

## Architecture and timing

The same `ExecutionSimulation` observer consumes replay and live feed updates.
One event-loop owner mutates market and simulation state. The existing bounded
SPSC writer persists raw data; a separate bounded writer persists sparse economic
JSONL events. Neither performs disk I/O on the decision callback. The economic
queue uses a short mutex for line publication; it is not a per-market-tick queue.
Overflow and output failures terminate the experiment instead of dropping lines.
No extra dependency or target-specific instruction set is introduced.

The simulation exposes its next pending deadline from the existing event heap.
An Asio timer records `paper.clock.v1` and advances that same observer even when
the market is silent or disconnected. Timers do not refresh books. Recorded clock
events preserve replay ordering; market observations at equal timestamps precede
simulated arrivals. At termination, pending events beyond the last recorded time
remain unknown and cash reservations are retained according to the existing model.

Fixed-memory histograms report sample counts, p50/p99 upper bounds and exact maxima
for book processing, the decision callback, receive-callback-to-decisions, and
timer lateness. They do not measure exchange order latency, physical packet arrival,
or buffering before the WebSocket callback. The existing simulation schedules
arrivals from the observation timestamp with declared delays; measured computation
is reported separately, not silently added or substituted. Runs whose computation
or scheduling delays are material relative to the assumed 100 ms require further
analysis. A late timer cannot be presented as proof of a real exchange fill.

The architecture follows the project's shared-decision-path plan and the applied
architecture guide's “We don't reinvent the wheel”, “We iterate on our work” and
“We never trade stability for speed” principles. A shared observer avoids two
strategies drifting apart; asynchronous sparse output adds one small worker and
bounded synchronization rather than adding network or account execution machinery.

## Interpretation and next experiments

The first recording is development/exploration evidence. Preserve all variants
and failed results; reserve later complete event dates before evaluating policy
changes. Two hours and many correlated ticks do not establish sustainable profit.
See [ECONOMIC_VALIDATION.md](ECONOMIC_VALIDATION.md) for the fixed-policy protocol.
Live paper adds prospective decisions and observed processing timings, while
replay makes debugging and controlled sensitivity experiments reproducible.
