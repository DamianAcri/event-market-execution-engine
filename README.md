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
- strict decoding of Kalshi order-book JSON messages with typed errors;
- Kalshi YES/NO book normalization on the unified YES-price scale;
- focused test executables and a small operator CLI.

The venue-neutral `eme_core` library has no Kalshi or JSON dependency. Kalshi
JSON decoding and normalization live in `eme_kalshi_gateway`; WebSocket
transport and authentication will remain at that boundary. The decoder resolves
the ticker carried by every payload through an explicit market registry, so an
update cannot silently be attached to the wrong internal market.

The Kalshi gateway requires subscriptions with `use_yes_price: true`. Both YES
bids and NO-derived asks then arrive on one YES-price scale, avoiding ambiguous
legacy price conversion inside the engine.

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
event-engine --version
```

`status` reports the implemented milestone and makes it explicit that execution
and authenticated connectivity are disabled.

## Credentials

No credentials are required to build or test the core. Future authenticated
connectivity will read a Kalshi key ID and a path to an RSA private-key file
from runtime configuration. Private keys must remain outside the repository.

## Safety

Development follows a demo-first policy. Production order submission will
remain disabled by default and will require explicit configuration, centralized
risk approval, and a kill switch before it is implemented.
