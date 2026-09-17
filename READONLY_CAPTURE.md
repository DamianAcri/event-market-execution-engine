# Read-only capture

The optional C++ TLS/WebSocket collector, strict subscription/recovery controller,
background recorder and replay are implemented. Synthetic TLS fixtures exercise
the complete path. **P2 observational acceptance remains open**: authenticated single-market and
eight-market production data captures have now passed (2026-09-16), but a
representative campaign and economic calibration remain pending. [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) is the execution plan.

## Running the optional collector

Build with Boost >=1.83 headers and OpenSSL >=3.0 installed:

```sh
cmake -S . -B build-live -DCMAKE_BUILD_TYPE=Release -DEME_BUILD_LIVE_CAPTURE=ON
cmake --build build-live --config Release
eme-capture reviewed-metadata.json new-capture-directory 3600 production
```

Set `EME_KALSHI_KEY_ID` and `EME_KALSHI_PRIVATE_KEY_PATH` in the launching
environment. The latter is a path to an unencrypted RSA private key, not the key
contents. When the OpenSSL installation has no configured trust bundle (which can
occur on Windows), set `EME_KALSHI_CA_FILE` to a trusted PEM CA bundle. An explicit
bundle replaces default trust paths; hostname/certificate verification stays enabled.
An encrypted/unavailable/non-RSA key fails without prompting. Credentials
are neither command-line arguments nor journal fields. The CLI only accepts the
production or demo venue; tests use an internal loopback fixture client. Missing
credentials fail before creating artifacts or opening a connection. No order API
exists in the collector.

The selected markets are the 1..64 explicit IDs in reviewed metadata. New captures
send one `orderbook_delta` command with `market_tickers` and `use_yes_price:true`.
Kalshi was observed to merge repeated same-channel subscriptions: the previous
one-market-per-subscription controller correctly stopped at the second `ok`
response, but could not record multiple markets. A single explicit group avoids
that incremental-subscription protocol. Dynamic membership is unsupported.

One acknowledgement binds the command to its nonzero subscription ID. A common
consecutive sequence covers every snapshot and delta in that subscription;
selection membership, per-market initial snapshots and recovery remain mandatory.
The venue-neutral `MarketState` checks the common predecessor before allowing a
book to advance past other markets' messages. This private book operation retains
the original sequence; a gap closes the connection and invalidates all books.
No sequence rewriting or public unchecked-delta API is introduced.

