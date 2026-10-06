# Open-source TODOs

NovaGraph is being prepared for an **experimental, source-only** public release. It is not production-ready. These TODOs describe unfinished work; they are not features, tested support claims or a release-date promise. Use disposable data for evaluation.

The current source includes the C++ engine and Swift package, storage/recovery tools, executable examples, a sample Apple app, developer documentation and a recorded playground. Native/Swift tests, bounded model checks, sanitizer workflow definitions and a local release preflight exist. Hosted execution and complete platform qualification remain separate evidence.

## Before public publication

These are launch tasks for maintainers. The production TODOs below may remain open for an experimental source release, provided its limitations remain visible.

- [ ] Choose the public repository owner/name, public Git author identity, initial experimental version and documentation URL. Update installation examples to use the real repository URL once it exists.
- [ ] Review the committed source snapshot, images, dependency licenses and workflow output for private material. Preserve the Apache-2.0 license and third-party notices.
- [ ] Import the reviewed source export into a new repository with a fresh initial commit. Do not copy the private development history, old branches/tags or private planning notes.
- [ ] Configure repository permissions and main-branch protection, enable dependency/secret alerts, and enable and test private vulnerability reporting before public publication. Verify the reporting route in the repository's security policy.
- [ ] Verify hosted toolchain availability and run the relevant CI, source-package installation, executable documentation and playground checks against the actual public candidate. Re-record examples after the fresh initial commit so provenance refers to public history, then regenerate the website.
- [ ] Review the final source-only release notes and website against the same public tag. Publish only after maintainer review; do not offer unqualified binary downloads or production support.

The repository's releasing checklist describes the source export and fresh-history procedure. A history-free export excludes private files; it is not a complete secret-content review. No public repository URL, release tag or hosted CI pass is currently promised.

## Production qualification TODOs

These gates remain open after experimental publication. A passing development test or a completed build cannot close them.

### Quality and sustained operation

- [ ] Collect successful hosted native/Swift sanitizer results, including supported Linux leak checks. Keep unsupported Apple leak coverage explicit.
- [ ] Complete the deferred parser, parameter, storage-decoder and bounded-executor fuzz coverage and campaigns when that work resumes. Fuzz work is currently deferred; no completed fuzz acceptance is claimed.
- [ ] Add sustained valid mixed-workload tests through mutation, traversal, trim, checkpoint and reopen. Retain correctness checks, process-memory and disk-growth trends, and failure logs; resolve unexplained continuing growth.
- [ ] Run the required 24-hour mixed-workload soak and seven consecutive passing full quality nightlies on a frozen candidate revision, with no unresolved findings. Scheduled sanitizer jobs alone do not satisfy this gate.

Existing [testing instructions](testing.md) describe implemented checks and their limits. The bounded randomized model is implemented; broader quality qualification remains unfinished.

### Apple platforms and performance

- [ ] Complete physical-device and oldest-runtime qualification for every claimed Apple platform and architecture. A [manual iPhone 17 Pro Max / iOS 27.0 sample smoke test](../../examples/NovaGraphApp/README.md#physical-device-smoke-test) passed on 2026-10-06; broader qualification remains open. Keep simulator execution, deployment-target compilation and physical-device results distinct; publish the actual tested matrix.
- [ ] Verify the claimed workloads against the existing latency, process-memory and size budgets on qualified platforms. Retain the accepted desktop reference and its documented consistency limitation; it is not an automated regression comparison pass.
- [ ] Retain candidate-bound source/binary consumer and artifact evidence before offering binary downloads. A built Apple slice or a source-package consumer pass does not qualify a binary release.

The [compatibility guide](compatibility.md) lists declared deployment targets, incomplete runtime coverage and unsupported combinations. visionOS remains unsupported. The [benchmark guide](benchmarks-data.md) describes the approved budgets and accepted desktop evidence.

### Complete release gate and rollback

- [ ] Automate candidate-bound collection of raw test, recovery, quality, packaging, performance and documentation evidence. Reject missing, stale, foreign, failed or skipped evidence and preserve the original outcomes.
- [ ] Complete production qualification and final review in the release gate. The current [local preflight](release-preflight.md) always reports no-go; supplied receipts are trusted-producer attestations, not authenticated proof of test outcomes.
- [ ] Build documentation, examples and artifacts from the same public candidate/tag, with clear development/release version separation and matching manifests/checksums.
- [ ] Rehearse package/site rollback and backup/restore for the candidate. Do not open newer database formats with an incompatible older engine as a rollback strategy; publish supported migration and recovery instructions.
- [ ] Archive the signed-off production decision and publish accurate platform, compatibility, support and migration information before claiming production readiness.

## Evaluation limits

The supported design remains single-process directory ownership and flat scalar properties. There is no distributed database service. Checkpoint and open/recovery may temporarily exceed query/residency limits; a strict total-process-memory ceiling is not promised. Larger or different workloads are not automatically covered by measured budgets. Read [compatibility](compatibility.md), [maintenance](maintenance.md) and [recovery](recovery.md) before evaluating persistence or retry behavior.

Contributors can choose a TODO, propose a small scoped change and include relevant checks plus updated documentation. Coordinate substantial API, storage-format or qualification changes with maintainers. Keep synthetic data and public evidence in contributions; private plans, personal paths, real databases and credentials do not belong in commits or issue attachments.
