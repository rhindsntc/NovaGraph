import CGraphDB
import Foundation
import CoreFoundation

public enum GraphDBError: Error, CustomStringConvertible, LocalizedError, Sendable {
    case openFailed(URL)
    case utf8DecodeFailed
    case invalidIdentifier(String)
    case queryFailed(String)
    case notFound(String)
    case decodingFailed(String)
    case checkpointFailed(String)
    case invalidPropertyPayload

    public var description: String {
        switch self {
        case .openFailed(let url): return "Unable to open NovaGraph database at \(url.path)"
        case .utf8DecodeFailed: return "Core returned a non-UTF8 response"
        case .invalidIdentifier(let value): return "Invalid NGQL identifier: \(value)"
        case .queryFailed(let msg): return "NovaGraph query failed: \(msg)"
        case .notFound(let id): return "Record not found: \(id)"
        case .decodingFailed(let msg): return "Failed to decode response: \(msg)"
        case .checkpointFailed(let msg): return "Checkpoint failed: \(msg)"
        case .invalidPropertyPayload: return "Invalid property payload"
        }
    }

    public var errorDescription: String? {
        description
    }
}

public enum GraphInspectionKind: UInt32, Sendable { case nodes = 0, edges = 1, indexes = 2 }

public struct GraphDatabaseConfiguration: Sendable {
    public var path: URL
    public var inactivityInterval: TimeInterval
    public var sweepInterval: TimeInterval
    public var queryOptions: GraphQueryOptions

    public init(
        path: URL,
        inactivityInterval: TimeInterval = 600,
        sweepInterval: TimeInterval = 30,
        queryOptions: GraphQueryOptions = GraphQueryOptions()
    ) {
        self.path = path
        self.inactivityInterval = inactivityInterval
        self.sweepInterval = sweepInterval
        self.queryOptions = queryOptions
    }
}

public enum GraphValue: Sendable, Equatable, Codable,
    ExpressibleByStringLiteral,
    ExpressibleByIntegerLiteral,
    ExpressibleByFloatLiteral,
    ExpressibleByBooleanLiteral,
    ExpressibleByNilLiteral
{
    case string(String)
    case int(Int64)
    case double(Double)
    case bool(Bool)
    case null

    public init(stringLiteral value: String) { self = .string(value) }
    public init(integerLiteral value: Int64) { self = .int(value) }
    public init(floatLiteral value: Double) { self = .double(value) }
    public init(booleanLiteral value: Bool) { self = .bool(value) }
    public init(nilLiteral: ()) { self = .null }

    public init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() {
            self = .null
        } else if let b = try? container.decode(Bool.self) {
            self = .bool(b)
        } else if let i = try? container.decode(Int64.self) {
            self = .int(i)
        } else if let d = try? container.decode(Double.self) {
            self = .double(d)
        } else if let s = try? container.decode(String.self) {
            self = .string(s)
        } else {
            throw DecodingError.dataCorruptedError(in: container, debugDescription: "Unsupported GraphValue")
        }
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .string(let s): try container.encode(s)
        case .int(let i): try container.encode(i)
        case .double(let d): try container.encode(d)
        case .bool(let b): try container.encode(b)
        case .null: try container.encodeNil()
        }
    }

    public var anyValue: Any {
        switch self {
        case .string(let s): return s
        case .int(let i): return i
        case .double(let d): return d
        case .bool(let b): return b
        case .null: return NSNull()
        }
    }

    public var stringValue: String? {
        if case .string(let s) = self { return s }
        return nil
    }

    public var intValue: Int64? {
        if case .int(let i) = self { return i }
        return nil
    }

    public var doubleValue: Double? {
        if case .double(let d) = self { return d }
        return nil
    }

    public var boolValue: Bool? {
        if case .bool(let b) = self { return b }
        return nil
    }

    static func from(any: Any) throws -> GraphValue {
        if let s = any as? String { return .string(s) }
        if any is NSNull { return .null }
        if let n = any as? NSNumber {
            if CFGetTypeID(n) == CFBooleanGetTypeID() { return .bool(n.boolValue) }
            let type = String(cString: n.objCType)
            if type == "d" || type == "f" {
                guard n.doubleValue.isFinite else { throw GraphDBError.invalidPropertyPayload }
                return .double(n.doubleValue)
            }
            guard let value = Int64(n.stringValue) else { throw GraphDBError.invalidPropertyPayload }
            return .int(value)
        }
        throw GraphDBError.invalidPropertyPayload
    }

    func validated() throws -> GraphValue {
        if case .double(let value) = self, !value.isFinite { throw GraphDBError.invalidPropertyPayload }
        return self
    }

    public var ngqlLiteral: String {
        switch self {
        case .string(let value): return "\"\(Self.escape(value))\""
        case .int(let value): return String(value)
        case .double(let value): return String(value)
        case .bool(let value): return value ? "true" : "false"
        case .null: return "null"
        }
    }

    private static func escape(_ value: String) -> String {
        value.replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
            .replacingOccurrences(of: "\n", with: "\\n")
            .replacingOccurrences(of: "\r", with: "\\r")
            .replacingOccurrences(of: "\t", with: "\\t")
    }

}

