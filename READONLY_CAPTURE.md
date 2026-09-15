# Read-only capture

The optional C++ TLS/WebSocket collector, strict subscription/recovery controller,
background recorder and replay are implemented. Synthetic TLS fixtures exercise
the complete path. **P2 observational acceptance remains open**: no authenticated
Kalshi connection or representative multi-event campaign has been validated with
this delivery. [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) is the execution plan.

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

The selected markets are the 1..64 explicit IDs in the reviewed metadata file.
The initial policy sends one `orderbook_delta` subscription per market with
`use_yes_price:true`; acknowledgements bind command ID, subscription ID and market.
This intentionally avoids ambiguous interleaved market sequence scopes. Actual
venue acceptance and scope must still be checked in a captured session; if the
venue merges subscriptions or uses a different scope, the controller stops that
generation. It never rewrites venue `sid` or `seq` to appear valid.

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

## Controller history and replay

The collector persists raw text messages and controller actions into the existing
checksummed journal. Payload whitespace and escapes are preserved. The new
manifest-bound `replay.json` schema 2 selects this controller and lists market IDs;
legacy schema 1 plans and transcripts remain supported unchanged.

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

A subscription acknowledgement must match a sent command, assign a unique nonzero
`sid`, and identify the requested channel. A first snapshot establishes each
subscription. Consecutive deltas then go through unchanged core checks. Duplicate
acks, extra snapshots, gaps, wrong-market/wrong-subscription messages, malformed
JSON (including duplicate keys), or venue errors invalidate every book and close
the generation. Reconnection resubscribes each market, explicitly transitions
existing stale books to recovery, and requires fresh snapshots. No valid old
liquidity survives a failed generation. Replay requires terminal close history.

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
  make replay pass. The controller now validates the conservative one-market subscription contract
  described above; actual venue acceptance remains to be observed. [Official schema](https://docs.kalshi.com/asyncapi.yaml).

The implemented controller/transport addresses this preparation. Actual venue
sequence-scope and same-session replay acceptance still require an observed
authenticated session. No user credentials or additional representative sessions
were used in this delivery. The original short REST pilot remains inconclusive;
the multi-date, reviewed-family and held-out-event criteria are unchanged.
