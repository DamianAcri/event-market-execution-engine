# Event Market Execution Engine

A production-minded C++20 engine for event-market data, structurally related
contracts, risk-aware execution, and deterministic replay.

The project is intentionally built in this order:

```text
correct -> usable -> observable -> measured -> optimized
```

It is not a profitability claim and it does not send live orders in its current
state.

## Current milestone: v0.1 market-data correctness

The first milestone establishes the venue-neutral core used by live, recorded,
and synthetic event sources:

- exact fixed-point price and quantity types (no floating-point money);
- a normalized local order book;
- snapshot/delta sequencing scoped to a stream;
- fail-closed transition to `STALE` after gaps or invalid updates;
- recovery only through a fresh snapshot;
- connection-generation tracking across multiple market books;
- explicit `STALE -> RECOVERING -> VALID` recovery transitions;
- strict decoding of Kalshi order-book JSON messages with typed errors;
- Kalshi YES/NO book normalization on the unified YES-price scale;
- an append-only binary raw journal with corruption-aware sequential replay;
- focused test executables and a small operator CLI.

The venue-neutral `eme_core` library has no Kalshi or JSON dependency. Kalshi
JSON decoding and normalization live in `eme_kalshi_gateway`; WebSocket
transport and authentication will remain at that boundary. The decoder resolves
the ticker carried by every payload through an explicit market registry, so an
update cannot silently be attached to the wrong internal market.

The Kalshi gateway requires subscriptions with `use_yes_price: true`. Both YES
bids and NO-derived asks then arrive on one YES-price scale, avoiding ambiguous
legacy price conversion inside the engine.

Live frames and replayed records converge through the same Kalshi decoder,
normalizer, and generation-aware market-state processor. The journal stores its
own versioned header plus the connection generation, local monotonic and wall
timestamps, sequence, optional exchange time, channel, and original payload for
each record. Payloads are length-delimited, so newlines and embedded null bytes
round-trip without transformation.

## Build

Requirements:

- a C++20 compiler;
- CMake 3.25 or newer.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

The JSON boundary uses the pinned, header-only `nlohmann/json` 3.12.0 release.
CMake downloads it from the upstream release archive and verifies its SHA-256.

## CLI

```bash
event-engine status
event-engine journal verify path/to/session.journal
event-engine --version
```

`status` reports the implemented milestone and makes it explicit that execution
and authenticated connectivity are disabled.

`journal verify` scans a raw journal without mutating it, rejects incompatible or
truncated data, and reports its record, generation, and sequence range.

## Credentials

No credentials are required to build or test the core. Future authenticated
connectivity will read a Kalshi key ID and a path to an RSA private-key file
from runtime configuration. Private keys must remain outside the repository.

## Safety

Development follows a demo-first policy. Production order submission will
remain disabled by default and will require explicit configuration, centralized
risk approval, and a kill switch before it is implemented.