public struct GraphTransaction: Sendable {
    private(set) var validationError: GraphDBError?

    private mutating func check(_ identifier: String) {
        if identifier.range(of: #"^[A-Za-z_][A-Za-z0-9_]*$"#, options: .regularExpression) == nil { validationError = .invalidIdentifier(identifier) }
    }
    private(set) var statements: [String] = []

    public mutating func upsertNode(label: String, id: String, properties: [String: GraphValue] = [:]) {
        check(label)
        let clause = propertyClause(properties)
        statements.append("upsert node \(label) \(atom(id))\(clause)")
    }

    public mutating func upsertEdge(type: String, from: String, to: String, properties: [String: GraphValue] = [:]) {
        check(type)
        let clause = propertyClause(properties)
        statements.append("upsert edge \(type) \(atom(from)) -> \(atom(to))\(clause)")
    }

    public mutating func deleteNode(id: String) {
        statements.append("delete node \(atom(id))")
    }

    public mutating func deleteEdge(type: String, from: String, to: String) {
        check(type)
        statements.append("delete edge \(type) \(atom(from)) -> \(atom(to))")
    }

    private mutating func propertyClause(_ properties: [String: GraphValue]) -> String {
        for (key, value) in properties {
            check(key)
            if (try? value.validated()) == nil { validationError = .invalidPropertyPayload }
        }
        guard !properties.isEmpty else { return "" }
        let pairs = properties.keys.sorted().map { "\($0)=\(properties[$0]!.ngqlLiteral)" }
        return " set " + pairs.joined(separator: ", ")
    }

    private func atom(_ value: String) -> String {
        if !value.contains("//"), value.range(of: #"^[A-Za-z0-9_:\-./@]+$"#, options: .regularExpression) != nil {
            return value
        }
        return GraphValue.string(value).ngqlLiteral
    }
}

public final class GraphDatabase: @unchecked Sendable {
    let handle: OpaquePointer
    let queryOptions: GraphQueryOptions

