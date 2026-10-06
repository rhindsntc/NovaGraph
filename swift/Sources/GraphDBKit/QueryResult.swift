import CGraphDB
import Foundation

public struct GraphErrorContext: Codable, Sendable, Equatable {
    public let file: String?
    public let osError: Int?
    public let object: String?
    public let transactionId: String?
    public let statementIndex: Int?
    public let sourceStart: Int?
    public let sourceEnd: Int?
}

public struct GraphCoreError: Error, Codable, Sendable, Equatable, LocalizedError {
    public let code: String
    public let message: String
    public let context: GraphErrorContext?
    public var errorDescription: String? { "\(code): \(message)" }
}

/// A successful durable commit. Never returned for a failed or read-only program.
public struct GraphMutationReceipt: Codable, Sendable, Equatable {
    public let transactionId: String
    public let committedLSN: UInt64
}

/// JSON structure without Foundation's NSNumber/Bool bridging. Property values in
/// v2 responses have explicit scalar tags; `decode` removes those tags for Codable models.
public enum GraphJSON: Codable, Sendable, Equatable {
    case null, bool(Bool), int(Int64), uint(UInt64), double(Double), string(String)
    case array([GraphJSON]), object([String: GraphJSON])

    public init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if c.decodeNil() { self = .null }
        else if let v = try? c.decode(Bool.self) { self = .bool(v) }
        else if let v = try? c.decode(String.self) { self = .string(v) }
        else if let v = try? c.decode(Int64.self) { self = .int(v) }
        else if let v = try? c.decode(UInt64.self) { self = .uint(v) }
        else if let v = try? c.decode(Double.self), v.isFinite { self = .double(v) }
        else if let v = try? c.decode([GraphJSON].self) { self = .array(v) }
        else {
            var fields = try c.decode([String: GraphJSON].self)
            if fields.count == 2, fields["type"] == .string("double") {
                switch fields["value"] {
                case .int, .uint, .double:
                    if case .double(let value) = try WireValue(from: decoder).value { fields["value"] = .double(value) }
                default: break
                }
            }
            self = .object(fields)
        }
    }
    public func encode(to encoder: Encoder) throws {
        var c = encoder.singleValueContainer()
        switch self {
        case .null: try c.encodeNil()
        case .bool(let v): try c.encode(v)
        case .int(let v): try c.encode(v)
        case .uint(let v): try c.encode(v)
        case .double(let v): try c.encode(v)
        case .string(let v): try c.encode(v)
        case .array(let v): try c.encode(v)
        case .object(let v): try c.encode(v)
        }
    }
    var untagged: GraphJSON { removingTags(allowScalar: false) }
    private func removingTags(allowScalar: Bool) -> GraphJSON {
        switch self {
        case .object(let fields):
            if allowScalar, fields.count == 2, let value = fields["value"] {
                switch (fields["type"], value) {
                case (.string("null"), .null), (.string("bool"), .bool),
                     (.string("int"), .int), (.string("double"), .double),
                     (.string("string"), .string): return value
                default: break
                }
            }
            return .object(fields.mapValues { $0.removingTags(allowScalar: true) })
        // Root records/projections and array elements are containers. Only named
        // field values can be scalar tags; graph properties cannot contain objects.
        case .array(let values): return .array(values.map { $0.removingTags(allowScalar: false) })
        default: return self
        }
    }
    public func decode<T: Decodable>(as type: T.Type = T.self) throws -> T {
        do { return try JSONDecoder().decode(type, from: JSONEncoder().encode(untagged)) }
        catch { throw GraphDBError.decodingFailed(String(describing: error)) }
    }
    var jsonString: String { get throws { String(decoding: try JSONEncoder().encode(self), as: UTF8.self) } }
}

