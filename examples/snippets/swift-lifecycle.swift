// Canonical runnable source: examples/Lifecycle/Sources/Lifecycle/LifecycleExample.swift
// Canonical SwiftUI integration: examples/Lifecycle/Sources/Lifecycle/DatabaseLifecycleView.swift
try await db.checkpoint()
let trim = try await db.handleMemoryWarning()
try await db.suspend(timeoutMilliseconds: 1_000)
try await db.resume()
try await db.close()
