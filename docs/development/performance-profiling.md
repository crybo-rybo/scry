# Development: Performance Profiling

Performance work in Scry is evidence-driven. A benchmark identifies a cost at a
sanctioned seam, a production change addresses that cost, and the same workload
shows whether the change helped without moving the cost elsewhere. Benchmark
numbers are review evidence, not portable product promises or absolute CI
thresholds. This policy is the review enforcement for `SCRY-QA-014` and remains
consistent with the behavioral-gate rule in `SCRY-QA-007`.

## Scope and Build Boundary

The profiling suite is an opt-in development tool:

- `SCRY_BUILD_BENCHMARKS` defaults to `OFF`. A normal configure does not fetch,
  discover, build, or run benchmark dependencies.
- The `profile` preset is a `RelWithDebInfo` build with benchmarks enabled and
  tests, examples, reflection, diagnostic logging, and sanitizers disabled.
  Sanitized or debug builds are correctness tools and never provide reported
  performance numbers.
- `scry_timing_benchmarks` measures execution time and throughput.
- `scry_allocation_benchmarks` measures C++ allocation calls and requested C++
  heap bytes. Timing claims never come from the instrumented allocation binary.
- Benchmark targets and dependencies are private to the build tree. They are
  never installed, exported, linked by `scry::scry`, or exposed to downstream
  package consumers.

The executables accept normal Google Benchmark flags and can emit raw Google
Benchmark JSON.
Google Benchmark provides calibration, warm-up, repetitions, optimizer
barriers, counters, filtering, and structured output; the dependency and pin
are recorded in the
[dependency architecture](../architecture/dependencies-and-errors.md).

The local entry points are:

```sh
just profile
just profile-smoke
just profile-dry
just profile-compare BASE HEAD [OUT]
just profile-pair BASE_CHECKOUT HEAD_CHECKOUT [OUT]
```

`profile` configures and builds the preset, then records the full suite under
`build/profile-artifacts/<UTC timestamp>-<short SHA>` unless `--output` is
supplied. `profile-smoke` runs every scenario once with a minimal duration.
`profile-dry` configures and builds both executables, lists their benchmark
identities, and writes valid zero-match raw JSON plus an empty summary without
executing workload iterations. Arguments after the run commands are forwarded
to the profiling driver. The comparator consumes two already-recorded artifact
directories; it does not claim that unlike-host results have become comparable.
The full driver is useful for local characterization, but its repetitions run
inside each benchmark process. It does not by itself satisfy the fresh-process
`A-B-B-A` review protocol below. `profile-pair` is the review-evidence driver:
it builds the two checkouts in distinct build trees, then uses the HEAD
checkout's scripts as one common orchestrator. Its default five cycles create
ten one-repetition, fresh-process samples for each revision. For a quick
diagnostic, pass `--mode smoke --cycles 1`; the resulting report is explicitly
not evidence-eligible.

The runner owns Google Benchmark's output, filter, repetition, duration,
warm-up, interleaving, context, and list/dry-run flags. A matching
`--benchmark-arg` is rejected rather than silently changing the recorded
measurement policy. Non-reserved Google Benchmark arguments are recorded
verbatim in the environment and must match before comparison.

## Scenario Contract

Scenario names describe observable work rather than the current implementation.
The initial families use the stable prefixes `SSE/`, `OpenAIStream/`,
`RequestEncoding/`, `TurnMachine/`, `Pump/`, `Admission/`, and
`ToolRegistry/`. A scenario is identified by:

| Field | Meaning |
|---|---|
| `scry_benchmark_schema` | Version of the workload semantics and correctness oracles; initially `1` |
| `scenario_name` | Exact Google Benchmark name, stable and independent of implementation type names |
| `parameters` | Ordered fixture dimensions such as bytes, chunk size, dialect, messages, schemas, calls, or routes |
| `scry_fixture_seed` | Fixed fixture seed; initially `0` for deterministic index-generated data |
| `scry_measurement_mode` | `timing` or instrumented `allocation_pressure`; optional RSS is separate process evidence |

Parameters are part of the identity. A result for 32 messages cannot be compared
with one for 256 messages even when the scenario name is the same. Adding a new
parameter point does not change existing identities. Changing input meaning,
timed boundaries, fixture construction, output validation, or a scenario's unit
of work requires a `scry_benchmark_schema` increment. Changing an artifact
envelope or its compatibility rules requires that artifact's schema increment.

A benchmark modification that materially changes a scenario lands before the
production optimization it is meant to measure. Both candidate implementations
are then rerun with the new scenario. Results from incompatible scenario
schemas are never compared.

Every scenario MUST:

- build deterministic fixtures outside the measured region;
- warm process-global or dependency state before measurement;
- keep I/O, real sleeps, model inference, and external network access out of
  deterministic workloads;
- validate the semantic result and a stable digest outside the timed region;
- consume measured results so the optimizer cannot discard the work;
- state its logical operation and report `input_bytes`, `output_bytes`, and
  `items`; and
- use application callbacks, handlers, and fake transports that do no work
  beyond the minimum checksum needed to retain observable behavior.

