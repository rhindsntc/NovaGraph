# Benchmarks and Measurement Limits

NovaGraph includes development benchmarks and diagnostic tools. Results depend on the exact source revision, build, workload and machine. No published throughput, memory, latency or physical-device guarantee is established.

## Swift workload smoke checks

`make benchmarks` runs a synthetic 2,000-node, 1,999-edge workload with eleven reported metrics. `make benchmarks-test` runs a small smoke workload and validates argument failures. Some percentile fields have only one sample, including checkpoint, trim and reopen; do not interpret those as a latency distribution.

The final phase measures a clean reopen, a point read and handle destruction, including close checkpoint. It does not measure process restart or uncheckpointed crash recovery. Runtime errors exit nonzero rather than printing a completion banner.

## Reproducible workload measurements

```sh
make performance-smoke PERFORMANCE_OUTPUT=build/performance-smoke-new
make performance-measure PERFORMANCE_OUTPUT=build/performance-desktop-new
make performance-ios PERFORMANCE_OUTPUT=build/performance-ios-new
```

Each output directory must be new. The harness runs seeded graph families in fresh processes, checks traversal identities against SQLite and an in-memory reference, and separates trim, checkpoint, clean-open and close samples. Failed outputs remain available for inspection. Starting budget definitions live in `benchmarks/budgets.json`; passing them on one machine is not a general product guarantee.

Protocol 2 measures all five graph families independently. A full timing run uses 20 fresh processes per family, 20 warmups and 100 samples for reads, traversal and mutations. Trim, checkpoint and clean-open each contribute one sample per process. Traversal uses depth two and a maximum of 100 results. Durable mutations use a separate 250-record database: one existing scalar record per single write, or exactly 250 updates per transaction. Construction, outcome validation and final durable reopen stay outside each timed interval. Full trim drains cold records in the background; its budget is not a frame deadline.

| Limit | Watch, 250 nodes | Phone, 1,000 nodes | Desktop, 10,000 nodes |
| --- | ---: | ---: | ---: |
| Hot-read p95 | 10 ms | 5 ms | 2 ms |
| Cold-read p95 | 50 ms | 20 ms | 10 ms |
| Traversal p95 | 20 ms | 10 ms | 5 ms |
| Single durable mutation p95 | 50 ms | 25 ms | 10 ms |
| 250-mutation transaction p95 | 250 ms | 100 ms | 50 ms |
| Full-trim p95 | 2 s | 5 s | 20 s |
| Checkpoint / clean-open p95, each | 0.5 s | 1 s | 2 s |
| Peak process RSS | 64 MiB | 128 MiB | 512 MiB |
| Sampled logical disk | 4 MiB | 16 MiB | 128 MiB |
| Sampled allocated disk | 16 MiB | 48 MiB | 384 MiB |
| Sampled file count | 2,000 | 8,000 | 70,000 |

