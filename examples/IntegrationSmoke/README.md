# Source integration smoke consumer

This uses the root `GraphDBKit` product with ordinary SwiftPM linkage. Supply a disposable database path:

```sh
swift run --package-path examples/IntegrationSmoke IntegrationSmoke create /tmp/nova-source-smoke
swift run --package-path examples/IntegrationSmoke IntegrationSmoke reopen /tmp/nova-source-smoke
```

Choose an unused directory. The program checks Int64/Bool/Double fidelity, a parameterized projection and typed paths, then explicitly closes. It does not remove the supplied database. `make source-package-test` reuses this exact source in fresh Debug/Release consumers of a temporary Git URL and candidate version. Its fixture owns and removes its database and isolates build/dependency caches. No public version is created or published.
