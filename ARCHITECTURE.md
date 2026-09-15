# Architecture

Event Market Execution Engine is a deterministic C++20 foundation for consuming,
recording, replaying, and validating event-market data. It deliberately separates
market-data correctness from strategy, connectivity, and order submission.

Updated on 2026-09-15 for merged P1 sizing and P2 persistence preparation. Work order is owned by
[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md); future boundaries below are
identified separately from implemented components.

## Data path

```text
Kalshi frame -> raw journal -> strict decoder -> venue normalizer
             -> generation-aware market state -> local order books
                                      |
versioned constraint registry -> compiled worlds -> payoff verification
                                      |
normalized books + compiled legs -> incremental gross candidate lifecycle
                                      |
verified session + replay plan + policy -> offline cost/depth/funding evaluation
                                      -> scheduled IOC simulation -> JSONL study
```

The current replay reads verified raw records. Future live input feeds the same
decoder, normalizer and state transitions after retaining the original bytes.
The optional background recorder queues owned raw records and performs journal
I/O on one worker. Queue acceptance is not durability; transport/controller
integration must stop on recording failure. See [READONLY_CAPTURE.md](READONLY_CAPTURE.md).

## Components

- `eme_core` contains fixed-point domain types, order books, multi-market state,
  the raw journal, constraint/payoff model, incremental candidate tracker and
  exact fill-charge arithmetic and bounded net-profit sizing with explicit fee
  policies. It has no Kalshi or
  JSON dependency.
- `eme_kalshi_gateway` is the venue boundary. It strictly decodes Kalshi JSON,
  resolves tickers through an explicitly versioned registry, normalizes YES/NO
  data, and applies records through the shared processor.
- `event-engine` is the operator CLI. It provides journal/metadata inspection and
  session pack, verify, import, replay and study commands. These are offline
  operations and do not submit orders.
- `eme_session` composes the gateway metadata loader and core journal into a
  finalized offline artifact. It owns publication/integrity, explicit controller
  plans, structured replay, capture import and the current fixed-policy execution
  study. The study prepares available depth and calls core fee/sizing/reservation
  functions; it owns scheduling and simulated fills. An optional single-producer
  background recorder moves journal I/O to one worker without sharing mutable
  market state. Session JSON and SHA-256 remain
  at this boundary; gateway JSON remains in the gateway.
  See [SESSION_FORMAT.md](SESSION_FORMAT.md) and [OFFLINE_STUDY.md](OFFLINE_STUDY.md).

## Correctness invariants

1. Price, quantity, quantity delta, and cash are distinct fixed-point types.
   Contract settlement requires an explicit, overflow-checked quantity-to-cash
   conversion.
2. A delta is accepted only for the active connection generation, the expected
   stream, and the immediately following sequence.
3. A sequence gap, stream mismatch, arithmetic error, or invalid level makes the
   affected book non-actionable.
4. A stale book becomes valid only through the explicit
   `STALE -> RECOVERING -> VALID` snapshot path.
5. A snapshot cannot roll an already valid same-stream book backward. A snapshot
   from another stream invalidates the book and requires recovery.
6. Connection lifecycle errors belong to the market layer; order-book results
   describe only order-book transitions.
7. Journal metadata version and payload sequence are verified before state is
   mutated. Market IDs are supplied explicitly and do not depend on registration
   order.
8. Semantic relationships are curated as versioned definitions with provenance,
   then compiled once into canonical valid worlds and payoff templates.

## Safety boundary

The repository currently has no authenticated transport and cannot place orders.
The core tracks gross candidates using compiled two-leg templates and best prices.
Offline fee/depth/funding evaluation and assumed IOC fills exist in `eme_session`;
they are not an exchange connection or production risk approval. Transport,
operational order management and actual execution remain pending. Production
submission must remain disabled by default and must not bypass centralized
limits or a kill switch.

## Planned reuse boundary

P1 extracts the smallest reusable costed sizing decision into `eme_core`.
Simulation retains assumed arrivals and fills; a later venue adapter supplies
observed responses. Both call the same decision/ledger functions with explicit
state and time. Mutable state has one owner; asynchronous inputs must not retain
borrowed book views beyond their lifetime. Research oracles can be slower and
independent, but are not separate production implementations.

The plan does not select microservices, a generic strategy plugin framework,
an optimizer on every market update or a separate live strategy rewrite.

## Dependency direction

The venue gateway depends on the core, never the reverse. The full CLI depends on
the session library, which depends on the gateway and core; it has no trading
authority. A core-only CLI depends directly on the core, with gateway and session
libraries disabled, and performs no JSON dependency download.
