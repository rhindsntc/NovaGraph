import CGraphDB
import Foundation

/// Valid only on the synchronous readSnapshot callback's thread. It cannot be
/// sent into a Task; a reference retained beyond the callback throws `closed`.
public final class GraphSnapshot {
    private let lock = NSLock()
    private var handle: OpaquePointer?
    private let request: GraphRequest
    init(_ handle: OpaquePointer, request: GraphRequest) { self.handle = handle; self.request = request }
    func invalidate() { lock.lock(); handle = nil; lock.unlock() }
    public func queryResult(_ ngql: String, parameters: [String: GraphValue] = [:]) throws -> GraphQueryResult {
        lock.lock(); defer { lock.unlock() }
        guard let handle else { throw GraphCoreError(code: "closed", message: "Snapshot callback has ended", context: nil) }
        try request.check()
        let params = Array(try scalarJSON(parameters).utf8) + [0], query = Array(ngql.utf8) + [0]
        let out = query.withUnsafeBufferPointer { q in
            params.withUnsafeBufferPointer { p in
                graphdb_snapshot_query_v2(handle,
                    UnsafeRawPointer(q.baseAddress!).assumingMemoryBound(to: CChar.self), UInt64(q.count - 1),
                    UnsafeRawPointer(p.baseAddress!).assumingMemoryBound(to: CChar.self), UInt64(p.count - 1))
            }
        }
        return try decodeV2(responseData(out), request: request)
    }
    public func query<T: Decodable>(_ ngql: String, parameters: [String: GraphValue] = [:], as type: T.Type) throws -> T {
        let value = try queryResult(ngql, parameters: parameters).decode(as: type)
        try request.check(); return value
    }
    public func node(id: String) throws -> Node? {
        do { return try queryResult("get node \(GraphValue.string(id).ngqlLiteral)").node() }
        catch let error as GraphCoreError where error.code == "notFound" { return nil }
    }
}

private class SnapshotCallback {
    func invoke(_ handle: OpaquePointer) { preconditionFailure("Abstract snapshot callback") }
}

private final class SnapshotBody<T>: SnapshotCallback {
    let body: (GraphSnapshot) throws -> T
    let request: GraphRequest
    var result: Result<T, Error>?
    init(request: GraphRequest, body: @escaping (GraphSnapshot) throws -> T) { self.request = request; self.body = body }
    override func invoke(_ handle: OpaquePointer) {
        let view = GraphSnapshot(handle, request: request)
        defer { view.invalidate() }
        result = Result { try body(view) }
    }
}

extension GraphDatabase {
    public func readSnapshot<T>(options: GraphQueryOptions? = nil, request: GraphRequest? = nil, _ body: (GraphSnapshot) throws -> T) throws -> T {
        let request = try request ?? makeRequest(options)
        let native = try options?.native()
        // The C callback is synchronous. The closure and borrowed C pointer are
        // invalidated before withoutActuallyEscaping returns.
        return try withoutActuallyEscaping(body) { callback in
            let box = SnapshotBody(request: request, body: callback)
            let opaque = Unmanaged.passUnretained(box as SnapshotCallback).toOpaque()
            let invoke: @convention(c) (OpaquePointer?, UnsafeMutableRawPointer?) -> Void = { view, context in
                guard let view, let context else { return }
                Unmanaged<SnapshotCallback>.fromOpaque(context).takeUnretainedValue().invoke(view)
            }
            let out: GraphDBString
            if var value = native { out = graphdb_read_snapshot_v2(handle, &value, request.handle, invoke, opaque) }
            else { out = graphdb_read_snapshot_v2(handle, nil, request.handle, invoke, opaque) }
            _ = try decodeV2(responseData(out), request: request)
            guard let result = box.result else { throw GraphDBError.decodingFailed("Snapshot callback was not called") }
            return try result.get()
        }
    }

    public func readSnapshot<T: Sendable>(options: GraphQueryOptions? = nil, _ body: @escaping @Sendable (GraphSnapshot) throws -> T) async throws -> T {
        let request = try makeRequest(options)
        return try await runGraphOperation(request: request) { try self.readSnapshot(options: options, request: request, body) }
    }
}
