import Foundation
import Combine
import GraphDBKit

@MainActor public final class SampleModel: ObservableObject {
    public enum State: String { case closed, opening, ready, suspended, closing, failed }
    public enum Action { case open, save(String, String), connect(String, String), query(String), paths(String), trim, close }
    @Published public private(set) var state: State = .closed
    @Published public private(set) var nodes: [Node] = []
    @Published public private(set) var paths: [GraphDBKit.Path] = []
    @Published public private(set) var output = ""
    @Published public private(set) var error: String?
    @Published public private(set) var status = "Closed"
    @Published public private(set) var busy = false
    @Published public private(set) var lastCommit: String?
    public let path: URL
    private let opener = DatabaseOpener()
    private let openDatabase: @Sendable (URL) async throws -> GraphDatabase
    private var database: GraphDatabase?
    private var coordinator: GraphLifecycle?
    private var work: Task<Void, Never>?
    private var lifecycleWork: Task<Void, Never>?
    private var desiredPhase: GraphLifecycle.Event = .foreground
    private var currentCommit: String?
    private var pendingCleanupError: String?
    public var canOpen: Bool { database == nil && [.closed, .failed].contains(state) }

    public convenience init(path: URL) {
        let opener = DatabaseOpener()
        self.init(path: path, openDatabase: { try await opener.open($0) })
    }
    init(path: URL, openDatabase: @escaping @Sendable (URL) async throws -> GraphDatabase) {
        self.path = path; self.openDatabase = openDatabase
    }

    /// One view-owned operation at a time; closing cancels and drains that owner.
    @discardableResult public func perform(_ action: Action) -> Task<Void, Never> {
        if case .close = action { return close() }
        guard !busy, state != .closing else { return work ?? Task {} }
        if case .open = action {
            guard database == nil else { return Task {} }
            state = .opening
        } else if state != .ready { return Task {} }
        busy = true; error = nil; currentCommit = nil; status = "Working"
        let task = Task {
            do {
                try Task.checkCancellation()
                try await execute(action)
            } catch {
                if self.currentCommit != nil {
                    self.status = "Committed; refresh unavailable"
                    self.error = "The write committed. Refresh separately; do not repeat it. " + error.localizedDescription
                } else if error is CancellationError || (error as? GraphCoreError)?.code == "cancelled" {
                    self.status = "Cancelled"
                } else {
                    self.error = error.localizedDescription; self.status = "Operation failed"
                }
                if case .open = action {
                    if let failure = await finishOwnership() {
                        self.error = failure.localizedDescription; self.status = "Close failed"
                        if state == .closing { pendingCleanupError = self.error }
                    }
                    if state != .closing { state = self.error == nil && database == nil ? .closed : .failed }
                }
            }
            if state != .closing { busy = false }
        }
        work = task
        return task
    }

    private func execute(_ action: Action) async throws {
        if case .open = action {
            let opened = try await openDatabase(path)
            database = opened
            try Task.checkCancellation()
            coordinator = GraphLifecycle(database: opened)
            nodes = try await opened.nodes(label: "Person", limit: 100)
            state = .ready; status = "Ready"
            await lifecycle(desiredPhase).value
            return
        }
        guard let database else { return }
        switch action {
        case .save(let id, let name):
            guard !id.isEmpty, id.utf8.count <= 256, name.utf8.count <= 4096 else { throw SampleError.invalidInput }
            let receipt = try await database.mutate("upsert node Person \(GraphValue.string(id).ngqlLiteral) set name=$name", parameters: ["name": .string(name)])
            record(receipt)
            nodes = try await database.nodes(label: "Person", limit: 100)
            status = "Saved"
        case .connect(let from, let to):
            guard !from.isEmpty, !to.isEmpty, from.utf8.count <= 256, to.utf8.count <= 256 else { throw SampleError.invalidInput }
            let receipt = try await database.mutate("upsert edge KNOWS \(GraphValue.string(from).ngqlLiteral) -> \(GraphValue.string(to).ngqlLiteral)")
            record(receipt); status = "Connected"
        case .query(let query):
            guard query.utf8.count <= 65_536 else { throw SampleError.invalidInput }
            let result = try await database.queryResult(query)
            if let receipt = result.receipt { record(receipt) }
            // Formatting up to the bounded result ceiling also stays off the UI actor.
            output = try await Task.detached {
                let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
                return String(decoding: try encoder.encode(result.data), as: UTF8.self)
            }.value
            nodes = try await database.nodes(label: "Person", limit: 100)
            status = result.receipt == nil ? "Query complete" : "Committed"
        case .paths(let start):
            let result = try await database.paths(from: start, over: "KNOWS", depth: 8, limit: 100)
            try Task.checkCancellation()
            paths = result.paths; status = "Found \(paths.count) paths"
        case .trim:
            let result = try await database.handleMemoryWarning()
            status = "Evicted \(result.evicted) records"
        case .open, .close: break
        }
    }

