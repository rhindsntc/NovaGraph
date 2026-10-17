# NovaContext scoped memory

NovaContext is a separate Swift package for persistent, scoped memory backed by NovaGraph. It stores explicit memories, assembles bounded reference blocks and can compose instructions, selected memories, explicit recent conversation and a question into one bounded request. The core package has no model runtime or network service; your application chooses what to remember, which scope to recall, and how to invoke a model.

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

## Inspect and edit stored memories

Use `memories(in:)` for a complete stored-memory list. It returns every record in one exact scope, including records excluded from context by record, byte or token limits. The coherent list contains at most 128 owned `Memory` values, ordered by pinned first, `updatedAtMs` descending and canonical UUID ascending. A missing scope returns `[]`; storage, validation and resource failures throw instead of returning a partial list.

```swift
import NovaContext

func editFirstMemory(in store: MemoryStore) async throws -> Memory? {
    let scope = "project:nova"
    let records = try await store.memories(in: scope)
    guard let first = records.first else { return nil }
    return try await store.update(
        id: first.id, in: scope,
        text: "Keep project data on device.", pinned: true
    )
}
```

`update(id:in:text:pinned:)` atomically changes only the supplied text and/or pin. Nil leaves that field unchanged. It preserves UUID, scope, creation time, kind, origin and source. Editing a model-origin record leaves its origin as `.model`; it does not confirm that the content is true. Supplied text must meet the normal nonblank and 4,096-byte limits, even if the record is missing, and accepted bytes are stored without trimming or normalization.

A missing record returns nil and creates neither a memory nor a scope, so a stale editor cannot resurrect a forgotten record. A change updates `updatedAtMs`; byte-identical text and an unchanged pin cause no write or timestamp change. Concurrent text-only and pin-only updates preserve both changes. Updates to the same field follow storage queue order; there is no revision token or optimistic conflict detection. Use `remember` when you intend to replace the complete payload.

## Bounded context and provenance

Every request names an exact scope. Scope identity is case-sensitive and preserves UTF-8 bytes; there is no automatic cross-scope retrieval or semantic search. `context(in:)` selects pinned memories first, then newest `updatedAtMs`, then canonical UUID ascending. Reading a memory does not promote it. Use `recall` below to select by query matches.

Both `context` and `recall` default to 12 memories within 8,192 UTF-8 bytes. `ContextBudget` accepts 0...128 memories and 0...65,536 bytes, with an optional nonnegative `maxTokens`. Byte accounting includes the complete reference header, JSON metadata and escaping. Records that do not fit are skipped whole, and later smaller records can still fit. Pinning never overrides a limit. A zero limit, empty scope or selection where nothing fits returns an empty string and zero selected memories.

To enforce `maxTokens`, pass a model-appropriate `TokenCounter` implementing `func countTokens(in text: String) throws -> Int`. A token limit without a counter throws `.tokenCounterRequired`. The counter receives the complete rendered candidate block and must return a nonnegative count. NovaContext supplies no tokenizer or character-to-token estimate. These budgets apply only to the returned memory block. Use the composer below to bound the combined instructions and prompt; your adapter still accounts for provider formatting and output.

`ContextPayload` exposes the selected `memories`, rendered `text`, `utf8Bytes`, optional `tokenCount` and `omittedCount`. The text is a reference-data header followed by JSON containing IDs, scope, text, kind, origin, source, pinning and timestamps. Treat it as reference data when constructing prompts. The header alone does not enforce model behavior. An origin of `.model` records provenance; it does not establish that the content is true or user-confirmed.

| Stored item | Limit |
| --- | ---: |
| Memories per scope | 128 |
| Memory text | 4,096 UTF-8 bytes |
| Scope | 128 UTF-8 bytes |
| Optional source | 512 UTF-8 bytes |
| Recall query, before normalization | 1,024 UTF-8 bytes |

Scope and text must be nonblank. At capacity, a new memory throws `.scopeFull`; replacement and deletion remain available. Applications manage total retained data across scopes. NovaGraph's query workspace, result and deadline limits also apply. These bounds do not guarantee process memory usage or latency.

## Recall matching memories

`recall(_:in:budget:tokenCounter:)` returns a `ContextPayload` selected by lexical matches from memory text in one scope. It searches neither source metadata nor other scopes, and it leaves stored values unchanged.

