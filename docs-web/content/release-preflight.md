# Release preflight

NovaGraph remains experimental. The local preflight prepares a candidate manifest and explains missing release evidence. Its first implementation deliberately reports **no-go**: full quality/soak/nightly qualification and final release review are incomplete. A production release requires those gates and complete platform/runtime evidence. No input receipt can waive the pending gates.

## Inspect a candidate

```sh
npm --prefix docs-web ci
make release-preflight-test
make release-preflight RELEASE_PREFLIGHT_OUTPUT=build/release-preflight-run1
```

Use a clean committed checkout and a new output directory. The command runs the real documentation generator in check mode, including generated-source parity and recorded-example input freshness. It reads existing evidence; it does not rerun engine tests, launch long campaigns, modify source, create tags or publish anything.

Outputs are `candidate-manifest.json`, `report.json` and `documentation-parity.log`. For direct Python invocation, exit **1** means a retained no-go report; it is expected for the current project. Exit **2** means invalid input/setup, including an existing output directory; a full report may not exist in that case. Make returns **2** when either Python exit status fails its recipe; inspect the retained report or invoke Python directly to distinguish no-go from setup failure. Preserve the output together and inspect its blocker list. `--version` is optional until a candidate version is selected; leaving it absent is an explicit missing gate, not an invented release version.

```sh
# Once an experimental candidate version has been selected:
python3 tools/release_preflight.py --version 0.1.0-alpha.1 --output build/release-preflight-run2
# Read existing receipts and optionally verify a complete Apple bundle:
python3 tools/release_preflight.py --version 0.1.0-alpha.1 --evidence-index build/candidate-evidence/index.json --apple-bundle dist/apple/candidate --output build/release-preflight-run3
```

The versions and paths above illustrate the command; they do not announce a release. Keep collected evidence under ignored build/output directories.

## Candidate identity and boundaries

The manifest captures HEAD, Git tree, tracked-file content/mode digest and dirty state **before** validation. Identity is checked again afterward; changes during validation block the candidate. It records actual root/catalog/record and WAL component versions from native declarations, the C header hash and exported-symbol inventory, declared platform/qualification wording, documentation content and generated-file hashes, and accepted receipt/artifact hashes. The report includes the manifest hash.

A C ABI fingerprint is not an ABI-version or compatibility certificate. Declared minimum targets are not tested runtime support. Documentation parity proves freshness against current inputs, not successful execution on a release platform. Missing named execution receipts therefore remain separate blockers.

## Evidence receipts

An index uses schema version 1 and a `receipts` array. Each entry has `gate`, a relative `path` to its receipt JSON, and that file's `sha256`. Paths resolve inside the index directory; traversal and links escaping it are rejected. A receipt uses:

| Field | Required meaning |
| --- | --- |
| `schemaVersion` | Integer `1` |
| `gate` | The same supported gate as the index entry |
| `revision` | Full candidate HEAD SHA |
| `dirty` | Boolean `false` |
| `outcome` | `passed` |
| `checks` | Array of unique `{id, outcome}` records, each `passed` |
| `artifacts` | Nonempty array of unique `{path, sha256}` raw-evidence records, relative to the index directory |

The exact required check IDs appear in each missing gate in `report.json`. Native and Swift IDs come from `tests/catalog.json`; a passing summary cannot omit named cases. Documentation requires all executable examples plus content/UI/live/build checks. Recovery, model, sanitizer, source-package, performance and rollback receipts retain separate boundaries. Missing, failed, skipped, duplicated or mismatched checks are rejected. Candidate revision, receipt hash and every referenced raw-file hash must match. Do not relabel historical or failed measurements as new passing evidence, or hide an accepted limitation behind a generic pass.

Receipts are **attestations from trusted test producers**, not authenticated proof supplied by this tool. Hashes establish integrity, not truthful outcomes. This first slice does not parse every raw engine/platform protocol or certify a production go. Retain the full reports/logs and review their provenance. It always reports `productionQualified: false`; deferred quality and final-review gates cannot be supplied as self-declared passing receipts.

## Apple artifacts and final review

`--apple-bundle` reuses the existing complete bundle verifier: slice matrix, file hashes, public headers, C symbols, native object platform/deployment records and runtime qualification. Missing watchOS slices, foreign candidate/version, dirty artifacts and incomplete minimum-runtime/device qualification block acceptance. A manifest alone is not enough; actual bundled files must be available. Validation requires the Apple inspection tools used by the local artifact packaging tools.

The tool is a local preparation step. Hosted installation, platform/runtime qualification, sustained resource tests, full quality nightlies, release/tag/site synchronization and rollback/final publication review remain required. Public-source availability and production qualification are separate decisions. See [compatibility](compatibility.md), [testing](testing.md) and the repository's releasing checklist before preparing an experimental publication.

For experimental source publication, keep the [public TODO roadmap](roadmap.md) visible and complete the repository's first-publication checklist. The production no-go report remains accurate while its qualification gates are open; it does not certify or authorize an experimental publication.
