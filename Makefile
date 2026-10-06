CMAKE ?= cmake
CTEST ?= ctest

.PHONY: c-header-check typed-api-example lifecycle-example benchmarks benchmarks-build benchmarks-test cpp-test cpp-build cpp-test-debug cpp-test-release cpp-test-strict test-runner-contract swift-build swift-test swift-test-strict dev-query-build zip clean docs-check

cpp-build:
	$(CMAKE) -S cpp -B build/cpp -DCMAKE_BUILD_TYPE=Release
	$(CMAKE) --build build/cpp --parallel

cpp-test: cpp-test-debug cpp-test-release test-runner-contract

cpp-test-debug:
	$(CMAKE) -S cpp -B build/cpp-debug -DCMAKE_BUILD_TYPE=Debug -DNOVA_STRICT_REGRESSIONS=OFF
	$(CMAKE) --build build/cpp-debug --parallel
	$(CTEST) --test-dir build/cpp-debug --output-on-failure --verbose
	python3 tools/check-test-inventory.py build/cpp-debug/nova_graph_test

cpp-test-release:
	$(CMAKE) -S cpp -B build/cpp-release -DCMAKE_BUILD_TYPE=Release -DNOVA_STRICT_REGRESSIONS=OFF
	$(CMAKE) --build build/cpp-release --parallel
	$(CTEST) --test-dir build/cpp-release --output-on-failure --verbose
	python3 tools/check-test-inventory.py build/cpp-release/nova_graph_test

# Strict storage correctness gate; passing alone does not qualify a production release.
cpp-test-strict:
	$(CMAKE) -S cpp -B build/cpp-strict -DCMAKE_BUILD_TYPE=Release -DNOVA_STRICT_REGRESSIONS=ON
	$(CMAKE) --build build/cpp-strict --parallel
	$(CTEST) --test-dir build/cpp-strict --output-on-failure

test-runner-contract:
	python3 tools/test-runner-contract.py

c-header-check:
	python3 tools/check-c-header.py

swift-build: c-header-check
	swift build

swift-test: c-header-check
	swift test
	mkdir -p build
	swift test list --skip-build > build/swift-test-inventory.txt
	python3 tools/check-test-inventory.py --swift build/swift-test-inventory.txt

swift-test-strict: c-header-check
	NOVA_STRICT_REGRESSIONS=1 swift test

dev-query-build:
	swift build --package-path devtools

benchmarks-build:
	swift build --package-path benchmarks

benchmarks-test: benchmarks-build
	cd benchmarks && python3 ../tools/test-benchmark.py "$$(swift build --show-bin-path)/NovaBenchmark"

benchmarks:
	swift run --package-path benchmarks NovaBenchmark

demo:
	swift run --package-path examples/NovaGraphDemo NovaGraphDemo

package-xcframework:
	@test -n "$(VERSION)" || (echo "Use make package-xcframework VERSION=0.0.0-candidate"; exit 1)
	./tools/package-xcframework.sh --version "$(VERSION)" --output "dist/apple/$(VERSION)"

typed-api-example: c-header-check
	swift run --package-path examples/TypedAPI TypedAPI

lifecycle-example: c-header-check
	swift run --package-path examples/Lifecycle Lifecycle

docs-install:
	cd docs-web && npm install

docs-dev:
	cd docs-web && npm run dev

docs-build:
	cd docs-web && npm run build

docs-check:
	cd docs-web && npm run docs:check && npm test && npm run test:web && npm run build

.PHONY: public-export public-export-test
# Source-only snapshot for a new public repository; never includes Git history.
public-export-test:
	python3 tools/test-public-export.py

zip: public-export

public-export:
	python3 tools/export-public.py --output dist/Nova-source.zip

clean:
	rm -rf .build build swift/.build devtools/.build benchmarks/.build examples/NovaGraphDemo/.build docs-web/dist