    private func record(_ receipt: GraphMutationReceipt) {
        currentCommit = receipt.transactionId; lastCommit = receipt.transactionId
    }

    public func cancel() { if state != .closing { work?.cancel() } }

    /// Submit synchronously from scene/notification callbacks so UIKit can obtain
    /// its background assertion before this callback returns.
    @discardableResult public func lifecycle(_ event: GraphLifecycle.Event) -> Task<Void, Never> {
        if event != .memoryPressure { desiredPhase = event }
        guard let coordinator, [.ready, .suspended].contains(state) else { return Task {} }
        if event == .background { state = .suspended }
        let operation = coordinator.send(event)
        let previous = lifecycleWork
        let task = Task {
            if let previous { await previous.value }
            do {
                try await operation.value
                guard state != .closing else { return }
                switch event {
                case .background: state = .suspended; status = "Suspended"
                case .foreground: state = .ready; status = "Ready"; error = nil
                case .memoryPressure: status = "Memory pressure handled"
                }
            } catch {
                guard state != .closing else { return }
                self.error = error.localizedDescription
                if event != .memoryPressure {
                    // Suspend closes admission before draining/checkpointing. Resume
                    // is safe even when an expiration happened before admission closed.
                    state = .suspended; status = "Lifecycle failed; Resume available"
                }
            }
        }
        lifecycleWork = task
        return task
    }

    /// Await an independent cleanup task: view cancellation cannot cancel close.
    /// Only drain cancellation/timeouts retain ownership for an explicit retry.
    private func finishOwnership() async -> (any Error)? {
        guard let database else { return nil }
        let result = await Task { try await opener.close(database) }.result
        if case .failure(let failure) = result,
           let code = (failure as? GraphCoreError)?.code,
           ["deadlineExceeded", "cancelled", "conflict"].contains(code) {
            return failure
        }
        coordinator = nil; self.database = nil; lifecycleWork = nil
        if case .failure(let failure) = result { return failure }
        return nil
    }

    private func close() -> Task<Void, Never> {
        if state == .closing { return work ?? Task {} }
        let previous = work, pendingLifecycle = lifecycleWork
        previous?.cancel(); busy = true; state = .closing
        let task = Task {
            if let previous { await previous.value }
            if let pendingLifecycle { await pendingLifecycle.value }
            let failure = await finishOwnership()
            if let message = pendingCleanupError ?? failure?.localizedDescription {
                pendingCleanupError = nil
                error = message; state = .failed; status = "Close failed"
            } else {
                nodes = []; paths = []; output = ""
                state = .closed; status = "Closed"; error = nil
            }
            busy = false
        }
        work = task
        return task
    }
}

private enum SampleError: LocalizedError {
    case invalidInput
    var errorDescription: String? { "Enter a nonempty ID (up to 256 UTF-8 bytes), a name up to 4 KiB, and a query up to 64 KiB." }
}