    public init(configuration: GraphDatabaseConfiguration) throws {
        var queryOptions=try configuration.queryOptions.native()
        guard configuration.inactivityInterval.isFinite, configuration.sweepInterval.isFinite,
              configuration.inactivityInterval>0, configuration.sweepInterval>0,
              configuration.inactivityInterval<Double(Int64.max)/1000,
              configuration.sweepInterval<Double(Int64.max)/1000 else {
            throw GraphDBError.queryFailed("invalidArgument: invalid tiering interval")
        }
        let path = try maintenancePath(configuration.path)
        let ttlMs = Int64(configuration.inactivityInterval * 1000)
        let sweepMs = Int64(configuration.sweepInterval * 1000)
        let bytes = Array(path.utf8)
        let opened = bytes.withUnsafeBufferPointer { buffer in
            graphdb_open_v2(UnsafeRawPointer(buffer.baseAddress!).assumingMemoryBound(to: CChar.self), UInt64(buffer.count), ttlMs, sweepMs, &queryOptions)
        }
        guard let h = opened.handle else {
            _ = try decodeV2(responseData(opened.error))
            throw GraphDBError.openFailed(configuration.path)
        }
        graphdb_string_free(opened.error)
        self.handle = h
        self.queryOptions = configuration.queryOptions
    }

    public convenience init(
        path: URL,
        inactivityInterval: TimeInterval = 600,
        sweepInterval: TimeInterval = 30
    ) throws {
        try self.init(configuration: GraphDatabaseConfiguration(
            path: path,
            inactivityInterval: inactivityInterval,
            sweepInterval: sweepInterval
        ))
    }

    deinit {
        graphdb_close(handle)
    }

    // MARK: - Raw Query & DSL Execution

    /// Typed v2 query response. Each supplied option can only lower configured ceilings.
    public func queryResult(_ ngql: String, parameters: [String: GraphValue] = [:], options: GraphQueryOptions? = nil, request: GraphRequest? = nil) throws -> GraphQueryResult {
        try decodeV2(queryResponse(ngql, parameters: parameters, options: options, request: request), request: request)
    }

    func queryResponse(_ ngql: String, parameters: [String: GraphValue], options: GraphQueryOptions? = nil, request: GraphRequest? = nil) throws -> Data {
        try request?.check()
        let json = try scalarJSON(parameters)
        let native = try options?.native()
        let query = Array(ngql.utf8) + [0], params = Array(json.utf8) + [0]
        let response = query.withUnsafeBufferPointer { q in
            params.withUnsafeBufferPointer { p in
                let qptr = UnsafeRawPointer(q.baseAddress!).assumingMemoryBound(to: CChar.self)
                let pptr = UnsafeRawPointer(p.baseAddress!).assumingMemoryBound(to: CChar.self)
                if var value = native { return graphdb_execute_request_v2(handle, qptr, UInt64(q.count - 1), pptr, UInt64(p.count - 1), &value, request?.handle) }
                return graphdb_execute_request_v2(handle, qptr, UInt64(q.count - 1), pptr, UInt64(p.count - 1), nil, request?.handle)
            }
        }
        return try responseData(response)
    }

    /// A page of actual records or declared indexes. Cursors expire after a commit.
    /// Inspection scans metadata under the operation gate and obeys query budgets.
    public func inspect(_ kind: GraphInspectionKind, cursor: String? = nil, limit: UInt32 = 50,
                        options: GraphQueryOptions? = nil, request: GraphRequest? = nil) throws -> GraphQueryResult {
        try request?.check()
        let bytes = Array((cursor ?? "").utf8) + [0]
        let native = try options?.native()
        let response = bytes.withUnsafeBufferPointer { b in
            let pointer = UnsafeRawPointer(b.baseAddress!).assumingMemoryBound(to: CChar.self)
            if var value = native { return graphdb_inspect_v2(handle, kind.rawValue, pointer, UInt64(b.count-1), limit, &value, request?.handle) }
            return graphdb_inspect_v2(handle, kind.rawValue, pointer, UInt64(b.count-1), limit, nil, request?.handle)
        }
        return try decodeV2(responseData(response), request: request)
    }

    public func query<T: Decodable>(_ ngql: String, parameters: [String: GraphValue] = [:], as type: T.Type, options: GraphQueryOptions? = nil) throws -> T {
        try queryResult(ngql, parameters: parameters, options: options).decode(as: type)
    }

