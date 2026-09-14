# Finalized market-data sessions

A session binds the exact reviewed metadata artifact to an explicitly finalized
journal. Journal CRCs protect individual records; the session adds file identity,
byte counts and a final record count, including detection of whole-frame loss at
EOF. It does not change journal schema 2 or metadata schema 1.

## Files and manifest schema 1

```text
capture-directory/
  metadata.json
  market.journal
  manifest.json       # published only after successful finalization
```

The names are fixed; no arbitrary paths are accepted in the manifest.
`metadata.json` is the snapshot's canonical UTF-8 JSON without an added newline.
`market.journal` uses the existing binary format. `manifest.json` is canonical
JSON with a trailing LF and exactly these fields:

| Field | Required value or meaning |
|---|---|
| `schema_version` | Integer `1` |
| `state` | String `complete` |
| `venue` | String `kalshi` |
| `journal_schema_version` | Integer `2` |
| `metadata_version` | Nonzero unsigned 64-bit integer matching metadata and every record |
| `metadata` | Object containing exactly `bytes` and `sha256` for `metadata.json` |
| `journal` | Object containing exactly `bytes` and `sha256` for `market.journal` |
| `records` | Unsigned 64-bit count of successfully appended records; zero is allowed |

Artifact byte counts are unsigned integers. Hashes are 64 lowercase hexadecimal
SHA-256 characters over every file byte, including journal framing and record
CRCs. The manifest does not hash itself. Identical metadata and record inputs
produce identical manifest bytes regardless of directory name.

Manifest reads are bounded to 4 KiB, metadata reads to 4 MiB, and JSON nesting is
bounded. Duplicate keys, unknown/missing fields, wrong types, unsupported versions,
invalid digests and impossible count/size combinations fail. Artifacts must be
regular files; symlink artifacts and symlink session roots are rejected. Parent
directories are under the caller's control.

## Writer and publication contract

`create_session(directory, snapshot)` exclusively creates a new directory and
writes canonical metadata. An existing directory is rejected even when empty.
`SessionWriter::append` checks the metadata version, delegates to the unchanged raw
writer and tracks the accepted count. Invalid input rejected before I/O can be
corrected; a journal I/O failure poisons the session writer.

`finalize()` makes one attempt:

1. Stop accepting records and flush/close the journal.
2. Check metadata against the fingerprint retained at creation.
3. Scan journal framing, CRCs, count and every record's metadata version.
4. Compute its full fingerprint using a 64 KiB streaming buffer.
5. Write and close `manifest.pending`, then rename it to `manifest.json` in the
   same directory, refusing an existing final manifest.

The directory and writer have one owner. Concurrent append/finalize calls and
external file mutations are unsupported. Destruction never finalizes implicitly.
Failed creation, failed finalization or an interrupted process leaves artifacts
for inspection; a missing final manifest means `incomplete_session`. There is no
automatic resume, repair or overwrite operation.

Stream flush and same-directory rename provide a completion-publication protocol,
not power-loss durability: this layer does not call file/directory `fsync` or
platform equivalents. Verification can detect missing/corrupt artifacts after a
failure; it does not recover them. Production durability remains separate work
with explicit filesystem/platform assumptions and measurements.

## Verification and scope

`verify_session` validates the bounded manifest and exact metadata fingerprint,
then parses and compiles the reviewed metadata. It hashes the journal and
independently scans framing, CRCs, metadata versions and count. Success returns
the manifest and an owned metadata snapshot. No market book is mutated.

Finalized inputs must remain unchanged throughout verification and subsequent use.
SHA-256 identifies file content; an unsigned manifest does not authenticate an
author or prevent consistent replacement of every file. This is an integrity
format for owned offline artifacts, not a hostile-filesystem sandbox.

Verification establishes artifact integrity and internal binding, not that data
was timely, complete at its original source, or economically actionable. Raw
payloads remain uninterpreted, including malformed venue messages useful for
reproduction. Downstream decoding still checks payload/envelope sequences,
normalization and market-state transitions.

Connection-open/close and explicit recovery commands are not added to the raw
journal by this change. Full live control-history replay, opportunity lifecycle,
structured CLI market replay and economic-policy versions remain planned work.
Reproducing behavior also requires the relevant engine revision: this schema
binds data artifacts, not a build or trading policy.

## Offline CLI

With the Kalshi gateway enabled:

```sh
mkdir -p captures
event-engine session pack reviewed-metadata.json original.journal captures/run-001
event-engine session verify captures/run-001
```

The output directory must not exist and its parent must exist. `pack` imports a
regular, closed journal into a new session, checking records and preserving their
encoded bytes. Source files are unchanged. Failed imports leave an incomplete
directory, which is not silently reused on retry.

Packing an older journal seals the bytes available at import; it cannot prove
that records had not already been lost. A `SessionWriter` used from the beginning
of a capture can check its own accepted count during finalization.

Successful pack and verify produce the same stable summary of metadata version,
record count, journal bytes and both fingerprints. Exit status is 0 on success,
1 on validation/I/O failure and 2 for unsupported usage. Neither command opens a
network connection or submits orders. A core-only CLI has no session commands.

## Architecture decision

The small `eme_session` library composes `eme_kalshi_gateway` and `eme_core`.
JSON and SHA-256 stay outside the core-only dependency boundary. Hashing occurs
during artifact creation/finalization/verification, not each market delta. The
append wrapper adds a metadata-version guard and count bookkeeping.

CRC32 alone is insufficient as a whole-artifact identity. Duplicating metadata
inside the manifest would enlarge it without addressing journal identity. Platform
crypto backends introduce different setup requirements across operating systems.
The selected baseline vendors the small MIT-licensed
[PicoSHA2](third_party/picosha2/README.md) implementation at an exact revision. It
adds no configure-time download or runtime service. Faster hashing can be compared
later if closing/verifying large sessions becomes a measured bottleneck.

The journal is scanned separately for hashing and framing validation. This reuses
the established reader and bounds memory, at the cost of a second file pass.
Warm-cache verification benchmarks measure that cost; cold storage, append latency
and durability barriers are separate experiments.

## Validation

Tests exercise open/abandoned/empty/finalized sessions, overwrite refusal, mixed
versions, same-version metadata replacement, corruption, whole-record loss,
appended records, inconsistent counts, schema errors and truncated manifests.
Matching outer hashes do not bypass record CRC or metadata schema checks.
Publication collisions fail. Symlink rejection is tested where the operating
system permits creating fixture links.

SHA-256 checks include known empty, `abc` and million-`a` vectors. CLI tests compare
hashes against CMake's independent SHA-256 and confirm packed journal bytes match
the source. Measurements are in [PERFORMANCE.md](PERFORMANCE.md).
