# Testing

Passing development checks does not make NovaGraph production-ready. Native tests, Swift tests, executable examples and platform checks cover different boundaries; run the checks relevant to a change.

The [public TODO roadmap](roadmap.md) tracks unfinished hosted quality, sustained-operation and platform qualification. Ordinary development checks do not close those TODOs.

## Native and Swift checks

```sh
make cpp-test
make cpp-test-debug
make cpp-test-release
make test-runner-contract
make swift-test
make cpp-test-strict
make swift-test-strict
```

Native checks use always-active assertions in Debug and Release. `tests/catalog.json` inventories the named tests, and tooling checks parity against the native runner and Swift test discovery. Strict mode treats a known failure as an ordinary failure; a development or optimized-build pass is not release certification.

Storage tests cover directory ownership, volatile versus durable filesystem state, short reads and failed syncs, transaction rollback, corruption, retained generations and explicit close. Process termination and modeled power loss are separate campaigns with different limits; see [recovery](recovery.md). Parser and query-limit tests exercise diagnostics, typed parameters, bounded cursors, result/batch rejection and cooperative cancellation. A simulated large fan-out checks bounded work, not million-edge performance or RSS.

## Bounded randomized graph model

The native `model` suite compares Nova with an independent reference graph stored in ordinary maps. Four fixed seeds (`1`, `42`, `1315927649`, `3735928559`) each run 200 operations across 16 node IDs in a disposable durable database. The reference model does not use Nova's key encoding, indexes or traversal helpers.

After every operation, it checks node and edge existence/properties, indexed and scanned equality finds, and inbound/outbound reachability through depth three. Operations include replacement, label changes, cascading deletion, valid staged transactions and rejected transactions that must roll back completely. The campaign includes real disk trim, cold index rebuild, checkpoint reopen and replay of committed WAL suffixes; an initial cycle and self-loop exercise traversal revisits. Result sets must contain no duplicates. Limits exceed this small graph's size, so this suite compares complete membership rather than capped ordering or path selection.

```sh
# After a native build, run the fixed seed corpus in strict mode.
build/cpp-debug/nova_graph_test --strict --suite model
# Replay one decimal unsigned 32-bit seed, with the same 200-operation bound.
NOVA_MODEL_SEED=42 build/cpp-debug/nova_graph_test --strict --suite model
# CTest integration uses the normal per-suite timeout.
ctest --test-dir build/cpp-debug -R '^nova_model$' --output-on-failure
```

The runner prints the seed, step and operation inputs before each operation and reports the failing seed/step. Preserve the complete output; CI retains the full CTest diagnostic log alongside results. A discovered engine failure must keep its seed in the fixed corpus and, where practical, a minimized named regression. Invalid `NOVA_MODEL_SEED` values fail the suite. Leave this override unset for normal CI so all four seeds run.

Debug, Release and the native sanitizer jobs discover this suite through CMake. Two sensitivity cases deliberately change a real engine record or inject a dangling edge to the missing endpoint and verify that the comparison rejects each divergence. The rejected batch's missing endpoint and submitted edges are checked explicitly alongside the normal graph. This bounded campaign does not establish exhaustive semantics, path ordering, concurrency, fault tolerance, sustained memory/disk growth or soak qualification; existing focused tests and the remaining quality campaigns cover different boundaries.

## Documentation checks

```sh
npm --prefix docs-web ci
make docs-examples
make docs-check
```

Executable examples compile the Swift API and lifecycle snippets, compile the SwiftUI integration, and assert complete real-engine NGQL outputs. The site generator checks source paths, links, snippets, metadata and recorded-output freshness. Web tests cover navigation, search, code tabs, theme, recorded results and live-runner interaction. See [documentation authoring](documentation.md).

## Maintenance and lifecycle

`python3 tools/test-maintenance-cli.py --cli build/cpp/nova-maintenance` exercises the offline commands after a native build. Maintenance tests cover process termination during backup, restore, migration and index repair. `make c-header-check`, `make typed-api-example` and `make lifecycle-example` validate the public boundary and compiled examples. Swift targets use Swift 6 concurrency checking. These tests do not replace physical-device lifecycle qualification.

## Clean source installation

`make source-package-test` resolves a temporary tracked-source Git URL and candidate version from independent Debug and Release consumers with isolated caches. Consumers run typed create/query/path checks, close and reopen from a second process. `python3 tools/test-source-package.py --revision HEAD --output build/source-package-evidence.json` tests exactly the committed tree. Neither path imports prebuilt archives or supplies application linker flags. This does not verify a hosted release URL or an older Apple runtime.

## Apple artifacts and sample