    public func mutate(_ ngql: String, parameters: [String: GraphValue] = [:], options: GraphQueryOptions? = nil) throws -> GraphMutationReceipt {
        guard let receipt = try queryResult(ngql, parameters: parameters, options: options).receipt else {
            throw GraphDBError.decodingFailed("Program did not commit a mutation")
        }
        return receipt
    }

    /// Compatibility payload JSON (without the C envelope). Values use ordinary JSON scalars.
    public func rawQuery(_ ngql: String, parameters: [String: Any]? = nil) throws -> String {
        let bindings = try (parameters ?? [:]).mapValues { try GraphValue.from(any: $0) }
        return try decodeRawV2(queryResponse(ngql, parameters: bindings))
    }

    @available(*, deprecated, message: "Use queryResult for typed results, query(_:parameters:as:) for Codable, or rawQuery for payload JSON")
    public func query(_ ngql: String, parameters: [String: Any]? = nil) throws -> String {
        try rawQuery(ngql, parameters: parameters)
    }

    public func execute(_ ngql: String, parameters: [String: Any]? = nil) throws -> String {
        try rawQuery(ngql, parameters: parameters)
    }

    public func explain(_ ngql: String) throws -> String {
        let trimmed = ngql.trimmingCharacters(in: .whitespacesAndNewlines)
        let queryStr = trimmed.lowercased().hasPrefix("explain") ? trimmed : "explain " + trimmed
        return try rawQuery(queryStr)
    }

    public func rebuildIndexes() throws {
        let out = graphdb_rebuild_indexes(handle)
        let raw = try decodeAndFree(out)
        _ = try unwrapCoreResponse(raw)
    }

    // MARK: - Typed Node Operations

    public func node(id: String) throws -> Node? {
        do { return try queryResult("get node \(atom(id))").node() }
        catch let error as GraphCoreError where error.code == "notFound" { return nil }
    }

    public func node<T: Codable & Sendable>(id: String, as type: T.Type) throws -> TypedNode<T>? {
        guard let n = try node(id: id) else { return nil }
        let props = try n.decode(as: T.self)
        return TypedNode<T>(id: n.id, label: n.label, properties: props, version: n.version)
    }

    public func nodes(
        label: String,
        where property: String? = nil,
        equals value: GraphValue? = nil,
        limit: Int = 100
    ) throws -> [Node] {
        try validateIdentifier(label)
        var ngql = "find nodes \(label)"
        if let property, let value {
            try validateIdentifier(property)
            ngql += " where \(property) = \(try value.validated().ngqlLiteral)"
        }
        ngql += " limit \(max(0, limit))"

        return try queryResult(ngql).nodes()
    }

    public func nodes<T: Codable & Sendable>(
        label: String,
        where property: String? = nil,
        equals value: GraphValue? = nil,
        as type: T.Type,
        limit: Int = 100
    ) throws -> [TypedNode<T>] {
        let rawNodes = try nodes(label: label, where: property, equals: value, limit: limit)
        return try rawNodes.map { node in
            let props = try node.decode(as: T.self)
            return TypedNode<T>(id: node.id, label: node.label, properties: props, version: node.version)
        }
    }

    @discardableResult
    public func upsertNode(
        label: String,
        id: String,
        properties: [String: GraphValue] = [:]
    ) throws -> String {
        try validateIdentifier(label)
        return try rawQuery("upsert node \(label) \(atom(id))\(try propertyClause(properties))")
    }

    @discardableResult
    public func upsertNode<T: Encodable>(
        label: String,
        id: String,
        model: T
    ) throws -> String {
        let properties = try encodedProperties(model)
        return try upsertNode(label: label, id: id, properties: properties)
    }

    public func deleteNode(id: String) throws {
        _ = try rawQuery("delete node \(atom(id))")
    }

    // MARK: - Edge Operations

