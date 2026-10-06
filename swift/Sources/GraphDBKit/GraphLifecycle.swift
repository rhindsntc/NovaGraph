import CGraphDB
import Foundation

public struct GraphTrimResult: Decodable, Sendable, Equatable {
    public let evicted: Int
    public let evictedBytes: UInt64
    public let pinnedBytes: UInt64
    public let unmetBytes: UInt64
}

extension GraphDatabase {
    func makeRequest(_ options: GraphQueryOptions? = nil) throws -> GraphRequest {
        if let options { _ = try options.native() }
        return try GraphRequest(timeoutMilliseconds: min(queryOptions.timeoutMilliseconds, options?.timeoutMilliseconds ?? 5_000))
    }
    private func lifecycleRequest(_ timeoutMilliseconds: Int) throws -> GraphRequest {
        guard (1...5_000).contains(timeoutMilliseconds) else {
            throw GraphCoreError(code: "invalidArgument", message: "Lifecycle timeout must be 1...5000 ms", context: nil)
        }
        return try GraphRequest(timeoutMilliseconds: min(timeoutMilliseconds, queryOptions.timeoutMilliseconds))
    }
    /// Stops new work, cancels/drains operations and reports checkpoint failure.
    /// Repeated calls retain the final status. A drain timeout may be retried.
    public func close(timeoutMilliseconds: Int = 5_000) throws {
        let request = try lifecycleRequest(timeoutMilliseconds)
        _ = try decodeV2(responseData(graphdb_close_v2(handle, request.handle)))
    }
    public func close(timeoutMilliseconds: Int = 5_000) async throws {
        let request = try lifecycleRequest(timeoutMilliseconds)
        try await runGraphOperation(request: request) {
            _ = try decodeV2(responseData(graphdb_close_v2(self.handle, request.handle)))
        }
    }
    public func suspend(timeoutMilliseconds: Int = 1_000) async throws {
        let request = try lifecycleRequest(timeoutMilliseconds)
        try await runGraphOperation(request: request) {
            _ = try decodeV2(responseData(graphdb_suspend_v2(self.handle, request.handle)))
        }
    }
    public func resume() async throws {
        let request = try makeRequest()
        try await runGraphOperation(request: request) {
            _ = try decodeV2(responseData(graphdb_resume_v2(self.handle, request.handle)))
        }
    }
    public func trimMemory(to targetBytes: UInt64 = 0) async throws -> GraphTrimResult {
        let request = try makeRequest()
        return try await runGraphOperation(request: request) {
            try decodeV2(responseData(graphdb_trim_memory_v2(self.handle, targetBytes, request.handle))).decode(as: GraphTrimResult.self)
        }
    }
    public func handleMemoryWarning() async throws -> GraphTrimResult { try await trimMemory(to: 0) }
    /// Uses the same maintenance policy as synchronous checkpoint and close.
    /// Cancellation/deadline applies before work starts; storage completion is not interrupted.
    public func checkpoint() async throws {
        let request = try makeRequest()
        try await runGraphOperation(request: request) {
            _ = try decodeV2(responseData(graphdb_checkpoint_v2(self.handle, request.handle)))
        }
    }
}