.PHONY: docs-examples docs-examples-native
# Full Swift/public-package examples run on macOS; NGQL fixtures also run on Linux.
docs-examples: c-header-check cpp-build
	python3 tools/test-doc-examples.py
	python3 tools/test-doc-examples-contract.py

docs-examples-native: cpp-build
	python3 tools/test-doc-examples.py --native-only
	python3 tools/test-doc-examples-contract.py

.PHONY: dev-query-test
# Real HTTP/engine and browser-component integration; no simulated engine responses.
dev-query-test: dev-query-build docs-build
	swift test --package-path devtools
	python3 tools/check-test-inventory.py --devtools
	python3 tools/test-dev-query.py
	python3 tools/test-dev-query.py --web

.PHONY: source-package-test
# Each consumer resolves only the source candidate URL with isolated caches.
source-package-test:
	python3 tools/check-test-inventory.py --package
	python3 tools/test-source-package.py --output build/source-package-evidence.json

.PHONY: apple-artifact-test
apple-artifact-test:
	python3 tools/test-apple-artifacts.py
	python3 tools/check-test-inventory.py --artifacts

.PHONY: sample-app-generate sample-app-test
sample-app-generate:
	xcodegen generate --spec examples/NovaGraphApp/project.json

sample-app-test:
	swift test --package-path examples/NovaGraphApp
	mkdir -p build
	swift test --package-path examples/NovaGraphApp list --skip-build > build/sample-test-inventory.txt
	python3 tools/test-sample-app-contract.py
	python3 tools/check-test-inventory.py --sample build/sample-test-inventory.txt

.PHONY: performance-contracts performance-smoke performance-measure performance-ios performance-resources resource-native-contracts
PERFORMANCE_OUTPUT ?= build/performance-evidence
resource-native-contracts:
	mkdir -p build/resource-contracts
	xcrun clang++ -std=c++20 -O2 -Wall -Wextra -Werror -DNOVA_RESOURCE_DIAGNOSTICS -pthread benchmarks/performance/test_allocation_tracker.cpp -o build/resource-contracts/allocations
	build/resource-contracts/allocations
	xcrun clang++ -std=c++20 -O2 -Wall -Wextra -Werror -Icpp/third_party benchmarks/performance/test_resource_disk.cpp -o build/resource-contracts/disk
	build/resource-contracts/disk
performance-contracts: resource-native-contracts
	python3 tools/check-test-inventory.py --performance
	python3 tools/test-performance.py
	python3 tools/test-swift-measurements.py
	python3 tools/test-resource-measurements.py
	python3 tools/test-performance-gate.py
	python3 tools/test-performance-diagnostic.py
	python3 tools/test-resource-budgets.py
performance-smoke: performance-contracts
	python3 tools/performance.py run --mode smoke --output $(PERFORMANCE_OUTPUT)
	python3 tools/test-performance-native.py $(PERFORMANCE_OUTPUT)/nova-performance
performance-measure: performance-contracts
	python3 tools/performance.py run --mode measure --output $(PERFORMANCE_OUTPUT)
performance-ios: performance-contracts
	python3 tools/performance.py run --mode measure --profile phone --platform ios-simulator --output $(PERFORMANCE_OUTPUT)

# Instrumented evidence is diagnostic-only; no timing or size qualification.
performance-resources: performance-contracts
	python3 tools/performance.py run --mode resources --output $(PERFORMANCE_OUTPUT)
	python3 tools/test-resource-native.py $(PERFORMANCE_OUTPUT)/nova-performance

.PHONY: release-preflight-test release-preflight
RELEASE_PREFLIGHT_OUTPUT ?= build/release-preflight
release-preflight-test:
	python3 tools/check-test-inventory.py --release
	python3 tools/test-release-preflight.py

# Python reports no-go with exit 1; Make returns 2 for a failed recipe.
release-preflight:
	python3 tools/release_preflight.py --output $(RELEASE_PREFLIGHT_OUTPUT)
