# Event Market Execution Engine

A C++20 engine that records and replays Kalshi market data, maintains fail-closed
local order books, and verifies curated cross-market payoff constraints.

The project is intentionally built in this order:

```text
correct -> usable -> observable -> measured -> optimized
```

It is not a profitability claim and it does not send live orders in its current
state.

## Current state: offline replay and execution studies

**Start with [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md)** for the current
work order and acceptance criteria. [Exact net-profit sizing](NET_SIZING.md) is
implemented; [read-only capture](READONLY_CAPTURE.md) includes an optional TLS/WS
collector, bounded persistence and controller replay. Real authenticated sessions
and representative observations remain to validate. Research documents contain
evidence and conditional proposals; they do not create additional work queues.
The package version remains 0.2.0, and capability status is described below.

The completed v0.1 foundation establishes the venue-neutral core used by live,
recorded, and synthetic event sources:

- exact fixed-point price and quantity types (no floating-point money);
- a normalized local order book;
- snapshot/delta sequencing scoped to a stream;
- fail-closed transition to `STALE` after gaps or invalid updates;
- recovery only through a fresh snapshot;
- connection-generation tracking across multiple market books;
- explicit `STALE -> RECOVERING -> VALID` recovery transitions;
- strict decoding of Kalshi order-book JSON messages with typed errors;
- Kalshi YES/NO book normalization on the unified YES-price scale;
- an append-only binary raw journal with per-record CRC32 and sequential replay;
- focused test executables and a small operator CLI.

The venue-neutral `eme_core` library has no Kalshi or JSON dependency and can be
built without downloading the JSON package. Kalshi
JSON decoding and normalization live in `eme_kalshi_gateway`; WebSocket
transport and authentication will remain at that boundary. The decoder resolves
the ticker carried by every payload through a versioned registry of explicit
stable IDs, so registration order cannot change persisted market identity and an
update cannot silently attach to the wrong internal market.

The Kalshi gateway requires subscriptions with `use_yes_price: true`. Both YES
bids and NO-derived asks then arrive on one YES-price scale, avoiding ambiguous
legacy price conversion inside the engine.

Live frames and replayed records converge through the same Kalshi decoder,
normalizer, and generation-aware market-state processor. The journal stores its
own versioned header plus the metadata version, connection generation, local
monotonic and wall timestamps, sequence, optional exchange time, channel, and
original payload for each record. Payloads are length-delimited and each record is
checksummed, so record corruption and partial-frame truncation fail before replay
mutates state. A finalized session additionally detects whole-record loss at EOF.

The v0.2 constraint core accepts curated definitions with a stable ID, semantic
version, key, and provenance. Each definition is compiled once into canonical
valid worlds, dependencies, and payoff leg templates. A generic finite-world
oracle emits a `GuaranteedPortfolio` only after verifying its minimum cash
settlement across every valid world. Contract quantity and cash are distinct
fixed-point types. There is deliberately no title matching or probability
inference: semantic relationships must be curated explicitly.

Reviewed [metadata snapshots](METADATA_FORMAT.md) now load stable market IDs and
compiled relationships together, reject ambiguous/invalid input, and produce
deterministic canonical output. The CLI can verify or canonicalize a snapshot.
Finalized [sessions](SESSION_FORMAT.md) bind exact metadata and journal bytes with
SHA-256 fingerprints and a verified record count. The CLI can pack existing files
into a new session and verify integrity without mutating market state.
The core now tracks [gross candidates](CANDIDATES.md) from the compiled two-leg
templates at the best available prices, with stable identity and incremental
opened/updated/invalidated events. These exclude fees, funding and execution risk;
the [offline study pipeline](OFFLINE_STUDY.md) adds structured replay, explicit
controller plans, costed depth, conservative funding and delayed IOC simulation.
Exact two-leg sizing now chooses the best funded margin after fees. Background
recording preserves raw bytes with explicit memory limits and overload failure.
Read-only transport/controller integration is tested with local TLS fixtures.
The [simulated lifecycle](LIFECYCLE_STUDY.md) now compares parallel/sequential
acquisition through cash settlement, including unknown responses and residual loss.
Real feed validation, calibration and operational order management remain pending. The
[economic validation plan](ECONOMIC_VALIDATION.md) defines the evidence needed
to judge the hypothesis.

## Documentation

- [Single implementation plan: priorities, dependencies and acceptance](IMPLEMENTATION_PLAN.md)
- [Project direction and research basis](PROJECT_DIRECTION.md)
- [Quantitative models, execution research and economic experiments](QUANT_RESEARCH.md)
- [Applied economic research: search, sizing and joint execution (2026-09-15)](ECONOMIC_STRATEGY_RESEARCH.md)
- [Performance methodology and research](PERFORMANCE.md)
- [Architecture](ARCHITECTURE.md)
- [Raw journal format](JOURNAL_FORMAT.md)
- [Reviewed metadata snapshot format](METADATA_FORMAT.md)
- [Finalized session format and offline verification](SESSION_FORMAT.md)
- [Gross candidate calculation and lifecycle](CANDIDATES.md)
- [Economic validation and decision gates](ECONOMIC_VALIDATION.md)
- [Roadmap](ROADMAP.md)
- [Contributing](CONTRIBUTING.md)

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
For a network-independent core-only build, configure with
`-DEME_BUILD_KALSHI_GATEWAY=OFF -DEME_BUILD_CLI=OFF`.

## CLI

```bash
event-engine status
event-engine journal verify path/to/session.journal
event-engine metadata verify examples/metadata.snapshot.json
event-engine metadata canonical examples/metadata.snapshot.json
event-engine session verify path/to/finalized-session
event-engine --version
```

`status` reports the implemented milestone and makes it explicit that execution
and authenticated connectivity are disabled.

`journal verify` scans a raw journal without mutating it, verifies every checksum,
rejects incompatible or truncated data, and reports its record, generation, and
sequence range. CLI version output is generated from the CMake project version.

Metadata and session commands require the Kalshi gateway build. The
[session workflow](SESSION_FORMAT.md#offline-cli) shows how to pack existing
metadata and journal files into a new finalized directory.

## Credentials

No credentials are required to build or test the core. Future authenticated
connectivity will read a Kalshi key ID and a path to an RSA private-key file
from runtime configuration. Private keys must remain outside the repository.

## Safety

Development follows a demo-first policy. Production order submission will
remain disabled by default and will require explicit configuration, centralized
risk approval, and a kill switch before it is implemented.

## Offline economic studies

The [structured replay and execution-study guide](OFFLINE_STUDY.md) covers
`session import`, `session replay` and `session study`, versioned cost policies,
IOC simulation and the first public-data pilot. No credentials are required;
no orders are sent and no profitability has been established.