Timing pauses around fixture reset, allocation, and correctness validation. A
benchmark may use private headers at an established internal seam, but it does
not create a new public API or make a private representation a compatibility
contract. The 64-bit semantic FNV-1a digest is exported losslessly as
`checksum_hi` and `checksum_lo` counters so the result artifact carries its
correctness oracle without depending on floating-point integer precision.

## Baseline Workloads

The foundation covers parameter curves rather than one favorable large case:

| Area | Representative dimensions | Primary question |
|---|---|---|
| SSE parsing | LF and CRLF; whole input, 4 KiB, 64-byte, and one-byte chunks | Does parser cost scale with bytes and events rather than chunk fragmentation? |
| OpenAI streaming | Text deltas and 1/8/32 interleaved tool calls | Where do fragment accumulation, lookup, and metadata copies cost time or allocations? |
| Request encoding | Both dialects; 0/32/256 prebuilt messages; 0/16/64 prebuilt schemas | How much do representative histories and schemas cost to encode and serialize after snapshot construction? |
| Tool round | 1/8/32 calls with representative argument and result sizes | Are canonicalization and machine transitions copying or parsing more than once? |
| Pump delivery | 64-route text delivery and 64-route completion commit | What do event movement, route lookup, callback delivery, and commit cost? |
| Turn admission | 63 queued turns with equal 128 KiB message-text payloads shaped as 32, 256, or 2,048 messages; 8/32/64 shared 2 KiB schemas | What allocation and latency cost is retained while accepted turns wait behind one blocked active turn? |
| Tool registration | Additive batches of 8/32/64 representative 2 KiB schemas | Does moving snapshot work into registration improve the full tradeoff rather than hiding cost? |

The suite may add two corroborating macro workloads without turning them into
microbenchmark gates:

- A deterministic full turn through the real worker and pump with a
  benchmark-local, non-copying fake transport.
- Repeated Curl transfers against a local loopback server. Curl results use
  wall time and connection/request counts and remain observational because
  kernel and server scheduling dominate small differences.

The admission workload parks one active turn in a stop-aware, non-copying fake
transport and queues the remaining 63 turns allowed by the default limit. Its
equal-logical-byte history shapes expose per-message ownership overhead.
Schema evidence combines the registration batches with repeated accepted-turn
snapshots so an optimization cannot hide send cost by moving it entirely into
registration. A validation-only warm-up releases the parked worker, captures
every queued provider request, and compares its encoded body with an
independently constructed expected request; draining, capture, and hashing stay
outside timing and allocation scopes.

## Metrics and Measurement Modes

### Timing

Pure parser, provider, encoding, machine, and registration workloads use CPU
time. Threaded pump, admission, and Curl workloads use wall time. Reports
include:

- median nanoseconds per logical operation;
- operation-specific throughput, such as MiB/s, events/s, fragments/s, or
  callbacks/s;
- input and output bytes; and
- sample count and dispersion.

Pure samples self-calibrate after warm-up. Pump samples batch logical
operations until scheduling noise is small relative to the sample duration;
admission uses one fixed 63-send retained batch per repetition so its unit and
memory pressure cannot drift during calibration.

### C++ allocations

The allocation executable reports these counters per logical operation:

- `cpp_allocations`: calls through the instrumented global C++ allocation
  forms; and
- `cpp_requested_bytes`: bytes requested through those allocation calls.

The tracker is benchmark-only, cross-thread safe, and inactive during fixture
construction. These counters cover Scry, Glaze, and standard-library C++
allocations routed through the replaced operators. They do **not** cover
libcurl's internal `malloc` calls, allocator metadata, resident pages, or the
configured logical payload limits. They must be labeled as C++ allocation
metrics, never as total process memory.

### Process memory

When resident-memory evidence is captured, it runs one filtered scenario per
fresh process because peak RSS is a process-lifetime high-water mark. Reports
normalize all values to bytes and record:

- RSS before the workload;
- RSS after the named retained-state checkpoint; and
- peak RSS for the process.

RSS is supplementary for microbenchmarks and primary only for retained-state
workloads large enough to exceed allocator noise. Results are comparable only
on the same operating system, host, allocator, compiler, standard library, and
build configuration. Native tools such as heaptrack or Instruments may explain
an observation, but their numbers are diagnostic artifacts rather than a
cross-platform acceptance metric. The initial portable harness reports C++
allocation calls and requested bytes; it does not automate RSS collection, so
no pull request may imply that those C++ counters cover total process memory.

## Result Metadata and Compatibility

Raw Google Benchmark output is retained. The project-owned artifact schemas are
versioned independently:

- `environment.json` uses `scry.performance.environment.v1`;
- `summary.json` uses `scry.performance.summary.v1`;
- paired `manifest.json` uses `scry.performance.pair-manifest.v1`; and
- paired `comparison.json` uses `scry.performance.comparison.v1`, accompanied
  by a human-readable `comparison.md`.

The environment and summary record at least:

