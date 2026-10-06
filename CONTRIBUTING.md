# Contributing to NovaGraph

NovaGraph is experimental. Contributions should preserve explicit error reporting, durable recovery, bounded resource behavior, and accurate documentation. Discuss substantial API or storage-format changes in an issue before implementing them.

See the [public TODO roadmap](docs-web/content/roadmap.md) for unfinished work. Propose a small scoped contribution and update the canonical roadmap and public guides when its acceptance is demonstrated. Experimental publication does not mean those production gates are complete.

## Development setup

The root Swift package builds the native engine from source. Use Swift 6.0 or later for the core package, CMake 3.22 or later with a C++20 compiler for native tests, Python 3 for tooling, and Node.js 22 with npm for the documentation app. The optional HTTP developer runner requires Swift 6.1 or later. Apple app and artifact checks require Xcode and the corresponding SDKs; see the workflow files for their exact pinned versions.

```sh
swift build
make cpp-test
make swift-test
cd docs-web
npm ci
npm run dev
```

## Changes and validation

Keep changes focused and add regression coverage for behavior changes. Explain the problem, resulting behavior, and checks run in the pull request. For storage, transaction, or recovery changes, run the strict checks as well:

```sh
make cpp-test-strict
make swift-test-strict
```

Update the public guides in `docs-web/content/` alongside behavior changes. Snippets and playground recordings come from executable examples under `examples/`; do not edit generated JSON by hand. Follow [the documentation workflow](docs-web/content/documentation.md), then run:

```sh
make docs-check
make public-export-test
```

The local `docs/` folder is reserved for private development notes and is not a public build dependency. Do not add personal paths, raw development evidence, credentials, signing profiles, database contents, or private plans to commits or attachments. Use synthetic data in tests and examples.

## Reporting problems

For ordinary bugs, include the revision, OS/toolchain, a minimal synthetic reproduction, expected behavior, and actual behavior. Follow [SECURITY.md](SECURITY.md) for suspected vulnerabilities; do not put sensitive details in public issues.

Contributions are distributed under the repository's [Apache-2.0 license](LICENSE). Preserve third-party notices and identify any new dependencies and their licenses in your pull request. Keep discussion respectful and focused on the work.
