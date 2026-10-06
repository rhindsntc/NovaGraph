import Foundation
#if os(iOS) || os(tvOS)
import UIKit
#endif

/// Keep one coordinator for each database. Call send from SwiftUI scene changes
/// and memory-warning notifications; await each returned task to handle failures.
/// Events execute in submission order and all storage work runs off the UI actor.
@MainActor public final class GraphLifecycle {
    public enum Event: Sendable { case background, foreground, memoryPressure }
    private let database: GraphDatabase
    private let backgroundTimeoutMilliseconds: Int
    private var tail: Task<Void, Error>?

    public init(database: GraphDatabase, backgroundTimeoutMilliseconds: Int = 1_000) {
        self.database = database
        self.backgroundTimeoutMilliseconds = backgroundTimeoutMilliseconds
    }

    public func send(_ event: Event) -> Task<Void, Error> {
        let previous = tail, database = database, timeout = backgroundTimeoutMilliseconds
        #if os(iOS) || os(tvOS)
        // Obtain the platform assertion synchronously, before dispatching work.
        let background = event == .background ? BackgroundExecution() : nil
        #endif
        let task = Task {
            if let previous { _ = await previous.result }
            #if os(iOS) || os(tvOS)
            defer { background?.finish() }
            if background?.expired == true { throw GraphCoreError(code: "cancelled", message: "Background execution expired", context: nil) }
            #endif
            switch event {
            case .background: try await database.suspend(timeoutMilliseconds: timeout)
            case .foreground: try await database.resume()
            case .memoryPressure: _ = try await database.handleMemoryWarning()
            }
        }
        #if os(iOS) || os(tvOS)
        background?.task = task
        #endif
        tail = task
        return task
    }
}

#if os(iOS) || os(tvOS)
@MainActor private final class BackgroundExecution {
    private var identifier: UIBackgroundTaskIdentifier = .invalid
    var task: Task<Void, Error>?
    private(set) var expired = false
    init() {
        identifier = UIApplication.shared.beginBackgroundTask(withName: "NovaGraph suspend") { [weak self] in
            guard let self else { return }
            self.expired = true
            self.task?.cancel()
            self.finish()
        }
        if identifier == .invalid { expired = true }
    }
    func finish() {
        task = nil
        guard identifier != .invalid else { return }
        UIApplication.shared.endBackgroundTask(identifier)
        identifier = .invalid
    }
}
#endif
