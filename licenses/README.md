# Dependency notices

NovaGraph and GraphDBKit use the repository's [Apache-2.0 license](../LICENSE). The private native implementation vendors nlohmann/json; its [MIT notice](../cpp/third_party/nlohmann/LICENSE.MIT) must accompany distributions containing it.

`dependencies.json` records pinned Swift dependencies used by the optional local HTTP developer runner, including license URLs, revisions, and hashes. They are not dependencies of the core GraphDBKit package. `docs-web/package-lock.json` records the documentation application's npm dependency licenses and integrity metadata.

`tools/apple_artifacts.py` validates this inventory against the dependency locks and includes the applicable notices in Apple artifact candidates. Review license changes whenever dependencies are updated.