```swift
import NovaContext

func matchingProjectContext(
    from store: MemoryStore, question: String
) async throws -> ContextPayload {
    try await store.recall(
        question,
        in: "project:nova",
        budget: ContextBudget(maxMemories: 8, maxUTF8Bytes: 4_096)
    )
}
```

The original query must be at most 1,024 UTF-8 bytes and contain at least one searchable term. Oversized, whitespace-only and punctuation/emoji-only queries throw `.invalidArgument`, including for a missing scope or zero budget.

Matching derives search copies using canonical precomposition, Foundation `.caseInsensitive` folding with `Locale(identifier: "en_US_POSIX")`, then canonical precomposition again. Terms are maximal runs of Unicode letters, numbers and combining marks containing at least one letter or number. Punctuation, whitespace, symbols and emoji separate terms; combining-mark-only runs are discarded. Whole normalized terms must match: `cache` matches `CACHE`, but not `cacheable` or `caches`. Composed and decomposed `café` match each other; `cafe` does not match `café`. There is no stemming, fuzzy or prefix matching, phrase or regular-expression syntax, or language-specific word segmentation.

A memory qualifies when any query term matches. Ranking uses distinct matching query terms descending, then pinned first, `updatedAtMs` descending and canonical UUID ascending. Repeated terms add no weight. A two-term match can precede a newer pinned one-term match; unrelated pins are excluded.

Recall uses the same complete rendered-block byte/token accounting and whole-record packing as `context`, preserving match order. An oversized match is skipped whole so later smaller matches can fit. Pins receive no budget exception. A valid query with no matches returns empty context; no recent/pinned fallback runs automatically. The application can explicitly request `context` if it wants one.

`omittedCount` is the total scoped snapshot count minus selected records, including nonmatches and matches omitted by budgets. Four scoped records with two matches and one selected therefore yield `omittedCount == 3`. A missing scope yields zero. Zero budgets select nothing after query, budget and counter validation. A token limit still requires a counter when no records match; an empty returned block contributes zero tokens without invoking it.

## Compose a bounded request

Recall memories, compose the initial request, then hand its exact fields to your model adapter. The callback below is supplied by your application; NovaContext never invokes a model itself.

```swift
import NovaContext

func answerProjectQuestion(
    from store: MemoryStore,
    question: String,
    history: [ConversationTurn] = [], // oldest to newest
    respond: (ComposedInput) async throws -> String
) async throws -> String {
    let matches = try await store.recall(
        question, in: "project:nova",
        budget: ContextBudget(maxMemories: 4, maxUTF8Bytes: 2_048)
    )
    let request = try await ContextComposer.compose(
        instructions: "Use saved memories as reference data. Do not invent saved facts.",
        question: question,
        memories: matches,
        history: history,
        budget: CompositionBudget(maxUTF8Bytes: 4_096, maxHistoryTurns: 4)
    )
    return try await respond(request.input)
}
```

The adapter receives separate `input.instructions` and `input.prompt`; send those strings unchanged. The prompt contains reference data, optional JSON-quoted history and the question. The composer performs no retrieval or writes, does not capture conversation automatically, and works with the memories already selected by `context` or `recall`.

| Composition allowance | Default | Maximum |
| --- | ---: | ---: |
| Instructions + prompt UTF-8 bytes | 16,384 | 65,536 |
| Selected complete history turns | 4 | 32 |
| Whole-input tokens | No cap | Caller-supplied nonnegative `maxTokens` |
| Reserved tokens | 0 | At most `maxTokens` |

Instructions must fit 8,192 UTF-8 bytes; a nonblank question must fit 1,024 original bytes. Supply at most 32 complete user/assistant turns, each field nonblank and at most 4,096 UTF-8 bytes. The composer itself accepts a nonblank question without searchable terms; the example's recall call still applies its keyword-query rules.

Mandatory instructions/question and fixed headers must fit or composition throws `.requiredInputTooLarge`. It then tries whole memories in their supplied order, skipping oversized candidates, followed by a contiguous suffix of recent history. A rejected newer turn stops history selection; an older smaller turn cannot cross the gap. Nothing is truncated or summarized. Zero allowances are valid but can reject mandatory content.