Peak process RSS is a measured acceptance limit for the specified workload, not an enforced runtime allocation cap. Checkpoint and reopen must satisfy it for production qualification; see the [first-release memory policy](maintenance.md#first-release-memory-policy).

These are initial acceptance targets, not published performance claims. Simulator measurements do not qualify a physical phone or watch. The p99 field is descriptive: this sample count does not establish reliable tail latency. Historical reports remain readable, but reports without the full version 2 inventory cannot qualify the current limits.

## Initial desktop budget acceptance

Two independent collections on October 2 and 5, 2026 provide an accepted initial performance reference for the specified desktop workload. Both used clean revision `b9e97c3b7bddb8401e7d4772d2cca38a9c4fde12`, native protocol 2, seed 42, five 10,000-node graph families and 20 processes per family. The environment was arm64 M4 Max, macOS 27.0.1, Xcode 27.0 (27A266a), Apple Clang 21 and macOS SDK 27.0. Each sampled operation used 20 warmups and 100 measurements per process; lifecycle operations contributed one observation per process.

All 200 processes met every operation p95 and peak-RSS cap. The table reports the largest pooled family p95 in each collection; it is not a reliable p99 estimate or a guarantee for other workloads.

| Operation | Collection A p95 | Collection B p95 | Desktop cap |
| --- | ---: | ---: | ---: |
| Hot read | 0.002 ms | 0.002 ms | 2 ms |
| Cold read | 4.769 ms | 5.403 ms | 10 ms |
| Traversal | 0.200 ms | 0.196 ms | 5 ms |
| Single durable mutation | 4.193 ms | 5.036 ms | 10 ms |
| 250-mutation transaction | 7.532 ms | 7.662 ms | 50 ms |
| Full trim | 9034.586 ms | 12598.225 ms | 20000 ms |
| Checkpoint | 762.345 ms | 755.027 ms | 2000 ms |
| Clean open | 1746.224 ms | 1723.130 ms | 2000 ms |

Peak process RSS was 376.391 MiB in A and 377.406 MiB in B, below the 512 MiB cap.

This acceptance has an explicit consistency limitation: some same-build process-summary comparisons exceed the 10% bidirectional rule. The first collection also retains its original failed postflight load result, with a separately reviewed exception for that observation; the second passed its original controls and absolute evaluation. The initial reference is accepted for budget assessment, without claiming repeatability or a passed candidate regression comparison.

Original reports and failures are retained privately. The strict automatic gate below is unchanged and cannot freeze this pair; no compatible controlled baseline is shipped. Future changes still require absolute-budget checks and explicit regression review. A passing repeatability pair and candidate comparison are deferred from this initial engineering acceptance. Physical-device/minimum-runtime qualification and sustained memory-growth/release checks remain open; NovaGraph remains experimental.

## Controlled desktop regression gate

The local gate separates build preparation, collection, explicit baseline review and candidate comparison. It requires a clean checkout, matching hardware/OS/compiler/SDK/build flags, a stable non-sensitive runner name and the same budget/protocol fingerprints. Engine implementation may change between baseline and candidate; the measurement harness may not.

```sh
python3 tools/performance_gate.py prepare --output build/controlled-build-new
python3 tools/performance_gate.py collect --prepared build/controlled-build-new --output build/baseline-a-new --runner-id desktop-01 --operator operator --attest-quiet --attest-ac-power --attest-thermal
python3 tools/performance_gate.py collect --prepared build/controlled-build-new --output build/baseline-b-new --runner-id desktop-01 --operator operator --attest-quiet --attest-ac-power --attest-thermal
python3 tools/performance_gate.py review --reports build/baseline-a-new/results.json build/baseline-b-new/results.json --output build/baseline-manifest-new.json --reviewer reviewer --approval-note "Reviewed both complete runs and their control evidence" --approve
```

Both baseline runs must use the same frozen revision, pass every absolute limit and agree within 10% in both directions. Failed runs remain evidence; do not pick the fastest run or promote a baseline automatically. Review hashes the reports and records the explicit reviewer action. These hashes detect changed files; they are not a trusted signature or proof of a reviewer's identity.

After preparing and collecting a new candidate with the same procedure:

```sh
python3 tools/performance_gate.py gate --candidate build/candidate-new/results.json --baseline build/baseline-manifest-new.json --output build/gate-new.json
```

A candidate must pass absolute limits and compare within 10% against **both** frozen runs, using both the median and p95 across per-process p50 and p95 values. Without a frozen baseline, absolute acceptance does not become a regression pass. The gate rejects incomplete, dirty, simulator, smoke, mismatched or altered evidence and baselines that already fail the absolute limits.

Collection holds a machine-wide lock and records power, thermal, process and load observations before and after the window. Preparation and ordinary native measurement commands also respect that lock. Keep the Mac on AC power and stop builds, tests, simulators and other heavy work for the full run. The explicit operator attestations are necessary because endpoint observations cannot prove the entire window stayed quiet. A complete collection can take 45 minutes or longer; prepare outside the quiet window. Raw local evidence contains machine/process metadata and belongs in ignored `build/` directories, not the public documentation. No controlled baseline is shipped with these tools. The immediate postflight one-minute load average can still include the benchmark's own completed work. A rejected load check does not prove unrelated CPU activity or an engine regression; the run remains ineligible even when all observed operation and RSS values are below their limits. Retain the failure and review the control policy before repeating long collections.

## Activity diagnostics

Use this short diagnostic to investigate system activity and timing variability before spending time on a full baseline pair. Prepare the build first, then coordinate an idle window of roughly 7–10 minutes. Keep the laptop on AC power with the lid open and pause other work until recovery finishes.

```sh
python3 tools/performance_gate.py prepare --output build/diagnostic-build-new
caffeinate -i python3 tools/performance_gate.py diagnose --prepared build/diagnostic-build-new --output build/activity-new --attest-quiet --attest-ac-power --attest-thermal
```

The fixed sequence is 120 seconds idle, navigation/recommendations alternating three times each, then 120 seconds recovery. Each native process uses 10,000 nodes, seed 42, 100 measured samples and 20 warmups. The command holds the existing machine-wide measurement lock and requires matching clean source, build and environment provenance. A failed readiness check after initial idle prevents benchmark launch. Preparation must be repeated after changing the measurement tools or environment; old evidence retains its original identity.

`diagnostic.json` records raw-file hashes, endpoint power/thermal checks, each native process identity and timing summaries. `activity.jsonl` records system load and process CPU activity every two seconds. CPU attribution separates the launched Nova executable, the collector and other processes using PID, process start time and executable identity; matching a process name alone is insufficient. Full command arguments are not collected. Raw process paths and machine metadata are private: retain them only in ignored `build/` output directories, never import them into the public site.

The activity summary exposes sampling errors, gaps longer than three seconds, process-identity changes and monitoring overhead. CPU deltas only cover identities present in consecutive valid snapshots within the same phase. Boundary intervals, newly observed and exited processes, and ownership transitions cannot be fully attributed. CPU seconds are not percentages of the whole machine. Other processes may be reacting to Nova; their activity does not prove independent interference or explain a particular operation's latency. There is no per-process physical I/O attribution or per-operation wall-clock timeline.

Timing spread is the maximum versus minimum per-process p50/p95 across at most three processes per family; it is **not** the full paired repeatability test. The two-second sampler perturbs the workload. Load averages include recent work, including Nova itself. Endpoint checks do not establish continuous power or thermal conditions. “Cold reads” means records were evicted from Nova's payload cache; the operating system's filesystem cache is uncontrolled.

These reports always have `complete: false`, `hardwareQualified: false` and `qualification: ineligible-diagnostic`. `diagnosticComplete: true` means the diagnostic sequence and activity coverage completed, not that performance qualified. Errors, timeouts, Ctrl-C, SIGTERM and SIGHUP preserve partial evidence and stop the active benchmark process group; forced SIGKILL or power loss cannot guarantee finalization. A nonzero exit means incomplete diagnostic collection. Both the controlled gate and general timing evaluator reject activity diagnostics, even if their saved pass flags are edited. Existing absolute budgets, immediate endpoint checks and the 10% bidirectional repeatability rule are unchanged. No baseline is approved automatically.

## Trim profiling

```sh
make cpp-build
clang++ -std=c++20 -O3 -I cpp/include tools/profile-trim.cpp build/cpp/libnovagraphdb.a -o /tmp/nova-profile-trim
/tmp/nova-profile-trim 2000
```

This native diagnostic seeds a graph in the real hot store and times forced trim through the file backend. It reports elapsed time, evicted/unmet bytes and I/O counters. Dataset construction and cleanup are outside the interval. A grouped `fileSync` count represents a group, not one syscall. Use repeated controlled samples; see [maintenance](maintenance.md) for what trim accounts for and what can remain resident.

## Swift operation diagnostics

`python3 tools/swift-measurements.py --output build/swift-measurements-new` compares C transport, C transport plus Foundation envelope decoding, and the typed Swift API using repeated hot reads at several payload sizes. Run from a clean checkout and use a new output directory. The collector retains build and environment details and rejects incomplete samples or changed inputs.

The original hot lanes rotate their execution order and remain comparable to historical reports. New reports also include trim-backed cold reads, durable single writes and exact 250-record transactions, depth-two traversal, and async read/traversal/write calls measured through completion. Separate fixtures validate their results and durable reopen. Swift mutation fixtures replace a string payload and integer property; they deliberately differ from the native scalar-write fixture.

The lanes include different work, so their ratio is not pure language overhead. Raw reports and their summaries remain diagnostic. A separate evaluator applies the explicitly approved Swift fixture caps from `benchmarks/budgets.json` (reviewed October 1, 2026):

| Swift lane | p95 cap |
|---|---:|
| Typed hot read and async hot read | 2 ms |
| Cold read | 10 ms |
| Sync and async traversal | 5 ms |
| Sync and async durable single mutation | 10 ms |
| Transaction of exactly 250 mutations | 50 ms |

Every lane must pass at 64, 512 and 1,024 payload bytes. Both pooled p95 and p95 across the 20 independent-process p95s must meet the cap; pooling cannot hide slow processes. These limits cover the existing macOS Release Swift fixture, including its 32-node traversal ring, rather than the native 10,000-node graph. They do not qualify physical devices or establish a controlled regression baseline. Historical hot-read-only reports cannot pass this evaluator. Comprehensive Swift allocation accounting remains open.

```sh
python3 tools/resource_budgets.py --revision FULL_COMMIT_SHA --source-digest SWIFT_SOURCE_DIGEST --output build/swift-gate-new.json swift build/swift-measurements-new/results.json
```

Use the expected clean candidate revision and Swift collector source digest. The evaluator rejects dirty, incomplete, foreign or mismatched fixture evidence and leaves the raw diagnostic report intact.


## Swift fixture budget result

A quiet-window collection from clean revision `448a13e6a3663826636a2d4406f22e00da75520f` passed every approved Swift fixture cap on October 1, 2026. All 20 independent Release processes completed, retaining 100 measured samples per lane and payload in each process (2,000 per lane and payload). Every payload size passed both pooled p95 and p95 across process p95s.

| Swift lane | Largest checked p95 (ms) | Cap (ms) |
|---|---:|---:|
| Typed hot read | 0.174 | 2 |
| Async hot read | 0.086 | 2 |
| Cold read | 0.147 | 10 |
| Sync traversal | 0.154 | 5 |
| Async traversal | 0.160 | 5 |
| Durable sync single mutation | 6.737 | 10 |
| Durable async single mutation | 5.449 | 10 |
| Exactly 250-mutation transaction | 28.597 | 50 |

The table takes the largest checked p95 across the three payload sizes and both populations; values are rounded for display, while the gate uses full precision. Raw reports retain their diagnostic-only summaries and the separate budget result records the pass. This verifies the approved macOS Swift fixture caps; it does not freeze a controlled native regression baseline or qualify physical devices.

## Resource diagnostics

`make performance-resources PERFORMANCE_OUTPUT=build/resources-new` records desktop resource diagnostics. For the simulator, use `python3 tools/performance.py run --mode resources --profile phone --platform ios-simulator --output build/resources-ios-new`. Resource mode rejects timing baselines and cannot pass latency/RSS budgets.

Resource protocol 2 retains the original direct-operation phases and adds mutation phases. Separate DSL traversal/checkpoint probes record reservation usage or admission rejection. They include parser/serialization work and do not establish workspace usage for the direct traversal/checkpoint API, close, open/recovery or setup index construction. Unobserved workspace stays unavailable.

Disk acceptance uses the maximum across all seven lifecycle snapshots for each family, including earlier snapshots that exceed the final size. Unavailable filesystem allocation data remains unqualified. It does not establish a transient peak or bound long-term growth.

```sh
python3 tools/resource_budgets.py --revision FULL_COMMIT_SHA --source-digest RESOURCE_SOURCE_DIGEST --output build/disk-gate-new.json disk build/resources-new/results.json
python3 tools/resource_budgets.py --revision FULL_COMMIT_SHA --source-digest ARTIFACT_SOURCE_DIGEST --output build/distribution-gate-new.json distribution --bundle path/to/bundle --archive path/to/bundle.zip
```

Use the expected clean candidate's provenance, not another revision's results. Resource and artifact builders hash different input sets, so their source digests are distinct. The distribution gate revalidates the full eight-slice bundle's ABI/platform matrix, file hashes, ZIP contents and checksum before applying 12 MiB for the ZIP and 40 MiB for the extracted bundle excluding `artifact-manifest.json`. These are distribution limits, not installed or linked app size. Passing resource or distribution limits does not qualify timing or physical-device behavior.

## Linked-app size snapshot

A matched minimal SwiftUI app built with and without GraphDBKit adds about **0.9 MiB** for the tested integration. Both variants use the same source and Release settings, arm64, dead-code stripping and stripped unsigned products. The Nova variant exercises create, write, read, close and reopen so the engine is actually linked.

| Platform | Baseline app bytes | App with Nova bytes | Added bytes |
|---|---:|---:|---:|
| macOS | 56,793 | 993,705 | 936,912 (0.894 MiB) |
| iOS | 72,792 | 1,005,608 | 932,816 (0.890 MiB) |

Measured from clean source revision `47dc5b9c0d5a0ff44f775e2f97dc64958752430b` with Xcode 27.0 (27A266a), SDKs 27.0 and minimum deployment targets macOS 13.0 / iOS 16.0. The executable-size increases equal the app-bundle increases in this pair. macOS runtime create/read/close/reopen checks passed; iOS is an unsigned device-target build, not a device runtime test.

A fresh collection from clean revision `e4032bd5c129946518d00049fcf186960389cccf` passed the approved size gate on October 1, 2026: macOS added **936,960 bytes (0.894 MiB)** and iOS added **932,872 bytes (0.890 MiB)**. The collector verified the consumed Xcode projects, resolved Release settings, successful stripping actions, actual bundle hashes and distinct macOS baseline/Nova probes. Xcode 27.0 (27A266a), SDKs 27.0; iOS remains an unsigned device build without runtime qualification.

These are uncompressed app-bundle logical bytes for one integration, excluding debug symbols, signing and user database files. Different API usage, assets or build settings can change the result. This is separate from distribution archive size and App Store download/installed size. The approved cap is **2 MiB (2,097,152 bytes) added per platform** for this matched minimal app pair. The historical snapshot above predates the reproducible collector and is not a current budget pass.

From a clean checkout on macOS with Xcode and XcodeGen installed:

```sh
python3 tools/linked_app_size.py --output build/linked-app-new
python3 tools/resource_budgets.py --revision FULL_COMMIT_SHA --source-digest LINKED_APP_SOURCE_DIGEST --output build/linked-app-gate-new.json linked-app build/linked-app-new/results.json
```

The collector retains all four builds (baseline/Nova for macOS/iOS) using `examples/LinkedAppSize/App.swift`. The gate checks the matched source and project settings, binary architecture/deployment platform, file inventory and hashes before recomputing each bundle contribution. macOS executes the durable reopen probe; iOS remains build-only. Keep the app bundles alongside the report for revalidation. Each collector hashes its own inputs; a Swift timing digest cannot substitute for a linked-app digest.

Distinguish engine-accounted workspace, tracked allocations, process RSS, logical disk bytes and allocated disk bytes. They measure different things. Caller-owned result copies and allocator caches can remain outside engine residency accounting. Artifact size is also different from app-linked size. See [testing](testing.md) and [compatibility and limitations](compatibility.md).
