# Architecture

Event Market Execution Engine is a deterministic C++20 foundation for consuming,
recording, replaying, and validating event-market data. It deliberately separates
market-data correctness from strategy, connectivity, and order submission.

## Data path

```text
Kalshi frame -> raw journal -> strict decoder -> venue normalizer
             -> generation-aware market state -> local order books
                                      |
versioned constraint registry -> compiled worlds -> payoff verification
```

Live input and replay use the same decoder, normalizer, and state transition path.
The raw payload is journaled before interpretation so parser and normalization
changes can be evaluated reproducibly against the original bytes.

## Components

- `eme_core` contains fixed-point domain types, order books, multi-market state,
  the raw journal, and the constraint/payoff model. It has no Kalshi or JSON
  dependency.
- `eme_kalshi_gateway` is the venue boundary. It strictly decodes Kalshi JSON,
  resolves tickers through an explicitly versioned registry, normalizes YES/NO
  data, and applies records through the shared processor.
- `event-engine` is a thin operator CLI. It reports implemented capabilities and
  verifies journals without changing them.

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
Connectivity, opportunity detection, fee/depth evaluation, risk approval, and
execution are later layers. Production submission must remain disabled by default
and must not bypass centralized limits or a kill switch.

## Dependency direction

The venue gateway depends on the core, never the reverse. The CLI depends on the
core and has no trading authority. A core-only build can disable the Kalshi gateway
and therefore performs no JSON dependency download.
