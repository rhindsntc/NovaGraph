import CGraphDB
import Foundation

/// An independently owned cancellation token. Deadlines start at creation;
/// use a fresh request for each operation. Cancel never waits for the database.
public final class GraphRequest: @unchecked Sendable {
    let handle: OpaquePointer
    public init(timeoutMilliseconds: Int = 5_000) throws {
        guard (1...5_000).contains(timeoutMilliseconds) else {
            throw GraphCoreError(code: "invalidArgument", message: "Request timeout must be 1...5000 ms", context: nil)
        }
        guard let value = graphdb_request_create(UInt64(timeoutMilliseconds)) else {
            throw GraphCoreError(code: "ioFailure", message: "Could not allocate request", context: nil)
        }
        handle = value
    }
    deinit { graphdb_request_release(handle) }
    public func cancel() { graphdb_request_cancel(handle) }
    func check() throws {
        if graphdb_request_is_cancelled(handle) != 0 {
            throw GraphCoreError(code: "cancelled", message: "Request cancelled", context: nil)
        }
        if graphdb_request_is_expired(handle) != 0 {
            throw GraphCoreError(code: "deadlineExceeded", message: "Request deadline exceeded", context: nil)
        }
    }
}

private let graphOperations = DispatchQueue(label: "NovaGraph.operations", qos: .utility, attributes: .concurrent)

/// The continuation waits for native completion, retaining its owner and request.
/// Never replace a committed/ambiguous outcome with a late cancellation error.
func runGraphOperation<T: Sendable>(request: GraphRequest, _ action: @escaping @Sendable () throws -> T) async throws -> T {
    try await withTaskCancellationHandler {
        if Task.isCancelled { request.cancel() }
        return try await withCheckedThrowingContinuation { continuation in
            graphOperations.async {
                do { try request.check(); continuation.resume(returning: try action()) }
                catch { continuation.resume(throwing: error) }
            }
        }
    } onCancel: { request.cancel() }
}