- source commit and dirty state;
- artifact schema, `scry_benchmark_schema`, exact scenario name/parameters, and
  `scry_fixture_seed`;
- a run identifier carried by the environment and both executables' raw Google
  Benchmark contexts, plus corroborating host, CPU-count, and target identity;
- a methodology digest covering benchmark sources (including their CMake
  definition), the profiling runner, comparator, and pair orchestrator;
- the common HEAD tooling commit and dirty state;
- compiler identity/version, standard library, build type, and effective flags;
- operating system/kernel, architecture, CPU model, and logical CPU count;
- allocator when known;
- the CMake-resolved libcurl version/library and the actual Glaze and Google
  Benchmark checkout commits, each asserted against its configured pin;
- measurement mode, execution order, warm-up policy, and sample count; and
- semantic output digest plus input/output dimensions.

The comparator refuses results with mismatched artifact or benchmark schemas,
methodology digests, common tooling revisions, scenario sets/parameters,
compiler or standard library, normalized compile-command fingerprints, build
flags, dependency checkouts, operating system, architecture, or host identity.
Compile commands are grouped into stable production and benchmark scopes; file
names and the number of translation units do not affect the fingerprint when
their effective flag sets are unchanged. For directory artifacts, the
comparator independently reloads `environment.json`, regenerates the raw-file
and scenario summaries from `raw/*.json`, and requires exact agreement with
the cached `summary.json`. It also verifies raw-file hashes and the
run/host/CPU/target binding before aggregation. A human may inspect unlike
results, but the tool does not present their ratio as evidence.

Dirty source or tooling artifacts are rejected by default. `--allow-dirty` is
available only for local diagnostics; it forces `evidence_eligible` to `false`
and emits a prominent warning in Markdown. Direct `--base`/`--head`
comparisons remain supported, including repeated arguments for multiple
artifacts, but without an ordered manifest they are diagnostic rather than
review evidence.

## Baseline and Paired Comparison Protocol

The profiling-foundation commit is the reproducible baseline; checked-in
machine-specific timing numbers are not. Before evaluating optimizations, run
the baseline against itself to establish the A/A noise floor for every target
scenario.

A reported comparison follows this protocol:

1. Build parent (`A`) and candidate (`B`) in separate clean build directories
   with identical effective configuration.
2. Run on the same otherwise-idle host, toolchain, allocator, and power mode.
3. Warm each executable before taking samples and run scenarios serially.
4. Use five `A-B-B-A` cycles, producing ten samples for each revision. For
   worker/pump or Curl claims, repeat the result in an independent session when
   no dedicated stable runner exists.
5. Report each revision's median and normalized median absolute deviation
   (`MAD / median`), the candidate/parent ratio, and a fixed-seed bootstrap 95%
   interval for that ratio.

`scripts/perf-pair.sh` automates this order and writes an ordered manifest that
binds every ordinal, cycle, revision, artifact, source commit, tooling commit,
and run identifier. The comparator requires exactly four entries in `A-B-B-A`
order per cycle and exactly ten samples per revision before declaring a clean,
full-mode report protocol-eligible. The equivalent direct command is:

```sh
./scripts/perf-pair.sh \
  --base-dir /path/to/parent \
  --head-dir /path/to/candidate \
  --output build/profile-pair
```

Each optimization PR compares its head with its immediate parent so its
marginal effect is visible. It also reports the affected representative
scenarios cumulatively against the profiling-foundation commit. If a candidate
is dropped, later work is restacked on the last accepted parent; a benefit is
not credited twice through a rejected ancestor.

The manually dispatched performance workflow uses the same paired driver, but
passes an explicit shared-runner informational reason, so its report remains
ineligible for a performance claim even when all five cycles complete. Hosted
shared-runner results may build confidence and produce artifacts, but their
absolute timings never gate a pull request. A future stable dedicated
runner may support scenario-specific regression gates only through the
[evolution rule](../architecture/quality-and-evolution.md).

## Interpreting an Optimization

A performance or memory claim is ready for review only when:

- all correctness, sanitizer, package, and documentation gates pass;
- exact semantic output remains unchanged;
- the target median improves by at least the greater of 5% or three times that
  scenario's measured A/A normalized MAD;
- the bootstrap interval is entirely favorable;
- a memory claim improves a representative allocation, requested-byte, or RSS
  metric by the same practical/noise threshold and by a meaningful absolute
  amount; and
- representative guardrail scenarios show no confirmed regression beyond
  their own practical/noise threshold.

The threshold is a review heuristic, not a mechanical timing gate. A smaller
result may still justify a uniquely simple change; a larger microbenchmark win
may still be rejected when it adds disproportionate ownership or lifecycle
complexity, worsens retained memory, disappears in a relevant seam/macro
workload, or only benefits an unrealistic fixture. The pull-request rationale
records that judgment.

Performance evidence is rejected or the change is redesigned when it is inside
the noise band, moves cost to another stage, harms a representative guardrail,
or stops helping after an earlier stack optimization. This is the practical
application of `SCRY-QA-014`: measurements select the changes; they do not
decorate changes selected in advance.