    public func edge(from: String, type: String, to: String) throws -> Edge? {
        try validateIdentifier(type)
        do { return try queryResult("get edge \(type) \(atom(from)) -> \(atom(to))").edge() }
        catch let error as GraphCoreError where error.code == "notFound" { return nil }
    }

    public func edge<T: Codable & Sendable>(from: String, type: String, to: String, as model: T.Type) throws -> TypedEdge<T>? {
        guard let value = try edge(from: from, type: type, to: to) else { return nil }
        return TypedEdge(from: value.from, type: value.type, to: value.to, properties: try value.decode(as: model), version: value.version)
    }

    @discardableResult
    public func upsertEdge<T: Encodable>(type: String, from: String, to: String, model: T) throws -> String {
        return try upsertEdge(type: type, from: from, to: to, properties: encodedProperties(model))
    }

    @discardableResult
    public func upsertEdge(
        type: String,
        from: String,
        to: String,
        properties: [String: GraphValue] = [:]
    ) throws -> String {
        try validateIdentifier(type)
        return try rawQuery("upsert edge \(type) \(atom(from)) -> \(atom(to))\(try propertyClause(properties))")
    }

    public func deleteEdge(type: String, from: String, to: String) throws {
        try validateIdentifier(type)
        _ = try rawQuery("delete edge \(type) \(atom(from)) -> \(atom(to))")
    }

    // MARK: - Traversal & Graph Walking

    public func from(_ id: String) -> TraversalBuilder {
        TraversalBuilder(db: self, startNodeId: id)
    }

    public func traverse(
        from id: String,
        over edgeType: String? = nil,
        direction: TraversalDirection = .outbound,
        depth: Int = 1,
        limit: Int = 100
    ) throws -> [Node] {
        var ngql = "walk from \(atom(id))"
        if let edgeType {
            try validateIdentifier(edgeType)
            ngql += " over \(edgeType)"
        }
        ngql += " direction \(direction.rawValue)"
        ngql += " depth \(max(0, depth)) limit \(max(0, limit))"

        return try queryResult(ngql).nodes()
    }

    public func paths(from id: String, over edgeType: String? = nil, direction: TraversalDirection = .outbound, depth: Int = 1, limit: Int = 100) throws -> GraphTraversalResult {
        if let edgeType { try validateIdentifier(edgeType) }
        let over = edgeType.map { " over \($0)" } ?? ""
        return try queryResult("walk from \(atom(id))\(over) direction \(direction.rawValue) depth \(max(0, depth)) limit \(max(0, limit)) paths").traversal()
    }

    @available(*, deprecated, message: "Use traverse(from:over:direction:depth:limit:) for nodes or paths(from:over:direction:depth:limit:) for real paths")
    // Backward-compatible walk()
    public func walk(
        from id: String,
        over edgeType: String? = nil,
        depth: Int = 1,
        limit: Int = 100
    ) throws -> String {
        var ngql = "walk from \(atom(id))"
        if let edgeType {
            try validateIdentifier(edgeType)
            ngql += " over \(edgeType)"
        }
        ngql += " depth \(max(0, depth)) limit \(max(0, limit))"
        return try rawQuery(ngql)
    }

    @available(*, deprecated, message: "Use node(id:) for typed reads or rawQuery for JSON")
    // Backward-compatible getNode()
    public func getNode(id: String) throws -> String {
        try rawQuery("get node \(atom(id))")
    }

    @available(*, deprecated, message: "Use nodes(label:where:equals:limit:) for typed reads or rawQuery for JSON")
    // Backward-compatible findNodes()
    public func findNodes(
        label: String,
        where property: String? = nil,
        equals value: GraphValue? = nil,
        limit: Int = 100
    ) throws -> String {
        try validateIdentifier(label)
        var ngql = "find nodes \(label)"
        if let property, let value {
            try validateIdentifier(property)
            ngql += " where \(property) = \(try value.validated().ngqlLiteral)"
        }
        ngql += " limit \(max(0, limit))"
        return try rawQuery(ngql)
    }

