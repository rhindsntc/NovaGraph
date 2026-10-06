# Preparing a public release

Public availability and production qualification are separate decisions. Until the platform, durability, recovery, compatibility, and resource gates are satisfied, describe the project and any downloadable builds as experimental. Do not advertise candidate artifacts or simulator measurements as production or physical-device qualification.

## Experimental source publication

The [public TODO roadmap](docs-web/content/roadmap.md) separates first-publication setup from incomplete production qualification. The quality/soak, runtime/device and complete release/rollback gates may remain TODOs for an experimental source-only release; they still block a production claim. Keep their status visible in README, the website and release notes. Fuzz work remains deferred.

The existing local preflight's no-go decision is a production boundary, not an automatic ban on sharing experimental source. It does not authorize publication. Complete the first-publication checklist below, verify the actual public candidate, and obtain final maintainer review before publishing. Ship source only unless separate binary qualification supports the advertised downloads.

## Local candidate preflight

The [release preflight guide](docs-web/content/release-preflight.md) describes the read-only tool that inventories a candidate revision, validates documentation parity and supplied evidence, and retains a manifest/no-go report. Run `make release-preflight-test` for its contracts, or `make release-preflight RELEASE_PREFLIGHT_OUTPUT=build/release-preflight-run1` to inspect the current candidate. Install the locked docs dependencies first.

Direct Python invocation returns **no-go / exit 1** by design: full quality/soak/nightly qualification and final release review are still pending. Supplied receipts cannot waive those gates. Python exit 2 is an input/setup error; Make returns 2 for either failed recipe status. Inspect the retained report or use Python directly to distinguish them. Existing outputs are never overwritten. It does not launch tests, run long campaigns, change source, create a tag, upload artifacts or publish a website. Receipt hashes establish file integrity, not the truth of test results; use trusted producers and retain their full raw evidence.

## First publication

1. Review the public source tree and generated website, including search, images, examples, dependency locks, and workflows. Check for secrets, personal information, and material you do not have permission to redistribute. Preserve the root license, vendored notices, and `licenses/dependencies.json`.
2. Run the relevant checks from a clean checkout without private notes: native and Swift tests, executable docs, documentation UI tests/build, source-package consumer tests, and artifact contracts. Confirm the hosted CI images actually provide the pinned Xcode versions before treating those jobs as release gates.
3. Choose the repository owner/name, initial experimental version, public contact, and documentation URL. Replace installation-template URLs with the actual repository URL once it exists. Decide whether the first publication is source-only or also includes validated binary artifacts.
4. Review and commit the cleaned source, then run `make public-export`. It creates `dist/Nova-source.zip` from the committed tree. The export refuses dirty checkouts, tracked private work-product paths, symlinks, and overwriting an existing archive. Inspect the extracted archive; it is a file-selection safeguard, not a content-level secret scanner.
5. Import the extracted `Nova/` directory into a **new** repository with a fresh initial commit. Choose the public author identity before committing. Do not copy the existing `.git`, push old branches/tags, use a mirror push, or make the private development repository public. Keep the original development history and private notes separately backed up.
6. Configure the new repository: least-privilege Actions permissions, protected main branch/required checks, dependency and secret alerts, and a private vulnerability-reporting channel. Verify that the reporting instructions in `SECURITY.md` work. Review any Actions logs or artifacts for private data before exposing them. Prefer pinned action commits for stronger supply-chain control.
7. After the fresh initial commit, re-run `python3 tools/test-doc-examples.py --record` (with the native example runner built), regenerate the website, and commit those outputs so fixture provenance refers to a public revision. Run CI against that public candidate. Review the generated site and playground, then create the experimental tag and release notes. Document tested platforms, outstanding limitations, migration expectations, checksums for any artifacts, and the exact release revision. Publish the repository and website only after this final review.

The export intentionally contains no Git history. Private planning documents retained locally under `docs/` and `benchmarks/performance/PLAN.md` must remain ignored. Removing them from the current tree does not remove them from the existing private repository's history.

## Documentation hosting

The included `.github/workflows/pages.yml` checks and deploys only `docs-web/dist` from `main`. After the fresh public repository exists, enable **Settings → Pages → GitHub Actions** and manually run **Deploy documentation to Pages** from `main`; subsequent pushes deploy automatically. An initial run before Pages is enabled can fail at configuration. GitHub supplies the site base path, so no owner/repository placeholder needs editing in the workflow. Update public URLs in the README once the deployed URL is known. See the [hosting guide](docs-web/content/documentation.md#github-pages-hosting).

## First-release memory qualification

The [memory policy](docs-web/content/maintenance.md#first-release-memory-policy) retains existing accounted-memory guards for queries, traversal, writes and trim. Checkpoint and open/recovery may temporarily exceed query and residency limits; the first release does not promise an aggregate temporary-memory cap for those paths or a strict total-process-memory ceiling.

Before claiming production support:

- Verify checkpoint/reopen peak process RSS against `benchmarks/budgets.json` for every claimed platform/workload. State the tested graph sizes and environment; larger or different workloads are not automatically covered.
- Retain sustained-test evidence that repeated operation does not exhibit unexplained continuing memory growth. Resolve budget breaches and growth findings before qualifying the affected workload.
- Keep allocation/reservation diagnostics distinct from process RSS, and simulator evidence distinct from physical-device qualification. Missing evidence remains incomplete, not a pass.
- Publish the maintenance/recovery exception and tested limits in the release notes and developer documentation. Applications requiring a strict total-memory ceiling remain outside this guarantee.

This checklist defines required evidence; it does not assert that qualification or sustained testing is complete. Introduce a dedicated maintenance-memory cap separately if measurements show a need or an application requires strict enforcement.

## Performance acceptance boundary

The [initial desktop measurements](docs-web/content/benchmarks-data.md#initial-desktop-budget-acceptance) meet the approved absolute budgets and are accepted with a documented consistency limitation. Preserve original failures and explicit review decisions; this acceptance is not an automated baseline freeze or candidate regression pass. Future changes retain absolute-budget checks and explicit regression review. The strict controlled comparison tool remains unchanged.

Production release qualification still requires the claimed platform/runtime evidence and sustained memory-growth checks. An accepted desktop reference does not qualify physical phone/watch hardware, minimum runtimes or larger/different workloads.

## Ongoing releases

Keep public guides, executable snippets, playground recordings, dependency notices, and release notes aligned with behavior. Re-run package and platform checks whenever their inputs change. Explicitly record unsupported combinations and incomplete hardware coverage rather than converting a successful build into a support claim.

If a public version has already shipped, follow the published compatibility policy before changing database formats or APIs. Preserve prior release notes and publish migration instructions where needed.

## Repository-host references

- [GitHub repository best practices](https://docs.github.com/en/repositories/creating-and-managing-repositories/best-practices-for-repositories)
- [Configure private vulnerability reporting](https://docs.github.com/en/code-security/how-tos/report-and-fix-vulnerabilities/configure-vulnerability-reporting/configure-for-a-repository)
- [Consequences of changing repository visibility](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/managing-repository-settings/setting-repository-visibility)
