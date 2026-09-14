# Performance engineering

This document is the maintained performance plan and curated research index.
It complements [the implementation plan](IMPLEMENTATION_PLAN.md), rather than
replacing the existing economic direction. No local timing is a claim about
exchange latency, execution success or profitability.

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

All data is synthetic, with a fixed seed and workload version printed in CSV.
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
