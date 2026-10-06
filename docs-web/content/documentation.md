# Authoring executable documentation

Canonical Markdown, examples and manifests generate this developer web app. The build rejects stale generated content, broken destinations, invalid snippet/test relationships and recorded outputs whose inputs or engine sources have changed. NovaGraph remains experimental; development checks do not establish a verified release.

## Sources and commands

Edit public prose in `docs-web/content/`, snippet source in `examples/snippets/` or its registered example package, and NGQL programs in `examples/ngql/`. `docs-web/content/site.json` maps articles, code tabs and presets; `examples/manifest.json` defines execution. `docs-web/content/features.json` is the public feature registry; its entries may be empty. `tests/catalog.json` must contain exactly the executable documentation IDs as well as the native and Swift inventories.

From the repository root:

```sh
make docs-examples                 # macOS: native fixtures, C++, public Swift package, mutation checks
make docs-examples-native          # Linux: NGQL fixtures and output-mutation checks
npm --prefix docs-web run docs:generate
make docs-check                    # parity, generator tests, web tests, TypeScript and production build
```

From `docs-web/`, the same commands are `npm run docs:examples` and `npm run test:web`. Node 22+, Python 3, CMake/C++20 and Swift 6 on macOS are required. The repository-root package builds GraphDBKit and the native engine together. The harness uses ordinary SwiftPM commands with no application linker flags. See [source installation](quickstart.md) and [clean consumer acceptance](testing.md#clean-source-installation).

## Register an example

Every example has a stable ID, language, source path, execution mode, setup group, ordered setup dependencies and supported check platforms. `run` means executed with assertions; `compile` means compiled as part of a runnable group, such as the SwiftUI view; `fragment` requires an explicit limitation and is excluded from execution and runnable presets. Platform metadata describes check support, not device qualification.

NGQL runs each fixture with its declared setup files in a fresh actual-engine database. Expected JSON in `examples/expected/` asserts the complete v2 response, including scalar tags, graph values, deterministic path order, index plans and diagnostic spans. Negative examples assert the expected error. Setup and close must succeed. Do not generate expectations from the same execution during tests: review them against the intended graph and API contract.

Swift group members are compiled in manifest order against GraphDBKit from the root source package without native linker flags, then executed. The guide group runs basics → atomic batch → Codable → parameters → traversal, with a final close assertion. Each snippet contains value assertions; the final stdout assertion proves the program reached the end. Standalone typed-API and lifecycle programs remain their own groups. The SwiftUI integration compiles with the lifecycle program on macOS; interactive physical-device qualification remains incomplete. Each C++ example declares a zero-argument `entryPoint` containing its assertions. Every source in a C++ group is compiled, linked and executed independently; C++ setup dependencies and compile-only modes are rejected. The scalar example checks complete round trips.

## Record actual engine results

After reviewing an intentional fixture or engine change, build and execute before refreshing recordings:

```sh
make cpp-build c-header-check
python3 tools/test-doc-examples.py --record
npm --prefix docs-web run docs:generate
make docs-examples docs-check
```

`--record` never updates expected values. It writes `examples/recorded-results.json` only after the assertions pass. Commit executable sources first, then record from that revision and commit the generated documentation/evidence. A following prose-only commit can retain that engine revision. The evidence in `build/docs-examples-evidence.json` records the checkout revision, dirty state, host, executed IDs and digests; CI retains this artifact. Recordings validate specific examples; they do not certify a release.

Only protocol object `last_read_ms` and `last_modified_ms` become `<timestamp-ms>`, and receipt transaction identity becomes `<transaction-id>`. Their types are checked before normalization. User properties, values, ordering, versions, committed LSNs, error codes/spans and plans remain exact. Runtime and example-input SHA-256 digests bind the recordings to the sources; changes invalidate generation until examples pass again.

Code tabs embed these exact source files and identify their setup groups. Recorded outputs use the same fixture IDs as the playground presets. Static mode displays revision-bound recorded fixture results and cannot execute edited input. Opening the opt-in [local runner](developer-tools.md#start-a-disposable-runner) enables real engine queries against its configured disposable or persistent database. The local runner serves its own same-origin app; static pages do not connect to localhost.

## Navigation and search

Article IDs are stable hash routes, with heading destinations such as `#documentation/register-an-example`. Search is generated from published headings and body content, plus any public feature IDs. The generator validates source paths, published anchors, related article IDs, snippet sources, preset executability and feature-to-test links. Unknown routes show an explicit error and an overview link.

Web tests exercise back/deep-link navigation, unknown routes, keyboard search and focus, code-tab changes, clipboard success/failure, persisted theme, mobile-menu state and recorded result labels. Browser inspection supplements DOM tests for layout. CI runs the examples, generator tests, parity check, web tests and production build; intentional fixtures prove that broken API calls, changed output and canonical prose drift fail their respective checks.

## Version and support metadata

`docs-web/content/site.json` retains the development/release version and platform metadata; the generator also records the documentation input digest. These fields remain in the generated data without a persistent status banner or article badges. Keep the metadata aligned with the [compatibility guide](compatibility.md), which describes declared minimum versions, tested coverage and remaining limits. Passing examples does not establish platform support or production readiness.

## GitHub Pages hosting

The `Deploy documentation to Pages` workflow in `.github/workflows/pages.yml` builds this static website on pushes to `main` and on manual runs from `main`. It installs the locked Node 22 dependencies, checks generated-content parity, runs the content and static UI tests, and builds the website before uploading **only `docs-web/dist`**. Native/Swift execution remains in the existing CI workflows; this deployment does not run the opt-in live query server.

The workflow reads the enabled Pages site's `base_path` using GitHub's `configure-pages` action and passes it as `DOCS_BASE_PATH` to Vite. A project site uses its repository prefix, such as `/Nova/`; an account site or custom domain at the root uses `/`. No repository owner/name is hardcoded. Ordinary local builds default to `/`, and the hash routes, generated search and recorded playground work on the static host.

After importing the source into a new public repository with fresh history:

1. Push the source to `main` with GitHub Actions enabled.
2. Open **Settings → Pages** and select **GitHub Actions** as the build and deployment source.
3. Open **Actions → Deploy documentation to Pages → Run workflow**, select `main`, and run it. If the first push ran before Pages was enabled, its configuration step may fail; rerun after enabling Pages.
4. Open the deployment URL shown by the successful `github-pages` environment. Then update the README's public documentation and repository links with the actual URLs.

The workflow uses the repository's built-in GitHub token; no personal access token is needed. It does not enable Pages itself. Only the deploy job receives Pages-write and OIDC permissions, and deployments wait for the documentation build to pass. Branch/environment rules still apply. For details, see [GitHub's custom-workflow guide](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages) and [Vite's Pages deployment guide](https://vite.dev/guide/static-deploy#github-pages).

To preview a repository-prefixed build locally (replace `/Nova/` with the desired path):

```sh
DOCS_BASE_PATH=/Nova/ npm --prefix docs-web run build
DOCS_BASE_PATH=/Nova/ npm --prefix docs-web run preview -- --host 127.0.0.1
```

Open the printed preview origin followed by `/Nova/#quickstart`. The static site serves guides, search, examples and recorded graph results; live NGQL execution continues to use the separately launched local developer runner. The private `docs/` directory is not a website input or a deployed directory.
