# Reviewed Kalshi metadata snapshots

Schema 1, introduced in v0.2. See [the synthetic example](examples/metadata.snapshot.json).
This is an engine-owned format, not a Kalshi API response. Loading is an offline
setup operation; credentials and environment variables are unnecessary.

## Contract

The root object contains exactly `schema_version`, `metadata_version`, `venue`,
`markets` and `constraints`. Schema version must be 1 and venue must be `kalshi`.
`metadata_version` is a positive uint64 matching the version in journal records.
The curator assigns stable IDs explicitly; array position never assigns identity.

Each market has exactly a positive uint32 `id` and a nonempty `ticker` (up to 128
UTF-8 bytes). Neither an ID nor a ticker can occur twice, even with identical data.
Tickers are preserved byte-for-byte without case folding or Unicode normalization.

Each constraint has exactly:

| Field | Meaning |
| --- | --- |
| `id` | Positive uint32 stable constraint ID; unique in the snapshot |
| `semantic_version` | Positive uint32 version of this reviewed definition |
| `key` | Unique nonempty descriptive key, up to 256 UTF-8 bytes |
| `provenance` | Review/source reference, nonempty, up to 4096 UTF-8 bytes |
| `relationship` | One of the two exact shapes below |

An implication uses `{"type":"implication","antecedent":2,"consequent":1}`.
A complement uses `{"type":"complement","left":1,"right":3}`. Both market IDs
must exist and be different. Additional or missing fields are rejected at every
schema level. Duplicate JSON keys (including escape-equivalent keys) are rejected
before DOM overwriting can hide them. Numbers must be unsigned integer tokens:
booleans, floating-point/exponent forms, negative numbers, zero and overflow fail.
Strings reject ASCII control characters and values consisting entirely of spaces.
JSON syntax and UTF-8 validity are checked by the pinned parser.

Limits: 4 MiB input, 10,000 markets, 50,000 constraints, parser callback depth at
most 8. These are independent admission limits, not a capacity recommendation;
the byte cap can be reached before a count cap. At least one market is required;
zero constraints is valid for a market-data-only session. Resource exhaustion
exceptions propagate; they never yield a partially loaded snapshot.

## Review, ownership and reproducibility

`parse_metadata_snapshot` returns either a fully owned `MetadataSnapshot` or a
structured error code and field. It constructs temporary registries and checks
all references before returning a usable object. It does not update any active
session, write a file, or provide concurrent publication. The snapshot exposes
only const registry access. Keep it alive and unmoved for the lifetime of any
`OrderBookProcessor` borrowing its market registry.

Constraints compile once during loading. Insertion follows increasing constraint
ID, so dependency traversal order is stable under input permutations. Parsing,
sorting, allocations and payoff compilation stay outside market-delta processing.
The architecture decision follows the architectural-decisions skill's small,
explicit boundaries: reuse the existing Kalshi JSON dependency and registries,
keep the core build independent of JSON, and avoid a new general configuration
framework before another venue needs one.

Structural validation does **not** establish that a claimed implication or
complement is true under venue settlement rules. It also does not solve joint
satisfiability of all definitions. A curator must verify the contracts, matching
measurement/time/source and exceptional settlement provisions. Provenance is a
reference, not an automatically verified proof or fetched URL. The example uses
invented tickers and must not be interpreted as a trading recommendation.

Changing mappings or reviewed rules requires a new metadata version. Changing a
definition's meaning requires a new semantic version. The loader validates one
artifact; it cannot detect reuse of a version across different files. A future
session manifest must bind the exact snapshot bytes and journal; version equality
alone does not establish content equality.

## Canonical output and CLI

With the Kalshi gateway enabled:

```sh
./build/event-engine metadata verify examples/metadata.snapshot.json
./build/event-engine metadata canonical examples/metadata.snapshot.json
```

`verify` prints version and counts after complete validation. `canonical` prints
only compact JSON followed by one LF on stdout; errors go to stderr and return 1.
Neither command edits its input. Reads are bounded even if the file grows.
Builds with the gateway disabled retain the original core-only CLI commands.

Canonical JSON sorts the market/constraint arrays by numeric ID and object keys
using the pinned nlohmann/json serializer. Strings and relationship orientation
are preserved; no timestamps or generated IDs are added. The in-memory canonical
string has no trailing LF. This is our schema-1 deterministic encoding, not an
implementation of RFC 8785 and not a cryptographic hash or signature. Reordered
arrays, object keys and insignificant whitespace produce the same encoding.

Implemented: phase 1 steps 1–2 in [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md).
Still required: a crash-aware persisted session manifest, content binding,
opportunity identity/lifecycle and an end-to-end session replay command. The CLI
currently validates snapshots and journals independently.

## Verification and measurement

Tests cover all market/constraint permutations in the fixture, canonical
idempotence, integration with the existing journal processor and metadata-version
rejection. Generated market ID pairs, both relation types and fractional contract
quantities are checked against an independent exhaustive truth-table payout
calculation. Malformed input includes every truncated prefix of the fixture,
duplicate/escaped keys, unknown fields, integer boundaries, invalid references,
depth/size limits and trailing input. CLI tests cover canonical round trips,
failure without partial stdout, oversized/missing files and unchanged source bytes.

`eme_metadata_benchmarks` measures in-memory parse/validate/compile/canonicalize
for 32/512/4096 markets with twice as many synthetic constraints. Inputs are built
outside timing; each result is checked outside timing against the complete
canonical output and dependency counts. Destruction and file I/O are excluded.
Two warmup loads precede samples. It uses the existing comparison CSV schema with
batch size 1, so percentiles concern individual **snapshot loads**, not messages.
Use `benchmarks/compare.py` with two versions of this executable, `--samples 20`
and the same source/compiler/flags. See [PERFORMANCE.md](PERFORMANCE.md).
