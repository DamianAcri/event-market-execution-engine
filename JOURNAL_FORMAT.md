# Raw Journal Format

The journal is an append-only binary capture of the original market payload and
its local processing envelope. All integers use little-endian byte order.

## File header

| Field | Size | Value |
| --- | ---: | --- |
| magic | 8 bytes | ASCII `EMEJNL02` |
| format version | `uint32` | `2` |

The format version describes framing. A reader rejects unknown versions.

## Record frame

Each record is stored as:

| Field | Size | Description |
| --- | ---: | --- |
| body size | `uint32` | Number of bytes in the body |
| body | variable | Schema described below |
| checksum | `uint32` | IEEE CRC32 of the complete body |

The checksum is verified before decoding. Truncation, impossible lengths, an
unsupported schema, and a checksum mismatch are distinct failures and include the
zero-based record index. EOF at a complete record boundary is valid for the raw
reader: it cannot tell whether whole records were removed. A
[finalized session manifest](SESSION_FORMAT.md) supplies the expected count, byte
length and SHA-256 fingerprint to detect that class of loss.

## Schema version 2 body

| Field | Encoding |
| --- | --- |
| schema version | `uint32`, currently `2` |
| metadata version | `uint64`, nonzero |
| connection generation | `uint64`, nonzero |
| monotonic receive time | signed nanoseconds encoded as `uint64` bits |
| wall observation time | signed nanoseconds encoded as `uint64` bits |
| sequence | `uint64` |
| exchange-time-present flag | `uint8`, `0` or `1` |
| exchange time | optional signed nanoseconds encoded as `uint64` bits |
| channel length | `uint32` |
| payload length | `uint32` |
| channel | exact bytes |
| payload | exact bytes |

Channels are limited to 256 bytes and payloads to 16 MiB. Empty channels and
payloads are invalid. Payload bytes are not transformed, so embedded nulls and
newlines round-trip exactly.

## Replay contract

The journal envelope is part of the deterministic input. Before applying a Kalshi
message, the processor requires:

- the record metadata version to equal the active market registry version;
- the record schema version to equal the processor schema version;
- the JSON payload sequence to equal the record sequence;
- the connection generation to be active;
- strict decoding and normalization to succeed.

Any mismatch fails before market state is changed. Writers scan and validate an
existing journal before appending, so they refuse to extend a corrupt file.

Version 1 files are intentionally rejected: schema 2 adds stable metadata identity
and checksummed framing. Migration should replay the original source capture into a
new file rather than silently reinterpreting bytes.
