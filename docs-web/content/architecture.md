# Embedded Architecture

NovaGraph runs in an application process with a Swift API, C ownership boundary and custom C++20 graph engine.

## Components

```text
Swift app → GraphDBKit → CGraphDB → GraphEngine
                                 ├─ NGQL lexer/parser/executor
                                 ├─ MemoryHotStore and indexes
                                 ├─ FileDiskStore cold records
                                 ├─ WalManager and Catalog
                                 └─ inactivity sweeper
```

The separate NovaDevQuery package uses SwiftNIO for local developer requests. It is not part of the embedded production API. There is no etcd, gRPC, cluster routing or distributed server control plane.

## State and concurrency

Nodes have IDs/labels/properties; edges have type/from/to/properties. The current internal edge key encodes identity components as length-prefixed tuples. Label/property and inbound/outbound indexes resolve candidate records; cold payloads promote on read.

An engine operation gate serializes individual queries, atomic writes, checkpoints and sweeps. Native RAII, C borrowed callbacks and Swift synchronous snapshot closures retain that gate across reads. Request tokens carry cancellation/deadlines independently of the gate. Retained C operation leases keep the engine alive until explicit close drains admitted work; Swift async operations await utility-queue completion. See [lifecycle and snapshot contracts](graph-database.md#async-and-lifecycle).

## Durability and product boundary

Schema-3 records and catalogs, version-4 transaction-framed WAL segments and CURRENT generation roots implement the storage protocol. Native, C and typed Swift errors preserve failures instead of silently returning empty data. Atomic publication and bounded recovery checks are implemented, but finite test coverage does not establish production readiness.

Read [storage](storage-engine.md), [API](graph-database.md), [NGQL](ngql-syntax.md), [file format](file-format.md) and [compatibility](compatibility.md) for current behavior and limitations.
