# Overview

NovaGraph is an **experimental** embedded property graph database for Swift applications. A C++20 engine provides storage and NGQL execution behind a C ABI and the GraphDBKit Swift module. It is not production-ready; use disposable data while evaluating it.

The API supports nodes and edges, property indexes, deterministic shortest-hop traversal, flat Codable properties, typed scalar values and errors, atomic mutation batches, request cancellation, read snapshots and explicit close. Storage combines hot in-memory payloads with cold binary records, a transaction write-ahead log and retained catalog generations.

Start with the [quickstart](quickstart.md), then read the [Swift API](graph-database.md) and [NGQL reference](ngql-syntax.md). The browser playground shows read-only recordings from executable examples. An opt-in [local developer runner](developer-tools.md) executes queries against a real development database.

Declared Apple deployment targets are separate from tested runtime support. See [compatibility and limitations](compatibility.md) before selecting a platform or opening existing data. Physical-device, oldest-runtime and production-release qualification remain incomplete.

The [open-source TODO roadmap](roadmap.md) lists launch preparation and unfinished production qualification. Experimental source availability does not complete those gates.