public struct GraphQueryResult: Sendable {
    public let data: GraphJSON
    public let receipt: GraphMutationReceipt?
    public func decode<T: Decodable>(as type: T.Type = T.self) throws -> T { try data.decode(as: type) }
    public func nodes() throws -> [Node] { try wire([WireNode].self).map(\.node) }
    public func node() throws -> Node { try wire(WireNode.self).node }
    public func edge() throws -> Edge { try wire(WireEdge.self).edge }
    public func traversal() throws -> GraphTraversalResult {
        let result = try wire(WireTraversal.self)
        return GraphTraversalResult(nodes: result.nodes.map(\.node), paths: result.paths.map {
            Path(nodes: $0.nodes.map(\.node), edges: $0.edges.map(\.edge))
        })
    }
    private func wire<T: Decodable>(_ type: T.Type) throws -> T {
        do { return try JSONDecoder().decode(type, from: JSONEncoder().encode(data)) }
        catch { throw GraphDBError.decodingFailed(String(describing: error)) }
    }
}
public struct GraphTraversalResult: Sendable, Equatable {
    public let nodes: [Node]
    public let paths: [Path]
}

struct WireValue: Decodable {
    let value: GraphValue
    enum Keys: String, CodingKey { case type, value }
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: Keys.self)
        switch try c.decode(String.self, forKey: .type) {
        case "null":
            guard try c.decodeNil(forKey: .value) else { throw GraphDBError.decodingFailed("Invalid null scalar") }
            value = .null
        case "bool": value = .bool(try c.decode(Bool.self, forKey: .value))
        case "int": value = .int(try c.decode(Int64.self, forKey: .value))
        case "double": value = .double(try c.decode(Double.self, forKey: .value))
        case "string": value = .string(try c.decode(String.self, forKey: .value))
        default: throw GraphDBError.decodingFailed("Unsupported scalar tag")
        }
    }
}
struct WireNode: Decodable {
    let id: String, label: String
    let properties: [String: WireValue]
    let last_read_ms: Int64, last_modified_ms: Int64
    let version: UInt64
    var node: Node { Node(id: id, label: label, properties: properties.mapValues(\.value), lastReadMs: last_read_ms, lastModifiedMs: last_modified_ms, version: version) }
}
struct WireEdge: Decodable {
    let from: String, type: String, to: String
    let properties: [String: WireValue]
    let last_read_ms: Int64, last_modified_ms: Int64
    let version: UInt64
    var edge: Edge { Edge(from: from, type: type, to: to, properties: properties.mapValues(\.value), lastReadMs: last_read_ms, lastModifiedMs: last_modified_ms, version: version) }
}
struct WireTraversal: Decodable {
    struct Route: Decodable { let nodes: [WireNode]; let edges: [WireEdge] }
    let nodes: [WireNode]; let paths: [Route]
}

func responseData(_ value: GraphDBString) throws -> Data {
    defer { graphdb_string_free(value) }
    guard let pointer = value.data, value.len <= UInt64(Int.max) else { throw GraphDBError.utf8DecodeFailed }
    return Data(bytes: pointer, count: Int(value.len))
}
func decodeV2(_ data: Data, request: GraphRequest? = nil) throws -> GraphQueryResult {
    struct Envelope: Decodable {
        let schemaVersion: Int, ok: Bool
        let data: GraphJSON?
        let error: GraphCoreError?
        let receipt: GraphMutationReceipt?
    }
    let envelope: Envelope
    do { envelope = try JSONDecoder().decode(Envelope.self, from: data) }
    catch { throw GraphDBError.decodingFailed(String(describing: error)) }
    guard envelope.schemaVersion == 2 else { throw GraphDBError.decodingFailed("Unsupported response schema version") }
    if !envelope.ok {
        guard let error = envelope.error else { throw GraphDBError.decodingFailed("Missing error status") }
        throw error
    }
    let reader = envelope.receipt == nil ? request : nil
    try reader?.check()
    try validateWireDictionaryKeys(data, request: reader)
    try reader?.check()
    guard let result = envelope.data else { throw GraphDBError.decodingFailed("Missing result data") }
    return GraphQueryResult(data: result, receipt: envelope.receipt)
}
