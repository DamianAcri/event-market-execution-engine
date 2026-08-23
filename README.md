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
- Kalshi YES/NO book normalization for legacy and unified YES-price feeds;
- a dependency-free test executable and a small operator CLI.

Kalshi JSON decoding, WebSocket transport, and authentication remain at the
gateway boundary. The normalizer already accepts parsed wire DTOs and emits the
same venue-neutral events that replay and synthetic sources will use.

## Build

Requirements:

- a C++20 compiler;
- CMake 3.25 or newer.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

The current source tree has no third-party runtime or test dependencies.

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
