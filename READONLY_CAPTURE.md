# Read-only capture preparation

P2 is **partially implemented**: the bounded background recorder and reusable
journal encoding are available and tested. The authenticated WebSocket transport,
subscription/recovery controller and representative multi-event observations are
still pending. This is not a live collector and does not complete P2 acceptance.
[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) remains the execution plan.

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
  make replay pass. The recorder implemented here preserves bytes but does not
  validate this protocol. [Official schema](https://docs.kalshi.com/asyncapi.yaml).

The next bounded delivery connects a read-only transport/controller to this
recorder and the existing processor, persists controller history, and exercises
authentication failure, heartbeat loss, subscription errors, sequence gaps and
reconnect with deterministic transport fixtures before venue observation. Captures
must preserve non-book control evidence as well as book data; the current market
replay stream alone does not supply that evidence. Actual sequence-scope and
same-session replay acceptance require an observed authenticated session.

No credentials were used for this preparation. No additional representative
session was collected. The original short REST pilot remains inconclusive; the
multi-date, reviewed-family and held-out-event collection criteria in the plan
are unchanged.