`make apple-artifact-test` checks artifact checksums, matrix rules, runtime metadata and archive contracts. A built slice alone does not establish runtime support. See [compatibility](compatibility.md).

`make sample-app-test` executes the application-owner suite and acceptance-harness contracts. `python3 tools/sample_app.py --output build/sample-app-acceptance` builds and launches the macOS and iOS simulator sample, verifies test inventories and exports screenshots. Use a new output directory. Failed subprocesses report their exit code and a bounded excerpt of errors and trailing log lines directly in CI output; the complete logs and any produced result bundles remain in the output directory for inspection. This harness creates a disposable simulator; it does not install on physical devices. A separate [manual physical-device smoke test](../../examples/NovaGraphApp/README.md#physical-device-smoke-test) passed on iPhone 17 Pro Max / iOS 27.0 at source revision `e42262f` in Debug on 2026-10-06, covering graph/query, edit persistence, background/return and manual trim. It is maintainer-reported evidence, not an automated harness result or device performance qualification. See the [sample guide](../../examples/NovaGraphApp/README.md).

## Local sanitizer checks

Use separate Debug build directories for AddressSanitizer/UndefinedBehaviorSanitizer (ASan/UBSan) and ThreadSanitizer (TSan). Strict mode makes unresolved regressions fail. Instrumentation adds significant overhead; `NOVA_TEST_TIMEOUT=180` gives each native suite a bounded three-minute allowance without changing test inputs or assertions. Ordinary builds retain the 30-second default and the maintenance suite's minimum 120 seconds. These timeouts are test-runner limits, not performance budgets.

```sh
cmake -S cpp -B build/t22-asan-ubsan -DCMAKE_BUILD_TYPE=Debug -DNOVA_STRICT_REGRESSIONS=ON -DNOVA_TEST_TIMEOUT=180 -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/t22-asan-ubsan --parallel 4
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ctest --test-dir build/t22-asan-ubsan --output-on-failure

cmake -S cpp -B build/t22-tsan -DCMAKE_BUILD_TYPE=Debug -DNOVA_STRICT_REGRESSIONS=ON -DNOVA_TEST_TIMEOUT=180 -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/t22-tsan --parallel 4
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/t22-tsan --output-on-failure
```

The native suites include the C API, storage/recovery and lifecycle concurrency/close/cancellation tests. On macOS, exercise the Swift wrapper and its compiled C++ dependency separately:

```sh
NOVA_STRICT_REGRESSIONS=1 ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 swift test --scratch-path build/t22-swift-asan --sanitize address --jobs 4
NOVA_STRICT_REGRESSIONS=1 TSAN_OPTIONS=halt_on_error=1 swift test --scratch-path build/t22-swift-tsan --sanitize thread --jobs 4
```

Leak detection is explicitly disabled in these macOS commands because LeakSanitizer is unsupported by the tested Apple toolchain; these runs establish no leak coverage. Do not disable supported leak checks when qualifying another platform. ASan checks memory access, UBSan checks undefined behavior, and TSan checks data races observed during the executed tests. They do not replace randomized model tests, fuzzing, sustained memory-growth/soak tests, hosted CI evidence or physical-device qualification. Record unsupported runtimes and failed/time-limited attempts separately from successful runs; do not suppress findings to obtain a pass.

### Automated sanitizer jobs

`.github/workflows/quality.yml` runs the existing strict suites on pushes, pull requests, manual dispatch and a daily schedule. Jobs fail on sanitizer findings or test/build errors; piped logging preserves nonzero exit codes. Each job has a 30-minute limit, native suites keep the explicit 180-second allowance, and the Swift matrix runs both lanes even if one fails. Environment, configuration, build and test logs are retained where applicable, including on failure, for 14 days; native jobs also retain CTest XML and diagnostic logs.

| Job | Runner | Instrumentation |
| --- | --- | --- |
| Native ASan/UBSan | Ubuntu 24.04, Clang | Address and undefined-behavior checks; leak detection enabled |
| Native TSan | macOS 15, Xcode 26.3 | Thread checks across all strict native suites |
| Swift ASan | macOS 15, Xcode 26.3 | Address checks through the Swift wrapper and compiled C++ dependency |
| Swift TSan | macOS 15, Xcode 26.3 | Thread checks through the Swift wrapper and compiled C++ dependency |

The macOS jobs explicitly set `DEVELOPER_DIR=/Applications/Xcode_26.3.app/Contents/Developer`, which is listed in the [hosted runner inventory](https://github.com/actions/runner-images/blob/main/images/macos/macos-15-Readme.md). A missing selected toolchain is a job failure, not fallback qualification. Logs record the actual source revision, OS and compiler/toolchain; the runner label alone does not prove a particular image or architecture. Swift 6 language checking remains enabled by the package manifest.

To reproduce native Linux ASan/UBSan, use the native configure/build commands above with `CC=clang CXX=clang++` and run CTest with `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. [LLVM documents integrated leak checking](https://clang.llvm.org/docs/LeakSanitizer.html); do not apply the Apple leak-disable setting to supported Linux jobs. On macOS, select the same available Xcode with `DEVELOPER_DIR` and use the native TSan and Swift commands above. Build directories may be renamed without changing coverage. The workflow uses `ctest --output-junit` to retain native results and `set -euo pipefail` when logging with `tee`.

Workflow configuration and local checks do not establish a hosted run pass. The October 1 local evidence below uses a different OS/toolchain/architecture from these hosted labels. Successful hosted run artifacts are still required before claiming that lane is verified. The native jobs also execute the bounded model suite above. Daily sanitizer runs alone do not satisfy broad fuzz coverage, mixed-workload soak, leak coverage on Apple runtimes, or seven passing frozen-revision full quality nightlies. Those acceptance gates and physical/minimum-runtime qualification remain open.

### Local sanitizer evidence — 2026-10-01

On arm64 macOS 27.0.1 with Xcode 27.0 (27A266a), Apple Clang 21 and Swift 6.4:

| Run | Executed coverage | Result |
| --- | --- | --- |
| Native ASan + UBSan | 245 cases across 20 strict suites | Passed; no sanitizer findings |
| Native TSan | 245 cases across 20 strict suites | Passed with 180-second suite allowance; no sanitizer findings |
| Swift ASan | 39 strict tests, including the compiled C++ boundary | Passed; no sanitizer findings |
| Swift TSan | 39 strict tests, including async/lifecycle and the compiled C++ boundary | Passed; no sanitizer findings |

The first native TSan attempt passed 19 suites but timed out in the I/O suite at the ordinary 30-second limit. The unchanged suite completed separately in 98 seconds; a full rerun with the explicit 180-second allowance then passed every suite (I/O: 94 seconds). The original timeout remains recorded as a failed attempt. No tests, workloads, assertions or engine behavior were changed, and sanitizer timeouts are not latency acceptance. The native ASan/UBSan run passed with the ordinary timeouts.

These are local existing-suite results. LeakSanitizer, hosted sanitizer CI, randomized model/fuzz coverage, sustained memory-growth/soak acceptance and other platform qualification remain outside this pass. No production-readiness claim follows from it.

## Release preflight contracts

`make release-preflight-test` runs the candidate/evidence contracts against disposable real Git trees and the real documentation generator. Cases reject stale guides, changed recorded inputs, missing watchOS slices, foreign revisions, skipped/omitted checks, altered receipts/logs and escaping evidence paths. They also verify identity capture before validator side effects and preservation of prior reports. CI retains the full contract log even on failure. These are tooling checks; see [release preflight](release-preflight.md) for the retained no-go report and qualification limits.

## Performance contracts

`make performance-contracts` validates sampling, current budgets, controlled baseline review, process locking, diagnostic process attribution, missing observations, interruption cleanup and rejection of diagnostic evidence, disk snapshot caps and distribution gate ordering, alongside the resource tracking contracts. `make performance-smoke` and `make performance-resources` also exercise the real native harness. Swift operation contracts exercise both historical reports and new validated operation inventories. These checks validate the measurement tools; they are not performance evidence. See [benchmark protocols and limits](benchmarks-data.md) before collecting acceptance runs.

The [initial desktop reference](benchmarks-data.md#initial-desktop-budget-acceptance) is accepted with a documented consistency limitation. Both collections meet absolute caps, but they are not a CLI-qualified frozen baseline or a passed regression comparison. This engineering acceptance does not replace sustained growth tests or deferred physical-device qualification.

## Memory qualification

For production qualification, measure peak process RSS through checkpoint and reopen for each claimed platform and workload, and compare it with the approved limits in `benchmarks/budgets.json`. Accounted reservations, allocation diagnostics and process RSS describe different quantities; none substitutes for another. Simulator evidence does not qualify physical phone/watch memory behavior.

Sustained quality tests must repeat mutations, traversal, trim, checkpoint and reopen, retaining memory trends and failures to detect growing memory use. A budget breach or unexplained continuing growth blocks qualification for the affected workload until resolved. This sustained coverage remains release work; ordinary unit tests and this policy document do not establish it.

The [first-release policy](maintenance.md#first-release-memory-policy) retains existing accounted-memory guards and explicitly excludes a hard aggregate checkpoint/recovery or total-process-memory guarantee. Qualification is limited to the tested workload/platform envelope.
