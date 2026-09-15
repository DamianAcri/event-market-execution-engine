# Performance engineering

This document maintains performance methods, results and candidate experiments.
[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) alone selects current priorities;
the techniques below are not an additional work queue. No local timing is a claim
about exchange latency, execution success or profitability.

## Public research translated into experiments

| Source | Applicable idea | Experiment and constraint |
|---|---|---|
| [Jane Street: magic-trace](https://blog.janestreet.com/magic-trace/) (2022, first-party engineering report) | Sampling can miss very short work and rare latency events; inspect a timeline around a slow event. | Add targeted traces after locating a costly stage. Their Intel Processor Trace/Linux tooling is not portable to this Apple Silicon host; use an appropriate profiler on the actual deployment machine. Do not attribute every hosted talk to Jane Street's own implementation. |
| [Drepper: What Every Programmer Should Know About Memory](https://www.akkadia.org/drepper/cpumemory.pdf) (2007) | Data locality and cache behavior can dominate instruction-level changes. | Compare book representations at equal behavior across working-set sizes, density and churn. Historical hardware numbers are not predictions for our CPU. |
| [Langdale and Lemire: Parsing Gigabytes of JSON per Second](https://arxiv.org/abs/1902.08318) (VLDB, 2019) and [simdjson lifetime rules](https://github.com/simdjson/simdjson/blob/master/doc/basics.md) | SIMD parsing and reuse can reduce parsing work; borrowed views need explicit lifetimes. | Measure short Kalshi deltas and large snapshots through the full decoder/normalizer, preserve strict rejection, and include buffer reuse tests. Large-document throughput is not message latency. |
| [Kroer et al.: Arbitrage-Free Combinatorial Market Making via Integer Programming](https://arxiv.org/abs/1606.02825) (EC, 2016) | Structured payoff reasoning avoids naive enumeration at larger scale. | Current two-market definitions stay compiled and incrementally indexed. A generic solver per delta is not justified by this paper. |
| [LMAX Disruptor](https://lmax-exchange.github.io/disruptor/disruptor.html) (2011, engineering report) | Ownership, bounded preallocated storage and reducing coordination matter. | Measure before adding a queue; define overflow and ordering first. Their Java benchmarks do not establish our C++ performance. |
| [Google Benchmark: reducing variance](https://google.github.io/benchmark/reducing_variance.html) | Scheduling, frequency and machine state affect measurements. | Alternate baseline/candidate runs, preserve every sample and repeat on the target deployment host. Do not change global power or scheduler settings as a side effect. |
| [HdrHistogram](https://github.com/HdrHistogram/HdrHistogram) | A load generator that waits for each response can hide overload and queueing delay. | The current batch benchmarks measure local service cost only. Future paced replay must use scheduled arrival time to expose backlog and coordinated omission. |
| [Clang PGO](https://clang.llvm.org/docs/UsersManual.html#profile-guided-optimization) | Profile-guided decisions depend on representative execution. | Train on one workload, evaluate another, and record compiler/flags. Keep regular builds and sanitizer validation. |
| [RFC 1952, CRC specification](https://www.rfc-editor.org/rfc/rfc1952.html) | CRC can use a precomputed byte table while preserving the polynomial and wire value. | Compare against an independent bitwise implementation, known vectors, all byte values and unaligned lengths. CRC-32C is not interchangeable with the journal's IEEE CRC. |

These are public methods and sources, not a claim to reproduce a firm's private
trading platform. Our immediate priority is avoiding unnecessary work in the
existing path while retaining replay and failure semantics.

The [quantitative research supplement](QUANT_RESEARCH.md) adds public accounts from
Optiver on research-to-production iteration and Jane Street on incremental
computation, deterministic fault testing and jitter. It also covers execution
models, adverse selection, collateral and statistical validation. These methods
inform the experiments; the firms' names do not establish a performance target.

## Economic latency experiment

This is a proposed measurement layer, not an existing benchmark. On reserved
sessions, compare a fixed policy with controlled extra delays at feed reception,
decision processing, outbound order arrival and response delivery. Keep inputs
and seeds paired. An initial sensitivity grid might add 0, 0.1, 1, 5, 20 and 100 ms;
these are scenario values, not measured venue latencies or hardware requirements.

Report completed portfolios, captured quantity, fees, partial-execution losses,
net results, worst event loss, peak funding and capital-time exposure. Include
per-event stage latency and queue age under paced arrivals and bursts. State fill,
queue, collateral and market-impact assumptions beside each result; their quality
limits what a simulated economic latency curve can establish.

Use this curve to prioritize low-level work and compare deployment hardware by
incremental economic benefit and operating cost. Continue differential correctness
checks, portable fallbacks and workload-specific measurements. Compatibility on
multiple platforms does not imply equal speed across them.

## Build and run

```sh
cmake -S . -B /tmp/eme-release -DCMAKE_BUILD_TYPE=Release \
  -DEME_BUILD_BENCHMARKS=ON
cmake --build /tmp/eme-release -j 4
ctest --test-dir /tmp/eme-release --output-on-failure
/tmp/eme-release/eme_benchmarks --samples 200
```

Benchmarks are opt-in and add no third-party dependency. Disable the Kalshi
gateway to run the core scenarios entirely offline. Do not compare a Debug or
sanitized binary to a Release binary. No authentication or exchange request is
involved in any benchmark.

Retain a prebuilt baseline **before changing production code**, with the same
benchmark source and build settings as the candidate. Compare sequentially:

```sh
python3 benchmarks/compare.py /tmp/eme-baseline/eme_benchmarks \
  /tmp/eme-candidate/eme_benchmarks --runs 6 --samples 200 \
  --output /tmp/eme-comparison \
  --build-notes 'Record both revisions, compiler, flags, CPU and power conditions here'
```

The output directory must be new. The runner saves raw CSV, stderr, executable
SHA-256 hashes, OS/architecture, run order, supplied build notes, and a comparison
of paired runs. A changed scenario, operation count or output digest is an error.
It does not assign statistical significance or enforce a speedup threshold.

## Workload and metric definitions

The `eme_benchmarks` data is synthetic, with a fixed seed and workload version printed in CSV.
Preparation occurs outside timing; 16 warmup batches precede recorded samples.
Operations include lightweight result checks and a rolling observable digest.
No disk/network I/O or output formatting occurs in the measured interval.

| Scenario | Timed work | Important boundary |
|---|---|---|
| `crc_256/4096/65536` | CRC across 32 varying byte buffers | Payload footprint ranges from 8 KiB to 2 MiB. Does not model cold DRAM or storage. |
| `journal_roundtrip_256/4096` | Encode, one CRC, decode and release temporary allocations | Payload size is in the name; encoded records also include their envelope. Does not include file framing, a second read-side CRC or flush. |
| `book_update_32x64` | Apply deterministic changes and look up updated quantity across 32 markets | 64 levels per side; existing-level updates only, including state validation. |
| `book_churn_32x64` | Delete and recreate levels across the same books | Exercises allocation/deallocation as well as lookup. |
| `kalshi_decode_normalize_apply` | Raw JSON through the existing processor to a validated book | Prebuilt messages, one market, alternating quantity changes. |
| `verified_memory_replay` | Verify recorded CRC, decode journal envelope, then the same JSON-to-book path | Records already in memory. Excludes file reader, network, queue delay and opportunity evaluation. |

`mean_ns_per_op` is total timed work divided by operations. The `p50/p95/p99_batch`
columns are percentiles of **batch mean costs**, not individual-message latency
percentiles. Batching amortizes clock cost but conceals individual slow events.
Report full-path tails only after implementing paced arrival and per-event timing;
do not subtract an estimated timer overhead or relabel these columns as p99 latency.

The current workloads are a baseline, not a complete performance specification.
Add mixed snapshots/deltas, stale/recovery bursts, dependency fan-out, much larger
working sets and allocation/RSS measurement with the features that need them.

## Test plan: baseline and compatibility-preserving optimization

Based on the current journal, book state machine and benchmark implementation.

Scope: offline deterministic workloads, CRC compatibility and repeatable
comparisons. Transport, disk durability, order execution and profit are outside
this block. The existing file journal tests still exercise actual framing and
corruption detection.

Required checks:
- Known CRC check value, empty buffer, all 256 byte values, 16 starting offsets,
  and lengths around 8/16/32/256/4096/65536-byte boundaries match the reference.
- Generated binary payloads, with/without optional exchange timestamps, retain
  every record field; every incomplete encoded prefix is rejected.
- Existing on-disk journal corruption, append and replay tests continue to pass.
- Book workloads check updated quantities against an array reference, then every
  terminal bid and untouched ask; existing sequence/recovery tests stay green.
- Core-only and full gateway configurations pass, including benchmark smoke.
- GCC, Clang and ASan/UBSan remain clean with warnings treated as errors.
- Before/after digests agree in every repeated run; the comparison records both
  improvements and regressions. A faster incorrect result is a failed experiment.
- Shared CI validates execution/behavior, never a wall-clock performance promise.

Risks: changed polynomial or signed byte handling can silently invalidate old
journals; mislabelled batch statistics can hide latency spikes; unrepresentative
fixtures can reward an optimization that fails under churn or larger state.
Independent reference checks, explicit metric names and additional workloads
address those risks. A retained optimization needs measured benefit in its scope.

## Session verification workload

`eme_session_benchmarks --samples 8` measures complete offline verification of
0, 1,000 and 100,000 synthetic snapshot records, using one reviewed market and no
constraints. `raw_journal_verify_N` opens the file, checks framing/CRCs and the
expected count. `session_verify_N` also loads and validates the manifest and
metadata, computes both SHA-256 fingerprints, and checks record metadata versions.
The full result is compared against the fixture manifest and registry.

File creation/finalization and console output occur outside timing. File I/O,
validation and destruction of each returned result are inside timing. The cache
is warm from fixture creation and two warmups per scenario. Each sample is one
whole verification (`batch_size=1`); the CSV percentile columns describe whole
session calls, never per-message latency. Eight samples cannot establish reliable
tail percentiles. `bytes_per_op` reports journal bytes only, including its header
and frames; full verification also reads metadata and the manifest.

Use the same comparison runner with two `eme_session_benchmarks` executables.
Hashing and framing validation currently make separate journal passes to reuse the
existing reader. These results do not measure cold storage, capture append cost,
durability barriers, paced replay, exchange latency or economic opportunity loss.
Core-only builds exclude this target and its SHA-256 dependency.

## Gross candidate update workload

`eme_candidate_benchmarks --samples 100` runs in both core-only and gateway
builds. Each scenario initializes positive complement candidates, then alternates
the quantity at one existing ask level. Timed work includes the normalized book
delta, candidate refresh, output checks and a digest of every emitted identity,
kind, reason, leg price, quantity and cash result. Setup, payoff compilation and
initial candidate opening are excluded. Four warmup batches precede 100 timed
batches of 16 updates; CSV percentiles are batch-mean costs, not message tails.
`bytes_per_op=0` means this workload does not report a byte-throughput metric.

Scenarios grow from 64 to 8,192 definitions while holding the affected dependency
count at one, then exercise 32 affected definitions among 8,192. The same changed
market and affected state remain hot; unrelated definitions are deliberately
present to test work avoidance. This does not represent random-market cache
misses, reconnect bursts, large depth, JSON decoding, file I/O, fees, time-based
freshness, capital allocation or execution simulation. There is no parallel work
or target-specific instruction path in this component.

## Portability, profiling and hardware decisions

The correctness matrix now includes Windows 2022/MSVC x64, Linux GCC arm64 and
macOS 15/AppleClang arm64 Release builds, alongside Linux x64 GCC/Clang Debug,
ASan/UBSan and the dependency-free core build. The selected architecture labels
are documented by [GitHub](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
The first matrix run on `e71337d` passed all seven jobs. This checks compilation,
behavior and smoke workloads; it does not establish comparable speed on these
shared hosts. Runtime libraries, code generation and scheduling still differ.

Our engineering application of the public research is to measure the stage that
limits useful decisions, including rare delays. Jane Street's
[magic-trace account](https://blog.janestreet.com/magic-trace/) shows why sampling
can miss very short or rare paths and why tracing overhead matters. Its Intel PT
implementation is hardware/platform-specific; adopting its measurement approach
does not require adding it to an ARM/Windows build. Start with stage-level timing
and recorded inputs; choose a supported profiler when that evidence warrants it.
This is a public engineering example, not evidence about proprietary strategies.

Google Benchmark's [variance guidance](https://google.github.io/benchmark/reducing_variance.html)
motivates recording machine load, frequency behavior and repeatability. Keep
compiler/flags/inputs fixed within each A/B comparison. Run sequentially, preserve
raw samples and test a held-out workload before accepting a specialization. Do
not tune host power/security settings implicitly or compare CI wall time as if
it were a controlled CPU experiment.

Hardware selection remains an experiment, not a shopping recommendation:

| Decision | Evidence required |
| --- | --- |
| Development machine | Supported compiler, complete tests, practical build/replay time |
| Operating host | Arrival-to-decision tails under paced bursts, network path and recovery |
| RAM | Peak resident memory for intended books, dependency fan-out, parser and bounded queues |
| Storage | Capture bytes/second, retention duration, durability policy and disk stalls |
| CPU specialization | Same workload on target x64/ARM CPUs, portable fallback, net path benefit |

Do not infer minimum RAM from the 32 GiB development Mac. Do not buy dedicated
hardware before representative capture and execution simulation show its benefit.
The next load baseline is `eme_metadata_benchmarks`: snapshot startup cost and
strict validation, outside the per-message path. Its parser callback explicitly
rejects duplicate keys; returning false from a
[nlohmann parser callback](https://json.nlohmann.me/features/parsing/parser_callbacks/)
would filter data, which is unsuitable for fail-closed metadata validation.

## Measurement log

### 2026-09-14: portable CRC table and reuse of an existing book iterator

Baseline: `8d8046a` (benchmark foundation, production code unchanged). Candidate:
`f555484` (the two production optimizations). These are the rebased equivalents of
the built trees `db345ce` and `07c1105`; rebasing only integrated the already-merged
direction document. The benchmark source SHA-256 is
`f2759cbec37128b48cb489c441b5dbbf30e7ba4ede07094ac4ae30f684f661fa`.

Environment: Apple M2 Pro, 32 GiB RAM, AppleClang 21.0.0.21000101, CMake 3.31.6,
macOS SDK 26.5, Release `-O3 -DNDEBUG -std=gnu++20`, no PGO/LTO/native tuning.
No CPU pinning or power-setting changes; ordinary desktop background activity
was not controlled. No local builds ran during measurement.

Six pairs of processes ran in alternating A/B and B/A order, each with 200 timed
batches per scenario. Every scenario and output digest agreed across all twelve
processes. Values below are medians of per-process mean costs, in nanoseconds;
change is the median of the six paired percentage changes, which need not equal
the ratio of the two displayed medians.

| Scenario | Baseline ns/op | Candidate ns/op | Paired change | Range of paired changes |
|---|---:|---:|---:|---:|
| CRC, 256 bytes | 1,962.4 | 561.7 | -71.4% | -72.7% to -70.9% |
| CRC, 4 KiB | 31,997.8 | 10,261.7 | -67.9% | -68.6% to -67.5% |
| CRC, 64 KiB | 513,191.0 | 164,227.9 | -68.1% | -69.4% to -67.6% |
| Journal round trip, 256 bytes | 2,590.5 | 890.6 | -65.4% | -66.3% to -64.5% |
| Journal round trip, 4 KiB | 32,886.0 | 10,725.0 | -67.5% | -67.6% to -66.8% |
| Existing book levels, 32 x 64 | 26.1 | 19.8 | -24.7% | -31.9% to -17.9% |
| Book deletion/insertion, 32 x 64 | 54.9 | 46.6 | -12.2% | -19.6% to -3.8% |
| Kalshi decode/normalize/apply | 1,650.9 | 1,622.2 | -1.7% | -6.2% to +1.8% |
| Verified in-memory replay | 3,280.4 | 2,185.7 | -33.5% | -34.4% to -32.6% |

Retained changes: a 1 KiB compile-time CRC table and direct assignment through
the book iterator already obtained for validation. No wire-format or state-machine
change. The parser path has no repeatable improvement established by this run;
the small change overlaps observed variation. The CRC and local replay benefits
were consistent across pairs, but these are synthetic service-cost measurements
on one machine, not exchange response times or a statistical confidence interval.

Raw CSV, paired summary and executable hashes are retained in the local experiment
artifact `calci-performance-20260914`; future runs should use the checked-in runner
and retain their own artifacts. Reproduction can build the two revisions above
with the same toolchain. Full Release and ASan/UBSan suites passed (12 CTest cases
each); the added codec executable performs 11,332 checks. CI also builds these
scenarios on GCC/Clang and in the core-only configuration.

Validation of candidate `551f61f` (documentation added, production code unchanged):
[all four CI jobs passed](https://github.com/DamianAcri/event-market-execution-engine/actions/runs/34867707844).
On this Mac, the six core-only correctness executables passed, but the core-only
benchmark process was repeatedly killed and its executable became unavailable,
including after an authorized run outside the sandbox. The host logs inspected
did not establish a definitive cause. The same core-only smoke passed on Linux
CI, and the full benchmark passed locally in Release and with sanitizers. No host
security settings were changed; the local core-only smoke remains unverified.

### 2026-09-14: snapshot loading and removal of compiled-constraint copies

Added a separate startup workload without changing the existing engine benchmark
source. Baseline `ee40c5f` introduces the snapshot loader; candidate `631ea40`
moves each compiled constraint into its registry instead of copying its strings,
worlds, legs and dependencies. The intervening `240c2c6` only renames a constructor
parameter for MSVC and strengthens a test assertion. No JSON admission rule,
canonical encoding or payoff definition changed in the optimization.

Same M2 Pro/toolchain/Release flags as above. Six alternating A/B pairs, 20 loads
per process per scenario, two warmups. Complete canonical output and dependency
counts agree; the comparison runner verified matching source/workload digests.
No concurrent local builds or tests ran during measurement. Background load and
frequency were uncontrolled. Reported costs exclude destruction of the returned
snapshot, output validation, input generation and file I/O.

| Markets x constraints | Baseline median mean (ms/load) | Candidate (ms/load) | Median paired change | Paired range |
| --- | ---: | ---: | ---: | ---: |
| 32 x 64 | 0.284 | 0.268 | -6.3% | -12.7% to -3.6% |
| 512 x 1024 | 6.555 | 6.238 | -4.7% | -23.1% to +63.9% |
| 4096 x 8192 | 168.014 | 166.553 | -0.5% | -1.8% to +4.1% |

Retained: a simpler ownership transfer with a consistent measured benefit for
the small setup workload. The larger cases do **not** establish a consistent
speedup. Do not quote their median changes as reliable gains. Setup at this scale
is not instantaneous; its measured cost stays outside per-message processing.
No allocation-count, peak-memory or hardware-general speedup claim is made.

Metadata benchmark source SHA-256:
`c81c7880bed6b138fcdd933bed9edb839a3398ca2da0f8134d87e9382c3ae31b`.
Raw process CSV, summaries, run order and executable hashes are preserved in the
local artifact `calci-metadata-performance-20260914/metadata-load`.

A separate six-pair, 200-sample regression comparison used the unchanged nine
engine scenarios against the pre-metadata PR #2 binary (production equivalent to
main `9c2d2fa`). All workload/output digests agreed. Verified in-memory replay
median process means were 2,185.023 vs 2,186.806 ns/op; paired changes ranged from
-3.1% to +18.2%. The medians are effectively unchanged, but variation prevents
claims about small speed differences or worst-case latency. No consistent engine
regression was established. Raw results remain in the same artifact's
`engine-regression` directory; do not interpret the setup gain as a trading-path
gain. Both local Release and ASan/UBSan suites passed all 16 CTest cases.

### 2026-09-14: finalized-session integrity and bounded metadata allocation

Added session manifests and offline pack/verify above the existing gateway and
journal. The engine's decoder, book, journal codec and benchmark source are
unchanged. SHA-256 is outside the market-update path.

The first session verifier allocated `maximum_metadata_bytes + 1` for every
metadata read (4 MiB plus one byte). The retained implementation first checks the
regular file's size against that limit, then allocates only its size plus one and
rejects any growth/shrinkage observed during reading. The extra byte and the byte
bound are preserved. No peak-RSS or allocation-count measurement is claimed.

Same M2 Pro/toolchain/Release flags as the earlier measurements. Six alternating
A/B process pairs, eight verification calls per process/scenario, two warmups;
no concurrent local builds, tests or benchmarks. Ordinary desktop background
activity and frequency were uncontrolled. The baseline executable was retained
before changing the bounded reader; benchmark source and fixtures are identical.
Every process agreed on workload and output digests.

| Full session records | Journal bytes | Baseline median mean (ms) | Candidate (ms) | Median paired change | Paired range |
|---|---:|---:|---:|---:|---:|
| 0 | 12 | 0.162 | 0.122 | -26.4% | -32.8% to -12.7% |
| 1,000 | 213,905 | 2.369 | 2.160 | -6.4% | -44.3% to +42.3% |
| 100,000 | 21,588,907 | 201.907 | 201.264 | -0.3% | -14.2% to +1.9% |

Only the empty-session case showed a consistent improvement across these pairs.
Larger cases do not establish a repeatable speedup. Retain the file-sized
allocation because it avoids unnecessary initialization for small artifacts while
preserving bounded reads, not because it makes large captures much faster.

For context, the candidate's raw journal-only verification medians were 0.025,
0.664 and 65.280 ms respectively. The full-session work has a real offline cost:
hashing, metadata validation and an additional journal pass. Raw verification
does not provide the same artifact identity guarantees, so those ratios are not
an optimization comparison.

Session benchmark source SHA-256:
`12b5ae76077f5e21752ff7e7e04f89b19ce436eb794ac4e41195965c2b9c61f5`.
The pre-optimization `session_files.cpp` SHA-256 is
`5b35ef0784cfa27803a3136834bfec96e617b718a39e2e2cb4ca4efef33af5cb`.
Its source, the reverse patch, raw CSV, executable hashes and run order are
preserved in the local artifact `calci-session-performance-20260914/verification`.

A separate six-pair, 100-sample comparison built main `d762325` and this branch's
unchanged engine benchmark. All source/workload/output digests agreed. Verified
in-memory replay median process means were 2,169.241 vs 2,164.225 ns/op, with
paired changes from -23.6% to +4.9%. No consistent engine regression was
established; this is not a speedup or a bound on tail latency. Those raw results
remain in the same artifact's `engine-regression` directory.

Local validation passed all 19 Release CTest cases, all 19 ASan/UBSan cases and
all 8 core-only cases with the CLI enabled and benchmarks disabled. Session
tests include SHA-256 known vectors, independent CMake hashes, complete-frame
loss, same-version metadata replacement and failed finalization. Cross-platform
CI validates behavior, not equal performance across CPUs or operating systems.

### 2026-09-14: dependency-driven gross candidate lifecycle

Baseline `298532d` implements the candidate tracker with a complete refresh for
every market change. Candidate `43e65b3` uses a precompiled market-to-entry index
for normal updates and retains global scans for connection/generation changes.
The existing `refresh_all` remains the differential correctness reference.
The candidate also adds a verified-session integration fixture; it is not part
of the timed workload. No book, decoder, normalizer or journal production code
changed in this comparison.

Apple M2 Pro, 32 GiB; AppleClang 21.0.0.21000101, CMake 3.31.6, Release
`-O3 -DNDEBUG -std=gnu++20`, no PGO/LTO/native tuning. Six alternating A/B process
pairs, 100 timed batches per scenario, 16 updates per batch. No other local builds,
tests or benchmarks ran concurrently; desktop activity/frequency were uncontrolled.
All source/workload/output digests agreed across all twelve processes.

| Total definitions | Affected definitions | Baseline median mean (ns/update) | Candidate (ns/update) | Paired reduction range |
|---:|---:|---:|---:|---:|
| 64 | 1 | 816.369 | 26.770 | 95.96%–98.07% |
| 1,024 | 1 | 12,934.961 | 28.165 | 99.76%–99.79% |
| 8,192 | 1 | 103,043.193 | 26.862 | 99.97%–99.98% |
| 8,192 | 32 | 103,796.655 | 1,077.124 | 98.86%–99.02% |

The large reduction measures avoiding unrelated work compared with our simple
full-scan reference, not an equivalent speedup of the whole trading system. It is
consistent across these pairs. The indexed cost depends on affected fan-out;
global connection changes still scan all definitions. The retained index adds
startup work and linear storage. No allocation-count, RSS, cold-cache, tail-latency
or economic-benefit claim is made.

Benchmark source SHA-256:
`cd93f5a0fccc946a77ac15d07596b6a747482a257ca607ddabb25ee0c4eb4952`.
Raw CSV, executable hashes, build notes and run order are preserved in the local
artifact `calci-candidate-performance-20260914`. Build the two revisions above
with identical options and use `benchmarks/compare.py` to reproduce the comparison.

Validation: 2,475 core candidate assertions, an independent cash/settlement grid,
generated event comparisons against full scans, and deterministic verified-session
integration. All 22 Release CTest cases passed. All 22 sanitizer cases passed
across the full run and the corrected integration-fixture rerun. The new core-only
candidate benchmark and all nine other non-engine-benchmark cases passed locally.
The pre-existing core-only `eme_benchmark_smoke` was again killed by this Mac,
as previously recorded above; the outside-sandbox retry was not authorized.
That local smoke remains unverified. Cross-platform CI remains required before
delivery and must independently exercise the complete core-only configuration.


## Costed study replay: funding-search reuse (2026-09-14)

The first costed implementation is commit `c2bc62e`. The optimization reuses the
initial depth quote and skips binary sizing search when full size is already
funded. If funding is tight, the same bounded search and acceptance policy apply.
No SIMD, architecture-specific instructions, new dependencies or speculative
concurrency are introduced.

`eme_study_benchmarks <session> <plan> <policy> <samples>` verifies the session
once outside timing, then measures policy reading/hashing, streaming journal
replay/CRC/JSON processing, cost evaluation, simulation and full JSONL formatting
into a digest sink. Each run rebuilds its simulation state; no output is elided.
The benchmark requires equal full-output digests for every repetition. It is an
offline study cost, not a hot-message or exchange round-trip latency measurement.

Benchmark source SHA-256: `3b799b905816a7988ee76f928768a2fa76b0a16fb7c19da4ba7fd7b0e27e3190`.
Apple M2 Pro, AppleClang 21, CMake Release/O3, no native tuning, PGO or LTO.
Six alternating baseline/candidate pairs per budget, 50 measured complete runs
and three warmups per process. No concurrent compilation/tests during measurement.
Input: the same 96-record observed REST pilot described in [OFFLINE_STUDY.md](OFFLINE_STUDY.md),
28 relationships, cap 100 contracts, 1 ms symmetric arrival assumption, direct
account quantum and declared general 0.07 fee coefficient. Neither version finds
an opportunity, so these measurements describe rejection-heavy replay; they do
not establish performance while filling orders.

| Scenario | Baseline median run mean | Optimized median run mean | Ratio | Interpretation |
|---|---:|---:|---:|---|
| $100 budget, funding search usually needed | 4.312328 ms | 4.283664 ms | 1.0067x | Difference within observed noise; paired ratios 0.9750–1.0642. |
| $1,000 budget, full-size reservation fits | 4.295766 ms | 4.007557 ms | 1.0719x | 6.71% less time; all six paired ratios improve, 1.0463–1.0883. |

Both scenarios retain identical complete-output digests across implementations:
`4507955345570429261` ($100), `1749662937826656227` ($1,000). Input fingerprints,
raw CSVs, binary hashes and the baseline source/binary are retained in the local
`calci-execution-study-20260914/performance` artifacts. Re-run on other architectures
before making hardware recommendations. The measured speedup says nothing about
whether a local time saving changes an economic result.

## Exact net-profit sizing (2026-09-15)

`eme_sizing_benchmarks` compares bounded exact selection with exhaustive selection
over 10,000 quantity choices, using identical fees, depth and funding. It checks
equal quantity/margin before timing. Each scenario has 12 alternating paired
samples of ten calls; construction is outside timing. Apple M2 Pro, AppleClang 21,
CMake Release, no native tuning, PGO or LTO. These are synthetic prepared-depth
microbenchmarks, not network-to-decision or exchange latency.

| Scenario | Bounded exact median/call | Exhaustive median/call | Quantities evaluated by bounded search |
|---|---:|---:|---:|
| Interior profitable size | 0.827 µs | 307.858 µs | 16 |
| Fractional grid, 32 levels per leg | 19.313 µs | 1,988.725 µs | 506 |
| Funding-constrained size | 0.683 µs | 304.688 µs | 14 |
| Zero gross edge, zero fee | 0.010 µs | 207.558 µs | 0 |

These ratios compare equivalent exact algorithms, **not the old strategy**.
The old strategy asks a simpler question and can miss profitable smaller sizes.
The exact solver allocates no heap memory; the caller prepares/reuses contiguous
depth buffers. Worst-case rounding can prevent pruning, so the explicit budget
and incomplete status remain necessary. No universal microsecond deadline follows
from these four fixtures.

As a compatibility check, six alternating old/new binary pairs, 50 measured
whole-study runs per process with three warmups, retained identical full-output
digests for the legacy policy on the 96-record REST pilot. Median per-process
p50 was 3.952 ms before and 3.927 ms after: effectively unchanged. Both the legacy
and new policy still find zero attempts on that limited pilot. A separate six-pair comparison on the current binary measured 3.954 ms for the
legacy policy and 3.898 ms for the new policy: no material regression in this
rejection-heavy case. The new policy uses a proven nonpositive-gross-edge filter
and aggregates search counters instead of formatting JSON for every rejected
search. This result does not predict cost on opportunity-rich captures.

Raw sizing CSV, paired study CSVs, source copies/diff, input policies and binary
hashes are retained in local `calci-sizing-20260915` artifacts. Reproduce with
`eme_sizing_benchmarks` and `eme_study_benchmarks`; CI checks the same algorithm on
MSVC/x64, GCC/x64 and ARM64, and Clang/x64 and ARM64. Local measurements do not
establish performance on those other architectures or a hardware recommendation.

## Bounded background capture (2026-09-15)

`eme_capture_benchmarks` measures synchronous journal append against ownership
handoff to the background writer. Both use the reusable encoding buffer; both
must produce identical journal hashes. Payload creation and session/thread
construction are outside timing. The producer distributions measure the append
call; total time also includes drain and verified finalization. Eight alternating
pairs per scenario, Apple M2 Pro, AppleClang 21, CMake Release without native
tuning. No builds or tests ran concurrently with this measurement.

| Arrival pattern / payload | Synchronous producer p99 | Background producer p99 | Synchronous total | Background total |
|---|---:|---:|---:|---:|
| Burst, 128 B | 9.459 µs | 0.209 µs | 12.197 ms | 12.015 ms |
| Burst, 512 B | 11.854 µs | 0.125 µs | 34.323 ms | 34.149 ms |
| Burst, 4,096 B | 56.250 µs | 0.083 µs | 235.221 ms | 233.603 ms |
| Scheduled 100 µs, 128 B | 56.542 µs | 7.562 µs | 53.326 ms | 53.202 ms |
| Scheduled 100 µs, 512 B | 28.271 µs | 8.438 µs | 54.624 ms | 54.749 ms |
| Scheduled 100 µs, 4,096 B | 93.146 µs | 9.917 µs | 72.164 ms | 71.808 ms |

Values are medians of each run's percentile/total. Bursts contain 4,096 records;
scheduled cases contain 512 and their total includes the arrival schedule. Raw
CSV also records start lateness: its p99 was roughly 44–64 µs in scheduled cases,
so these are ordinary OS scheduling measurements, not perfectly timed arrivals.
The schedule is fixed before work; delays do not reset it or hide overdue inputs.
Single-call burst medians were close to timer overhead and should not be treated
as precise nanosecond guarantees.

This moves storage stalls off the producer and improves the measured tail. It
does **not** materially speed up complete storage/verification. At low message
sizes, waking the worker can cost more than the typical synchronous call: for
scheduled 128 B messages p50 went from 0.855 to 1.271 µs, while p99 improved.
The synchronous path remains available. CPU/scheduler and buffering costs must
be considered when choosing the future live recorder's budget.

Burst tests deliberately allow the whole burst: 4,096 slots and 64 MiB retained
budget. Observed high-water marks ranged from about 1.05 to 17.32 MB; paced cases
used far less. These settings are test assumptions, not new default production
limits. Saturation aborts a capture instead of silently dropping messages, and
finalization is not a power-loss durability guarantee.

The journal codec also reuses its encoding storage. Generated buffer tests and
an old/new binary fixture comparison preserve exact bytes and checksums. Local
ThreadSanitizer passes the capture ownership suite; CI now carries that check.
Raw CSV, hashes, source copies, old/new fixtures and build provenance are in the
local `calci-capture-20260915` artifacts. No live feed or economic return was
measured in these persistence tests.

## Read-only controller decode path (2026-09-15)

`eme_feed_benchmarks` measures strict WS subscription routing, JSON decoding,
normalization and application of 10,000 prepared consecutive deltas. Construction,
TLS/network I/O, journal writing, candidate evaluation and sizing are excluded.
Each run checks the exact final quantity (10,100 centicontracts) and original wire
sequence (10,001). The optimization shares the first decoded JSON tree with the
gateway and removes the full raw-record copy; it does not relax validation.

M2 Pro, AppleClang 21, CMake Release, portable default flags: eight alternating
before/after process pairs, each with eight measured 10,000-update runs. Medians
of run distributions, with compilation/tests stopped during the final campaign:

| Path | Per-update p50 | Per-update p99 | 10,000-update total |
|---|---:|---:|---:|
| Initial controller, copy + second JSON parse | 3.917 us | 7.917 us | 41.604 ms |
| Shared parsed input | 2.583 us | 3.771 us | 27.299 ms |

This workload reduces median update/total CPU time by about 34%. Tails include OS
scheduling/allocator variation; it is not a venue latency measurement, an economic
return, or a cross-hardware speed guarantee. The earlier capture benchmark still
owns persistence measurements. Local CSVs, source snapshots and executable hashes
are preserved in the task's `calci-live-20260915` artifact directory.
