# NovaContext scoped memory

NovaContext is a separate Swift package for persistent, scoped memory backed by NovaGraph. It stores explicit memories and assembles a bounded reference block for your application. The core package has no model runtime or network service; your application chooses what to remember, which scope to recall, and how to invoke a model.

## Install from a local package

NovaContext currently has no published remote package URL or version tag. Obtain the NovaContext source directory and add it as a **local Swift package** in Xcode. For sibling SwiftPM projects, add this dependency:

```swift
.package(path: "../NovaContext")
```

Add its library product to your target dependencies:

```swift
.product(name: "NovaContext", package: "novacontext")
```

NovaContext uses Swift tools 6.0 and depends on the `GraphDBKit` product from NovaGraph, pinned to version 1.0.0. Its declared deployment floors are iOS 16 and macOS 13. Deployment declarations are separate from tested runtime support; see NovaGraph's [compatibility notes](compatibility.md).

The NovaContext checkout contains its own `README.md`, `Guides/Usage.md` and `Examples/OnDeviceMemory/README.md`. Read those files locally for package development and sample setup.

## Remember, build context and close

Use a dedicated directory under your application's Application Support location. One store/process owns that directory at a time. The store initializes only an empty database and rejects unrelated databases, unsupported schemas and malformed records it encounters.

```swift
import Foundation
import NovaContext

func rememberAndBuildContext(at directory: URL) async throws -> ContextPayload {
    let store = try await MemoryStore.open(at: directory)
    do {
        let draft = MemoryDraft(
            text: "Keep project data on device.",
            kind: .fact,
            origin: .user,
            source: "Project decision",
            pinned: true
        )
        _ = try await store.remember(draft, in: "project:nova")
        let context = try await store.context(
            in: "project:nova",
            budget: ContextBudget(maxMemories: 8, maxUTF8Bytes: 4_096)
        )
        try await store.close()
        return context
    } catch {
        try? await store.close()
        throw error
    }
}
```

Reopening the same directory returns previously committed memories. Keep a draft's UUID to update or reconcile it: remembering the same UUID in the same scope replaces the complete payload, preserves creation time and updates modification time. Omitting an earlier source or pin clears it. The same UUID can exist independently in another scope.

`memory(id:in:)` reads one scoped record; `forget(id:in:)` durably deletes it; `clear(scope:)` durably removes a scope and its records. Missing records or scopes make deletion a successful no-op. Nothing is automatically extracted, expired, evicted or summarized.

## Bounded context and provenance

Every request names an exact scope. Scope identity is case-sensitive and preserves UTF-8 bytes; there is no automatic cross-scope retrieval or semantic search. Context ranks pinned memories first, then newest `updatedAtMs`, then canonical UUID ascending. Reading a memory does not promote it.

The defaults are 12 memories within 8,192 UTF-8 bytes. `ContextBudget` accepts 0...128 memories and 0...65,536 bytes, with an optional nonnegative `maxTokens`. Byte accounting includes the complete reference header, JSON metadata and escaping. Records that do not fit are skipped whole, and later smaller records can still fit. Pinning never overrides a limit. A zero limit, empty scope or selection where nothing fits returns an empty string and zero selected memories.

To enforce `maxTokens`, pass a model-appropriate `TokenCounter` implementing `func countTokens(in text: String) throws -> Int`. A token limit without a counter throws `.tokenCounterRequired`. The counter receives the complete rendered candidate block and must return a nonnegative count. NovaContext supplies no tokenizer or character-to-token estimate. Budgets apply only to the returned block; reserve room and validate your complete model prompt separately.

`ContextPayload` exposes the selected `memories`, rendered `text`, `utf8Bytes`, optional `tokenCount` and `omittedCount`. The text is a reference-data header followed by JSON containing IDs, scope, text, kind, origin, source, pinning and timestamps. Treat it as reference data when constructing prompts. The header alone does not enforce model behavior. An origin of `.model` records provenance; it does not establish that the content is true or user-confirmed.

| Stored item | Limit |
| --- | ---: |
| Memories per scope | 128 |
| Memory text | 4,096 UTF-8 bytes |
| Scope | 128 UTF-8 bytes |
| Optional source | 512 UTF-8 bytes |

Scope and text must be nonblank. At capacity, a new memory throws `.scopeFull`; replacement and deletion remain available. Applications manage total retained data across scopes. NovaGraph's query workspace, result and deadline limits also apply. These bounds do not guarantee process memory usage or latency.

## Ownership, cancellation and lifecycle

Storage operations run on a private serial queue. Complete memory writes and scope membership are atomic Nova transactions with durable mutation receipts. Context reads a coherent scope snapshot, then releases storage before packing or token counting. Concurrent edits appear on the next context request; previously captured values remain valid.

The host application forwards lifecycle events. NovaContext imports neither SwiftUI nor UIKit:

```swift
// Given an active store:
try await store.trimMemory() // Evict cached records while preserving saved memories
try await store.suspend()    // Checkpoint and stop ordinary operations
try await store.resume()     // Restore access before reading or writing
try await store.close()      // Drain earlier work and end ownership
```

A suspended store rejects ordinary operations and trim with `.suspended`. A failed suspension also blocks access until successful resume or close. Repeated suspend retains the attempt's outcome; failed resume leaves the store suspended and can be retried. Serialize app lifecycle transitions and do not rely on background execution to make the first durable write. See the underlying [NovaGraph lifecycle contract](swiftui-lifecycle.md).

Close explicitly to observe storage errors. Close runs even from a cancelled task and retains its result for repeated calls. A closed instance rejects further operations with `.closed`; reopen using a new `MemoryStore`. Dropping an unclosed store only schedules best-effort cleanup.

Reads, writes and lifecycle operations forward cooperative cancellation. Completed or ambiguous commits retain their actual outcomes if cancellation arrives late. Native storage errors propagate unchanged. If NovaGraph reports `commitOutcomeUnknown`, preserve the UUID, scope and transaction ID, then close/reopen and inspect recovered data before deciding whether to retry. NovaContext does not automatically retry or guarantee exactly-once writes. See [transaction outcomes](transactions.md).

## Separate on-device sample and validation

The NovaContext checkout's `Examples/OnDeviceMemory` is a separate SwiftUI app requiring Xcode 26+, XcodeGen and iOS 26+. Real generation additionally needs an Apple Intelligence eligible device with Apple Intelligence enabled and the on-device model ready. The app reports availability; its labelled `--model-free` mode provides a walkthrough without invoking a model. If the model becomes ready while the app remains open, use **Check model availability** to enable **Ask** without a scene transition; this check performs no generation or retry.

From the NovaContext repository root:

```sh
xcodegen generate --spec Examples/OnDeviceMemory/project.yml
open Examples/OnDeviceMemory/OnDeviceMemory.xcodeproj
```

Select the `OnDeviceMemory` scheme, choose a destination and configure your own signing team for a physical device. The sample lets you remember decisions, inspect the exact selected context, ask Foundation Models, forget memories and explicitly save an answer with model provenance. Generated answers are not saved automatically. The core SDK remains usable without Foundation Models.

The owner reports installing and testing the sample successfully and that it worked well. That report did not include the device, OS, build configuration, NovaContext revision or resolved NovaGraph revision. It is useful user feedback, but does not establish a reproducible device support matrix or performance result. The sample's recorded simulator tests and unsigned device build are separate evidence; they do not validate actual on-device generation. Minimum-runtime and production qualification remain incomplete.