The official schema permits absent snapshot sides when that side is empty. The
new controller accepts these as empty vectors; present non-array/null/malformed
sides still fail. Legacy replay keeps its previous decoder semantics. Sources:
[orderbook protocol](https://docs.kalshi.com/websockets/orderbook-updates),
[complete schema](https://docs.kalshi.com/asyncapi.yaml).

TLS verifies the certificate chain and hostname, requires TLS >=1.2, and sets SNI.
OpenSSL signs a fresh millisecond timestamp + `GET` + `/trade-api/ws/v2` using
RSA-PSS/SHA-256, MGF1/SHA-256 and digest-length salt. Authentication failures stop
without reconnecting. Other failures reconnect with a capped exponential delay
(1,2,4,8,16,30 seconds), at most eight connection attempts. DNS/TCP/TLS/upgrade
and initial snapshots have ten-second deadlines; receive inactivity has a
30-second limit. A 100ms health timer also detects recording failure during idle
markets. SIGINT/SIGTERM or the requested duration stops and finalizes the capture.
These are bounded collection defaults, not tuned live trading latency limits.

Beast processes fragmented messages and replies to Ping automatically while a read
is pending. TCP_NODELAY is enabled. One Asio event-loop thread owns the connection
and mutable market state; the existing worker owns disk I/O. One decoded JSON tree
serves controller validation and the existing normalization/sequence checks. The
maximum reassembled text message is 1 MiB. Binary/oversized messages terminate the
generation; they are recorded as transport failures, not accepted book data.

## Research coverage and public trades (2026-09-17)

The runner retains `--profile baseline` for the original eight-market BTC sample.
`--profile research` prepares two eligible BTC events, up to sixteen thresholds
per event, plus up to sixteen NFL winner/spread markets for **observation only**.
Use `--btc-events`, `--btc-per-event` and `--nfl-markets` to set explicit budgets;
the requested total must not exceed 64. `--nfl-markets 0` disables sports.
BTC strikes are sampled at evenly spaced ranks, including both tails. This
provides coverage across strikes; it is not a profitability ranking. Events must
remain open for the requested duration plus ten minutes. NFL groups are ordered
by expected expiration, not by a verified live-game state. The sample stays fixed
throughout the run; it does not scan every market or resubscribe dynamically.

```sh
python3 scripts/capture_readonly.py --profile research --paper --seconds 7200 --max-mib 1024
```

`--prepare-only` downloads public metadata without opening credentials or an
authenticated connection. The runner archives every REST page, series terms and
hashes, and rejects incomplete/looping pagination or duplicate tickers. BTC and
NFL terms must match reviewed hashes. `selection.json` stores the chosen contracts;
`coverage.json` lists selected/excluded counts, roles and limitations. Terms and
selection are fixed before recording. NFL contracts have no certified constraints
or paper fee entries: exceptional discretionary settlements do not have an
established cross-contract payoff lower bound. See
[the research decision](ECONOMIC_STRATEGY_RESEARCH.md).

Research mode implies `--public-trades`, which can also be used with the baseline.
The native CLI accepts `[paper-policy.json] [--public-trades]` after the venue.
It sends a separate `trade` subscription for the same explicit tickers; no trade
subscription requests account fills or order endpoints. Book and trade streams
have independent subscription IDs and sequence checks. Readiness requires both
acknowledgements and initial books. A trade gap or malformed message terminates
the generation conservatively; reconnect requires fresh books. Sequence scope
must be revalidated if the venue protocol changes. The official
[AsyncAPI schema](https://docs.kalshi.com/asyncapi.yaml) defines sequence numbers
for detecting missing messages, including the trade payload.

Replay plan schema 4 records this dual-channel contract. Schemas 1–3 keep their
old interpretation and output. Normalized `public_trade` events preserve the
trade ID, YES price, fractional quantity, exchange milliseconds, taker outcome
and optional block-trade status. Original wire fields stay in the journal.
`ts_ms` is preferred; deprecated `ts` is accepted, with agreement checked if both
exist. Trades advance the causal clock but do not change book quantities, book
freshness, candidate prices or paper fill attribution. Trade IDs are not globally
deduplicated across reconnects; consumers must not count raw events as unique
fills without accounting for that boundary. `is_block_trade` absent is unknown,
not false. See [Public Trades](https://docs.kalshi.com/websockets/public-trades)
and [order direction](https://docs.kalshi.com/getting_started/order_direction).

The current paper strategy still uses simulated IOC arrivals. Recording public
trades does **not** implement passive orders, queue position or measured network
order latency. A future queue simulator must handle unknown cancellation position,
block trades, duplicate observations, and trade/book double counting explicitly.

Storage uses the existing raw binary journal plus small metadata/JSONL sidecars.
`--max-mib` defaults to 1024 MiB and requests a graceful stop when exceeded; the
five-second check and finalization can overshoot. It is a soft stop, not an exact
byte quota or a promise about two-hour size. An early stop is labeled in
`result.json`; a finalized partial window remains censored. No compression or
new external dependency was introduced.

## Controller history and replay

The collector persists raw text messages and controller actions into the existing
checksummed journal. Payload whitespace and escapes are preserved. The
manifest-bound `replay.json` schema 3 selects the shared-subscription controller
and lists market IDs. Schema 2 retains the original per-market subscription
controller; schema 1 retains legacy market-only replay. Existing captures are
never silently reinterpreted or renumbered.

| Channel | Meaning |
|---|---|
| `ws.attempt.v1` | New monotonically increasing generation; endpoint and timeout configuration. |
| `ws.open.v1` | Authenticated WebSocket upgrade completed. |
| `ws.send.v1` | Exact subscription command handed to asynchronous write; delivery is not asserted. |
| `ws.receive.v1` | Exact reassembled incoming text message, including acknowledgements and errors. |
| `ws.ping.v1`, `ws.pong.v1` | Received control payload encoded as a JSON string; no claim that an automatic Pong was delivered. |
| `ws.close.v1` | Local generation termination with a bounded reason code. |

In this controller envelope `sequence=0` means opaque input; the actual venue
sequence remains inside the original message. The gateway decodes and applies
that unchanged sequence. This differs explicitly from legacy market-only records,
where envelope and payload sequences must agree. Binary framing stays schema 2;
versioned channel names and replay-plan schema select interpretation.

An acknowledgement must match a sent command and identify the requested
channel. A first snapshot establishes each selected market. Every following
snapshot or delta advances the common stream sequence by exactly one. Duplicate
acks, extra snapshots, gaps, unknown markets/subscriptions, malformed messages or
venue errors invalidate every book and close the generation. Reconnection resets
the sequence scope, transitions stale books to recovery, and requires fresh
snapshots for each market. Replay requires terminal close history.

`manifest.json` certifies byte integrity, not venue authenticity or economic
validity. A successful run also writes `replay.json` and checks controller replay.
A duration stop can be a censored window. Failed connections are preserved for
review; finalized does not mean the entire interval had usable books. Storage
failure never publishes a completion manifest. Stream flush/rename remains a
process-level integrity contract, without an fsync power-loss guarantee.

## Tests and performance

The independent Python standard-library TLS/WebSocket fixture generates ephemeral
RSA credentials and certificates. It verifies the client's signature with the
OpenSSL CLI, exercises fragmentation/Pong, authentication rejection, untrusted and
wrong-host certificates, gaps, duplicate updates, malformed frames, subscription
errors, initial/idle timeouts, reconnect with reused sid/reset seq, message limits
and recorder overflow. Successful captures preserve original text bytes and
produce byte-identical repeated replay. All tests use loopback, never a venue.

Controller unit tests enforce invalidation and recovery. The portable feed
benchmark checks exact final sequence and depth. The optimization removes a full
payload copy and redundant JSON parse, retaining strict validation; measurements
and limitations are in [PERFORMANCE.md](PERFORMANCE.md). TLS/socket/venue latency
and economic performance are not inferred from a prepared in-memory workload.

## Implemented persistence boundary

`create_async_capture(directory, metadata, limits)` starts a C++20 writer around
the existing `SessionWriter`. One producer transfers owned `RawMarketRecord`
objects through `try_append`; one worker owns encoding, checksum, filesystem I/O
and final verification/publication. No networking dependency or order API is
introduced. The original synchronous writer remains available for offline tools.

The queue is a preallocated ring, with a release/acquire publication boundary for
filled slots and another for reclaimed slots. It moves payload ownership without
copying or parsing payload bytes. Publication indices are separately aligned to
avoid sharing typical 64/128-byte coherence lines. The worker waits on an atomic
epoch when idle; enqueue, failure and close change the epoch before notification.
Close is observed before checking the final published head, so an empty-queue
observation cannot discard the last accepted frame. The joining caller alone
reads the worker's final result.

The implementation follows the C++ memory-order contract rather than relying on
x86 ordering: [release/acquire](https://eel.is/c++draft/atomics.order) establishes
visibility, while [atomic wait/notify](https://eel.is/c++draft/atomics.wait) permits
idle waiting without continuous polling. Queue index publication is lock-free on
supported targets. The producer performs no journal encoding, checksum or file
I/O; notification/runtime scheduling is not a hard real-time guarantee.

Limits cover both record count and retained bytes, including the record currently
being written. Accounting uses string **capacity**, so shrinking a large string
does not evade the budget. The worker releases string allocations before making
their bytes available again. Defaults are 1,024 records and 16 MiB retained bytes;
the caller must select a justified budget for its message sizes and tolerated
backlog. Queue storage, metadata, the stream buffer and encoder scratch are
additional memory. The encoder reuses one bounded buffer instead of allocating a
new vector for every record; its maximum follows the journal's 16 MiB payload
limit. Journal bytes and checksums remain unchanged.

`queued` means ownership was accepted, not that the message is durable. Capacity
exhaustion permanently aborts admission for that capture. The controller must stop
using the generation, close the connection and start a new explicitly recorded
recovery/session; it must never drop a frame and continue as if the book were
complete. The producer can poll `status()` from its health/heartbeat path to
notice worker failure even when market data is idle. Errors also reach `finish()`.
A failed or abandoned capture does not
publish a completion manifest. Successful `finish()` drains, joins, verifies and
publishes the existing session format; it is an operator-side blocking action.
The existing flush/rename contract still provides no `fsync` power-loss guarantee.

All producer operations, stats, finish and destruction belong to **one caller
thread**. This is deliberately not a multiple-producer queue or a second owner of
market/decision state. Processing data on the future live path must check recorder
failure and retain original bytes before interpretation; accepted recording can
still fail before finalization.

## Verified behavior

Tests compare an 8,000-record background capture with synchronous output, including
exact whitespace and escaped payload bytes. They cover retained-capacity overflow,
permanent failure after a refused frame, metadata validation failure, finalization
failure, abandoned captures, wraparound, idle wakeups and close/drain. Another test
copies a complete replay fixture through the background writer and reproduces the
same manifest, causal decisions and simulated fills. ThreadSanitizer is a dedicated
CI check in addition to the existing portability, ASan and UBSan checks.

`eme_capture_benchmarks` compares producer call distributions **and** completion
through verified finalization. It alternates synchronous/background runs on equal
records and requires equal journal hashes. Bursts and scheduled arrivals measure
different conditions; low handoff time does not imply faster total storage or a
measured economic benefit. See [PERFORMANCE.md](PERFORMANCE.md).

## Protocol findings to carry into the next implementation

Official Kalshi documentation was checked on 2026-09-15:

- WebSocket connections require authenticated handshakes, including market-data
  channels. Production uses `wss://external-api-ws.kalshi.com/trade-api/ws/v2`.
  Sign the timestamp, `GET` and `/trade-api/ws/v2`; keep authentication material
  out of journals and diagnostic output. [Quick start](https://docs.kalshi.com/getting_started/quick_start_websockets).
- Request `orderbook_delta` for explicit tickers, with `use_yes_price: true`.
  Snapshot/delta NO-side prices then share the YES price convention used by this
  gateway. Defaults are subject to migration, so an implicit convention is
  insufficient. [Official schema](https://docs.kalshi.com/asyncapi.yaml).
- The feed sends an initial snapshot followed by deltas, and supports snapshot
  requests through `update_subscription`. Persist actual commands, replies,
  generation changes and recovery actions rather than reconstructing them from
  observed payloads afterward. [Orderbook updates](https://docs.kalshi.com/websockets/orderbook-updates),
  [connection messages](https://docs.kalshi.com/websockets/websocket-connection).
- Respond to venue Ping frames with Pong; the documented heartbeat interval is
  ten seconds. A live adapter must handle idle timeout and reconnect explicitly.
  [Keep-alive](https://docs.kalshi.com/websockets/connection-keep-alive).
- The schema includes subscription identifiers and sequence-bearing control
  responses, including subscription errors. Do not feed interleaved subscriptions
  into the current per-book consecutive-sequence assumption without validating
  actual scope. Keep original `sid`/`seq` bytes; do not renumber wire records to
  make replay pass. The initial controller validated a conservative per-market subscription contract.
  The observed shared-subscription correction is documented above. [Official schema](https://docs.kalshi.com/asyncapi.yaml).

The initial controller/transport addressed this preparation using local fixtures.
The subsequent observed acceptance below resolves subscription scope for the
current bounded group. The multi-date, reviewed-family and held-out-event
criteria remain unchanged; short connection checks do not complete P2.

## Operator-run research capture

After building the optional target, install `eme-capture` in `out/bin/` (or pass
`--binary` explicitly). Python 3.9+ and curl are required. Run:

```sh
python3 scripts/capture_readonly.py --seconds 7200
```

The runner reads `.config/event-market-execution-engine/.env.local` under the
user's home directory, containing `EME_KALSHI_KEY_ID` and
`EME_KALSHI_PRIVATE_KEY_PATH`, with optional `export` prefixes. It parses assignments
as data, never sources a shell, never prints their values, and never reads the
private-key contents itself. The collector consumes the key internally.

In the baseline profile, public preflight archives the current BTC series, market definitions and contract
PDF. The PDF hash must match the version reviewed on 2026-09-16. Selection requires
the exact reviewed above-threshold rule, same event/time/secondary conditions,
$1 notional and standard series fees. Eight thresholds nearest midpoint 0.5 are
frozen before capture, producing 28 implications; no title inference or cross-event
relation is used. The earliest eligible event must stay open for the requested
window plus ten minutes. A changed contract, partial listing or incompatible rule
stops preparation. `--prepare-only` does not load credentials or open WebSockets.

Artifacts go to a new `captures/<profile>-<UTC timestamp>/` directory, ignored by Git:
public sources, metadata, selection, provenance and binary/script hashes, result,
and the original finalized session. The default duration is two hours (maximum
three). The process reports disk usage each minute; Ctrl+C requests finalization.
The configurable soft storage budget (default 1024 MiB) and 1 GiB free-space
floor request early termination, with possible overshoot during the five-second check.
Keep the computer awake and connected. A partial capture is diagnostic data, not
a completed window; never delete it merely because the process failed.

No account/balance/order endpoints are used. No order transport is included.
The resulting folder can be passed to the offline replay/study tools or reviewed
in a later task. Repeated chunks must not be summed as independent profit runs:
funding, position holding, depletion and observation gaps require joint treatment.

## Observed acceptance, 2026-09-16

A 45-second read-only run on eight KXBTCD-26SEP1617 thresholds finalized with
2,613 market updates, one connection and 2,618 journal records. Controller replay
reported zero rejected updates and zero gross candidate events. These counts
validate the corrected protocol and replay, not economic viability. Two published
schema-2 captures also produced byte-identical old/new replay transcripts.

The longer campaign is deliberately left for the operator to launch. P2 still
requires broader event/date coverage; no fills, profits or account precision have
been calibrated. Raw public data and research artifacts stay outside Git.

## Optional live paper mode

Add `--paper` to `scripts/capture_readonly.py` to simulate the existing strategy
as data arrives while retaining the recording. The optional sixth `eme-capture`
argument is the frozen policy JSON. This does not enable any order endpoint.
See [LIVE_PAPER.md](LIVE_PAPER.md) for exact assumptions and output files. Live
and replay economic traces are compared after collection, without writing a
second verbose per-tick transcript. Credentials and capture-only defaults are
unchanged.


## Coverage preflight, 2026-09-17

The research profile prepared 48 markets: two BTC events with 16 sampled strikes
each and 240 within-event implications, plus 16 observation-only NFL markets.
A 30-second authenticated run finalized on one connection with all 48 snapshots,
no deltas/trades, zero simulated orders and exact live/replay accounting. Both
subscriptions were acknowledged. This validates setup and initial-book handling;
it does not validate actual trade-stream sequence behavior or economic activity.
Synthetic fixtures cover interleaved trades, independent sequence gaps and a
missing trade acknowledgement. Longer observations remain operator-run.
