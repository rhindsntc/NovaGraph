# Apple lifecycle, cancellation and snapshots

Keep one database and one `@MainActor GraphLifecycle` coordinator in the app owner. Await asynchronous storage APIs from UI tasks; native work and decoding run on a utility queue. Call `send(.background)`, `.foreground` or `.memoryPressure` from lifecycle callbacks and handle the returned task's error. Events run in submission order, so a foreground event waits for an earlier suspend. The coordinator retains work until completion. Configure multiple-window apps at the application ownership level.

<!-- snippet: swiftui-lifecycle -->

This SwiftUI source is compiled by `make lifecycle-example`. On UIKit, `send(.background)` synchronously requests a background assertion before scheduling suspend. It ends the assertion on completion or expiration, cancelling the request on expiration. A refused assertion reports cancellation. This follows Apple's [background execution contract](https://developer.apple.com/documentation/uikit/uiapplication/beginbackgroundtask(expirationhandler:)). There is no guaranteed number of background seconds. App extensions need their own platform execution grant; this UIKit application helper is not an extension API.

Memory warnings trigger a forced, bounded trim to zero eligible hot payload bytes without waiting for TTL. Pinned payloads, metadata and indexes can remain resident; `GraphTrimResult` reports unmet bytes. macOS applications can route a [Dispatch memory-pressure source](https://developer.apple.com/documentation/dispatch/dispatchsourcememorypressure) to the same `.memoryPressure` event. The coordinator does not install global observers automatically.

## Request and snapshot example

<!-- snippet: swift-lifecycle -->

`make lifecycle-example` runs this exact command-line source and compiles the view above. An async `readSnapshot` dispatches its synchronous callback off the caller's actor; the callback itself cannot await. All reads share one gate, deadline and cumulative query budgets. Do not wait for other database work inside it, retain the snapshot for later, or move it across threads. The Swift wrapper rejects expired references; raw C callbacks must honor the borrowed lifetime.

Async Task cancellation reaches an independent native token. A synchronous caller can pass a fresh `GraphRequest` and cancel it from another thread. Cancellation is checked in gate waits, parsing, scans, traversal, between bounded IO steps and during decoding. Deadline starts at token creation, includes queuing, and is capped by the configured query timeout (maximum 5000 ms). A Swift read reports cancellation after decoding if it arrives during a decoder call. Cancellation before WAL append prevents the write; after append begins the durable protocol returns a receipt or `commitOutcomeUnknown`. Never blindly retry ambiguous writes.

## Explicit checkpoint

Both `try db.checkpoint()` and `try await db.checkpoint()` use native maintenance, with the same workspace policy as close/suspend. Async checkpoint checks cancellation/deadline while queued and before starting, then reports the completed storage outcome without a late cancellation override. Explicit NGQL `checkpoint` retains query budgets and may reject work that the maintenance APIs allow. See [checkpoint APIs](maintenance.md#checkpoint-apis) for the memory-policy boundary.

## Closing and background work

`try await db.close()` rejects new work, cancels/drains admitted operations, stops the worker, checkpoints and releases the directory lock. It retains success or failure for repeated calls, and never retries a failed final checkpoint during deinit. `GraphDatabase` can remain alive after close; later queries fail with `closed`. Use explicit close before deleting, backing up offline, or reopening a database. Deinit performs best-effort synchronous cleanup and cannot report errors.

Suspend defaults to a 1000 ms drain deadline, retains directory ownership and rejects normal queries with `busy` until resume. Bounded pressure trim remains permitted while fully suspended, participates in close draining, and never reopens normal query admission. Close defaults to 5000 ms. Both are limited by configured query timeout. If drain expires, admission stays closed/suspended, resources stay alive, and the caller can retry after the active callback/IO returns. Resume can deliberately reopen admission after a suspended drain timeout. Checkpoint failure is reported; suspend remains suspended until resume. Checkpoint failure may also fence writes until close/reopen.

Deadlines are cooperative, not hard execution limits. One blocking OS operation, Foundation decoding call or arbitrary callback cannot be preempted. Once commit or lifecycle checkpoint starts, its safe storage protocol finishes. Applications must keep snapshot callbacks short and must not depend on background shutdown to make already acknowledged commits durable. Local checks do not replace Apple device lifecycle testing or later production release qualification.

## Runnable application owner

The [Apple sample](../../examples/NovaGraphApp/README.md) combines this protocol with view-owned task cancellation, visible errors, commit receipts and explicit close. Its actual modifier is compiled with the documentation examples and exercised by app acceptance:

<!-- snippet: sample-lifecycle -->

The sample owner retains one coordinator and handles each task result. Pressure/background controls inject the same events used by platform callbacks. Simulator injection demonstrates wiring and engine behavior; it does not replace physical-device pressure or background-expiration qualification.