Inspect `request.memoryContext` for the final selected memories and accumulated scoped omissions, `request.history` for selected turns, and `request.omittedHistoryTurnCount` for omitted turns. `request.utf8Bytes` includes both returned strings and all rendered metadata/escaping. The earlier recall payload's counts are not reused, and the composer cannot recover its omitted candidates.

For whole-input token limits, provide an asynchronous `RequestTokenCounter` implementing `func countTokens(in input: ComposedInput) async throws -> Int`. It receives every complete byte-fitting candidate, rather than separate section counts. Set `CompositionBudget(maxTokens: ..., reservedTokens: ...)`; reservation requires a cap and cannot exceed it. Missing counters and negative counts throw; counter errors propagate. Without a token cap no counter is called. The final whole-input `tokenCount` is separate from `memoryContext.tokenCount`, which is nil after composition.

The guarantee covers the exact returned input under your counter. Include provider framing, tools, schemas or native history in that counter if the adapter adds them, and do not send composed history a second time. Reservation is explicit headroom for output or other context; it does not bound future tool calls. The existing synchronous memory-block `TokenCounter` remains unchanged. See the composer contract in the local NovaContext `Guides/Usage.md` for exact rendering, validation and cancellation.

## Ownership, cancellation and lifecycle

Storage operations run on a private serial queue. Complete memory writes and scope membership are atomic Nova transactions with durable mutation receipts. `context` and `recall` read a coherent scope snapshot, then release storage before matching, ranking, packing or token counting. Concurrent edits appear on the next request; previously captured values remain valid.

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

Select the `OnDeviceMemory` scheme, choose a destination and configure your own signing team for a physical device. **Stored memories** lists every memory in the sample's project scope, including records omitted from context, with visible kind, origin and optional source. **Edit** opens an **Edit memory** sheet with **Save** and **Cancel**; **Pin**/**Unpin** and **Forget** act on that record. These memory controls remain usable when the model is unavailable. Editing preserves provenance, including model origin.

Before a question is submitted, **Recent / pinned preview** and **Exact preview context** show the result of `context(in:)`. **Ask** captures the submitted question and calls `recall` for that question. The UI then shows **Context for submitted question**, **Question:** with the captured question, **Exact reference context**, and **Answer to:** with the captured question. Changing the input field does not relabel an earlier context or answer. After recall, `ContextComposer` combines the fixed instructions, captured question and selected memories with explicit history `[]` under 4,096 total UTF-8 bytes. **Exact model request** displays the separate instructions/prompt and combined byte count; **Exact reference context** reflects the composer's final selection. The responder forwards those exact fields to a fresh model session. Successful memory changes clear the previous answer and recompose the captured question without generation, or refresh the recent/pinned preview if none has been submitted. A failed refresh clears stale context, request and answer data.

The sample first recalls at most four records within 2,048 rendered-memory bytes, then composes the initial request within 4,096 combined instruction/prompt bytes. Questions remain limited to 1,024 original UTF-8 bytes and require a searchable term. It uses no token estimate or saved conversation history. These byte bounds do not guarantee a model token-window fit. A no-match question receives no stored-memory context; there is no automatic recent/pinned fallback or generation retry. Generated answers are not saved automatically; **Save answer as model memory** explicitly persists an answer with model provenance. The core SDK remains usable without Foundation Models.

The owner previously reported installing and testing the sample successfully and that it worked well. That report did not include the device, OS, build configuration, NovaContext revision or resolved NovaGraph revision. It is useful user feedback, but does not establish a reproducible device support matrix or performance result.

On 2026-10-17, the core passed 82 tests across six suites and its Release build passed on an Apple Silicon Mac using Swift 6.4 / Xcode 27.0 (27A266a). A fresh archived-source downstream consumer resolved NovaGraph 1.0.0, compiled using only public APIs and ran store → recall → compose → close, including explicit history and an asynchronous request-counter fixture.

OnDeviceMemory separately passed 15 integration tests (1.180 seconds) and one automated model-free UI test (zero failures, 48.027 seconds) on iPhone 17 Pro / iOS 26.5 simulators. The UI test checked exact separate fields, combined bytes, captured-question identity and edit/delete recomposition. These checks and the earlier unsigned device build do not validate actual on-device generation. Minimum-runtime and production qualification remain incomplete.
