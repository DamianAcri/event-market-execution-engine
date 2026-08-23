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
- a dependency-free test executable and a small operator CLI.

Kalshi-specific WebSocket, JSON, authentication, and YES/NO normalization stay
at the gateway boundary and will be added after the core invariants are stable.

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