    // MARK: - Indexing & Transactions

    @discardableResult
    public func createIndex(label: String, property: String) throws -> String {
        try validateIdentifier(label)
        try validateIdentifier(property)
        return try rawQuery("create index on \(label)(\(property))")
    }

    public func transaction(_ block: (inout GraphTransaction) throws -> Void) throws {
        _ = try transactionReceipt(block)
    }

    @discardableResult
    public func transactionReceipt(_ block: (inout GraphTransaction) throws -> Void) throws -> GraphMutationReceipt? {
        var tx = GraphTransaction()
        try block(&tx)
        if let error = tx.validationError { throw error }
        guard !tx.statements.isEmpty else { return nil }
        return try mutate(tx.statements.joined(separator: "; ") + ";")
    }

    // MARK: - Durability & Memory Management

    /// Writes a verified snapshot to an absent destination while serializing engine operations.
    public func backup(to destination: URL) throws {
        let path = try maintenancePath(destination)
        _ = try maintenanceData(graphdb_backup(handle, path))
    }

    public func checkpoint() throws {
        let out = graphdb_checkpoint(handle)
        let res = try decodeAndFree(out)
        _ = try unwrapCoreResponse(res)
    }

    @discardableResult
    public func trimMemory() throws -> Int {
        let out = graphdb_trim_memory(handle)
        let data = try legacyData(out)
        struct Trim: Decodable { let evicted: Int }
        return try JSONDecoder().decode(Trim.self, from: data).evicted
    }

    @available(*, deprecated, message: "Use the async throwing handleMemoryWarning() and handle storage errors")
    public func handleMemoryWarning() {
        _ = try? trimMemory()
    }

    public func sweepOnce() throws -> String {
        let out = graphdb_sweep_once(handle)
        return String(decoding: try legacyData(out), as: UTF8.self)
    }

    // MARK: - Swift Concurrency (async/await)

    public func queryResult(_ ngql: String, parameters: [String: GraphValue] = [:], options: GraphQueryOptions? = nil) async throws -> GraphQueryResult {
        let request = try makeRequest(options)
        return try await runGraphOperation(request: request) {
            try self.queryResult(ngql, parameters: parameters, options: options, request: request)
        }
    }
    public func query<T: Decodable & Sendable>(_ ngql: String, parameters: [String: GraphValue] = [:], as type: T.Type, options: GraphQueryOptions? = nil) async throws -> T {
        let request = try makeRequest(options)
        return try await runGraphOperation(request: request) {
            let result = try self.queryResult(ngql, parameters: parameters, options: options, request: request)
            let decoded = try result.decode(as: type)
            if result.receipt == nil { try request.check() }
            return decoded
        }
    }
    public func mutate(_ ngql: String, parameters: [String: GraphValue] = [:], options: GraphQueryOptions? = nil) async throws -> GraphMutationReceipt {
        guard let receipt = try await queryResult(ngql, parameters: parameters, options: options).receipt else {
            throw GraphDBError.decodingFailed("Program did not commit a mutation")
        }
        return receipt
    }
    private func readDecoded<T: Sendable>(_ ngql: String, _ decode: @escaping @Sendable (GraphQueryResult) throws -> T) async throws -> T {
        let request = try makeRequest()
        return try await runGraphOperation(request: request) {
            let result = try self.queryResult(ngql, request: request)
            let value = try decode(result)
            try request.check()
            return value
        }
    }
    public func node(id: String) async throws -> Node? {
        do { return try await readDecoded("get node \(atom(id))") { try $0.node() } }
        catch let error as GraphCoreError where error.code == "notFound" { return nil }
    }
    public func nodes(label: String, where property: String? = nil, equals value: GraphValue? = nil, limit: Int = 100) async throws -> [Node] {
        try validateIdentifier(label)
        var query = "find nodes \(label)"
        if let property, let value {
            try validateIdentifier(property)
            query += " where \(property) = \(try value.validated().ngqlLiteral)"
        }
        return try await readDecoded(query + " limit \(max(0, limit))") { try $0.nodes() }
    }
    public func traverse(from id: String, over edgeType: String? = nil, direction: TraversalDirection = .outbound, depth: Int = 1, limit: Int = 100) async throws -> [Node] {
        if let edgeType { try validateIdentifier(edgeType) }
        let over = edgeType.map { " over \($0)" } ?? ""
        return try await readDecoded("walk from \(atom(id))\(over) direction \(direction.rawValue) depth \(max(0, depth)) limit \(max(0, limit))") { try $0.nodes() }
    }
    public func paths(from id: String, over edgeType: String? = nil, direction: TraversalDirection = .outbound, depth: Int = 1, limit: Int = 100) async throws -> GraphTraversalResult {
        if let edgeType { try validateIdentifier(edgeType) }
        let over = edgeType.map { " over \($0)" } ?? ""
        return try await readDecoded("walk from \(atom(id))\(over) direction \(direction.rawValue) depth \(max(0, depth)) limit \(max(0, limit)) paths") { try $0.traversal() }
    }
    public func execute(_ ngql: String, parameters: [String: Any]? = nil) async throws -> String {
        let bindings = try (parameters ?? [:]).mapValues { try GraphValue.from(any: $0) }
        let request = try makeRequest()
        return try await runGraphOperation(request: request) {
            try decodeRawV2(self.queryResponse(ngql, parameters: bindings, request: request), request: request)
        }
    }
    public func explain(_ ngql: String) async throws -> String {
        let text = ngql.trimmingCharacters(in: .whitespacesAndNewlines)
        return try await execute(text.lowercased().hasPrefix("explain") ? text : "explain " + text)
    }
    public func rebuildIndexes() async throws { _ = try await queryResult("rebuild indexes") }

