# Contributing

Changes should preserve deterministic replay and fail-closed behavior. Keep venue
details at the gateway boundary and represent domain values with strong fixed-point
types rather than floating point or untyped integers.

## Build and test

Run these commands from the repository root.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Warnings are errors by default. CI builds with GCC and Clang and also runs Address
Sanitizer plus Undefined Behavior Sanitizer.

To work only on the venue-neutral library without downloading the JSON dependency:

```bash
cmake -S . -B build-core -G Ninja \
  -DEME_BUILD_KALSHI_GATEWAY=OFF \
  -DEME_BUILD_CLI=OFF
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

## Change expectations

- Add a focused regression test for every corrected invariant or bug.
- Treat journal framing/schema changes as compatibility changes and update
  `docs/formats/JOURNAL_FORMAT.md`.
- Give persisted market and constraint metadata explicit stable IDs and versions.
- Do not infer semantic contract relationships from titles. Definitions must be
  reviewed, versioned, and carry provenance.
- Keep credentials, private keys, captures, and local research material out of Git.
- Do not add production order submission without explicit risk controls and review.

Before opening a pull request, run the full test suite and `git diff --check`.
