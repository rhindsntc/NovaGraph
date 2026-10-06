# Local Developer Tools

The developer website has two explicit modes: read-only recordings from the actual engine, and an opt-in local runner served by NovaDevQuery. It is a development tool, not a production network API. The public/static website never contacts localhost. Selecting a preset does not execute it.

## Start a disposable runner

From the repository root on macOS with Swift 6.1 or newer (the pinned SwiftNIO requirement), install the web dependencies and build the tool through the root source package and build the website:

```sh
npm --prefix docs-web ci
make dev-query-build docs-build
./devtools/.build/debug/NovaDevQuery
```

Open the printed `http://127.0.0.1:8080/#playground` URL and press **Connect local runner**. The page remains read-only until you connect. **Run query** submits the complete editor text to the actual engine, including writes. Choose a preset, run it, and inspect the returned typed JSON or result graph. Editing text clears the previous result; nothing runs automatically.

`--port 8081` changes the port. `--web-root /absolute/path/to/docs-web/dist` selects an already built app. Run from the repository root unless passing an absolute web root. The server pins its SwiftNIO dependency and serves this same generated documentation app. Rebuild the website after changing canonical docs or UI code.

Each default launch opens a new, uniquely named temporary database. **Reset disposable database** creates a fresh database and discards the old one; it does not merely clear the editor. Ctrl-C or SIGTERM closes the database and removes session directories. A forced kill can leave an orphaned `nova-runner-*` directory in the OS temporary directory; the next launch does not reopen it. No automatic deletion of unrelated directories occurs.

## Open an explicit persistent database

Pass the path deliberately; this enables real durable writes to that database:

```sh
./devtools/.build/debug/NovaDevQuery --database /absolute/path/to/my-development-db
```

The UI labels the session **persistent** and shows its path. Browser reset is disabled. Stop the runner and use the same explicit path to retain data across restarts. Nova's exclusive directory ownership prevents a second engine from opening that directory simultaneously. Prefer a development copy when experimenting. The old `NOVA_DB_PATH` and `NOVA_DEV_HOST` environment overrides are rejected; port selection uses `--port`.

## Query and inspect

After running the Social Network preset, run:

```ngql
explain find nodes Person where email="ada@nova.io" limit 10;
walk from user:ada over FOLLOWS depth 2 limit 10 paths;
```

Explain output comes from the real planner, including the chosen index and access path. JSON preserves scalar type tags, full integer digits, mutation receipts, projections, ordered paths and structured errors with source context. The graph displays only records actually present in the result, capped at 200 nodes/400 edges; an edge is drawn only when both endpoints are returned. A find/projection or mutation result is not an entire database picture. Complete output remains available in JSON. The recorded playground presets request `paths`, so their graphs include the traversal start and the actual connecting edges. The Social Network preset shows Ada → Grace → Margaret over FOLLOWS: Linus is beyond depth 2, and COLLABORATES is excluded by the edge filter. This is the query result, not every record created by the setup statements.

Use **Inspect nodes**, **Inspect edges** or **Inspect indexes** to fetch real pages (50 entries in the browser, at most 100 through the API). Index pages list declared node-property indexes. Implicit label/adjacency indexes are visible through explain plans, not fabricated inspector entries. **Next page** advances the opaque cursor. Each page runs under the native operation gate; any intervening commit, database reset, or close/reopen invalidates the cursor and returns `conflict`, so restart inspection. Cursors also cannot be used with another database handle, even at the same revision. Inspection is a bounded metadata scan: it can return `limitExceeded` on a large database rather than allocate an unbounded snapshot. It reads cold payloads through the normal verified storage path.

Invalid NGQL displays the actual error and clears old results and inspection state. A stopped/unreachable runner reports unavailable. If a connection fails while a write is running, the outcome can be unknown; inspect before retrying. Reload the page after a runner restart to obtain the new session credentials. The app never falls back to simulated success.

## Local HTTP contract

The server binds only `127.0.0.1`. It verifies an exact Host and, on every `/dev/*` request, the matching Origin and a random session token injected into the served app. It sends no permissive CORS headers, disallows framing and disables caching of credentials/results. This protects the development database against web pages on other origins; it is not authentication against other programs running as the local user.

All API calls use POST, `Content-Type: application/json` and `X-Nova-Session`. Query bodies contain `query` and an optional `parameters` array. Each parameter has an explicit `name`, scalar `type` (`null`, `bool`, `int`, `double` or `string`) and `value`. For example, `{"query":"upsert node N a set value=$value","parameters":[{"name":"value","type":"double","value":1.0}]}` preserves a double even when it is mathematically integral. Integers must fit signed 64-bit range; doubles must be finite. Invalid types/values, duplicate or canonically equivalent names, and untagged parameter dictionaries are rejected before executing any statements. `/dev/session` reports session mode/path; `/dev/query` returns the v2 engine envelope; `/dev/inspect/nodes`, `/dev/inspect/edges` and `/dev/inspect/indexes` accept optional `cursor` and `limit`; `/dev/reset` resets disposable sessions only. HTTP errors also use `{schemaVersion:2,ok:false,error:{code,message,context}}`. A deadline before admission returns `timeout`; after admission it returns `outcomeUnknown` and warns that the operation may have committed. Cancelling the request does not imply rollback. Inspect state before retrying. One request owns each connection: extra pipelined input is ignored while that request completes, then the connection closes. No API redirects or automatic retry of writes are used.

Limits: 16 connections, one admitted operation (busy requests receive 503), 64 KiB request bodies, 16 KiB header lists/32 fields, 10 seconds per HTTP connection, a five-second native request deadline, 1 MiB native result budget and 1,000 query results. Native depth/work/statement ceilings still apply. The inspector defaults to 50 and caps pages at 100. Static assets are confined to the built web root. Source/package files and paths outside that directory are not served.

The CLI helper fetches credentials from this same loopback origin for each explicit invocation:

```sh
./tools/dev-query.sh 'get node user:ada' 8080
```

It performs a real write if given mutation text. Quotes and shell-looking text are passed as data, not interpolated into Python source.

## Verification

`make dev-query-test` builds the tool/app, checks the named HTTP inventory, runs real HTTP security/query/inspection/reset/restart tests plus two actual-engine transport outcome regressions, and runs browser-component integration against the actual engine. The live tests compare a preset response to the same CLI fixture, execute edited NGQL, render paths/projections/plans, exercise pagination and check error/disconnect behavior using a real dropped TCP connection. Transport outcome tests hold delivery of a real committed response across a deadline or pipelined request; they verify the stored mutation and response semantics without simulating engine results. `make docs-check` separately verifies the static read-only view, controls, parity and production build. Local results do not certify every browser, operating system, Apple device or production deployment.