    // MARK: - Private Helpers

    private func decodeAndFree(_ out: GraphDBString) throws -> String {
        defer { graphdb_string_free(out) }
        guard let data = out.data else { throw GraphDBError.utf8DecodeFailed }
        let bytes = Data(bytes: data, count: Int(out.len))
        guard let response = String(data: bytes, encoding: .utf8) else {
            throw GraphDBError.utf8DecodeFailed
        }
        return response
    }

    private func unwrapCoreResponse(_ jsonString: String) throws -> String {
        struct Status: Decodable { let ok: Bool; let code: String?; let error: String?; let context: GraphErrorContext? }
        let data = Data(jsonString.utf8)
        let status = try JSONDecoder().decode(Status.self, from: data)
        if !status.ok { throw GraphCoreError(code: status.code ?? "ioFailure", message: status.error ?? "Unknown error", context: status.context) }
        return jsonString
    }
    private func legacyData(_ value: GraphDBString) throws -> Data {
        let data = try responseData(value)
        _ = try unwrapCoreResponse(String(decoding: data, as: UTF8.self))
        return data
    }

    private func propertyClause(_ properties: [String: GraphValue]) throws -> String {
        guard !properties.isEmpty else { return "" }
        let pairs = try properties.keys.sorted().map { key in
            try validateIdentifier(key)
            return "\(key)=\(try properties[key]!.validated().ngqlLiteral)"
        }
        return " set " + pairs.joined(separator: ", ")
    }

    private func validateIdentifier(_ value: String) throws {
        guard value.range(of: #"^[A-Za-z_][A-Za-z0-9_]*$"#, options: .regularExpression) != nil else {
            throw GraphDBError.invalidIdentifier(value)
        }
    }

    private func atom(_ value: String) -> String {
        if !value.contains("//"), value.range(of: #"^[A-Za-z0-9_:\-./@]+$"#, options: .regularExpression) != nil {
            return value
        }
        return GraphValue.string(value).ngqlLiteral
    }
}
