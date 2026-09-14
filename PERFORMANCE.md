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

## Measurement log

Initial comparison pending. Record source revisions, machine, flags, repeated-run
results and limitations here when the first experiment is complete. Raw captures
and machine-specific experiment directories stay outside Git.
