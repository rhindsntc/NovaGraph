import Foundation

public enum TraversalDirection: String, Sendable {
    case outbound = "out"
    case inbound = "in"
}

public struct Node: Identifiable, Sendable, Equatable {
    public let id: String
    public let label: String
    public let properties: [String: GraphValue]
    public let lastReadMs: Int64
    public let lastModifiedMs: Int64
    public let version: UInt64

    public init(
        id: String,
        label: String,
        properties: [String: GraphValue] = [:],
        lastReadMs: Int64 = 0,
        lastModifiedMs: Int64 = 0,
        version: UInt64 = 1
    ) {
        self.id = id
        self.label = label
        self.properties = properties
        self.lastReadMs = lastReadMs
        self.lastModifiedMs = lastModifiedMs
        self.version = version
    }

    public subscript(key: String) -> GraphValue? {
        properties[key]
    }

    public func decode<T: Decodable>(as type: T.Type = T.self) throws -> T {
        let data = Data(try scalarJSON(properties).utf8)
        return try JSONDecoder().decode(T.self, from: data)
    }
}

public struct Edge: Sendable, Equatable {
    public let from: String
    public let type: String
    public let to: String
    public let properties: [String: GraphValue]
    public let lastReadMs: Int64
    public let lastModifiedMs: Int64
    public let version: UInt64

    public init(
        from: String,
        type: String,
        to: String,
        properties: [String: GraphValue] = [:],
        lastReadMs: Int64 = 0,
        lastModifiedMs: Int64 = 0,
        version: UInt64 = 1
    ) {
        self.from = from
        self.type = type
        self.to = to
        self.properties = properties
        self.lastReadMs = lastReadMs
        self.lastModifiedMs = lastModifiedMs
        self.version = version
    }

    public subscript(key: String) -> GraphValue? {
        properties[key]
    }

    public func decode<T: Decodable>(as type: T.Type = T.self) throws -> T {
        let data = Data(try scalarJSON(properties).utf8)
        return try JSONDecoder().decode(T.self, from: data)
    }
}

public struct TypedNode<Props: Codable & Sendable>: Identifiable, Sendable {
    public let id: String
    public let label: String
    public let properties: Props
    public let version: UInt64

    public init(id: String, label: String, properties: Props, version: UInt64 = 1) {
        self.id = id
        self.label = label
        self.properties = properties
        self.version = version
    }
}

public struct Path: Sendable, Equatable {
    public let nodes: [Node]
    public let edges: [Edge]

    public var length: Int {
        edges.count
    }

    public init(nodes: [Node], edges: [Edge] = []) {
        self.nodes = nodes
        self.edges = edges
    }
}

public struct TypedEdge<Props: Codable & Sendable>: Sendable {
    public let from: String
    public let type: String
    public let to: String
    public let properties: Props
    public let version: UInt64
    public init(from: String, type: String, to: String, properties: Props, version: UInt64 = 1) {
        self.from = from; self.type = type; self.to = to
        self.properties = properties; self.version = version
    }
}

func scalarJSON(_ properties: [String: GraphValue]) throws -> String {
    let entries = try properties.keys.sorted().map { key in
        let name = String(decoding: try JSONEncoder().encode(key), as: UTF8.self)
        let value = try properties[key]!.validated()
        let scalar: String
        if case .string(let text) = value { scalar = String(decoding: try JSONEncoder().encode(text), as: UTF8.self) }
        else { scalar = value.ngqlLiteral }
        return name + ":" + scalar
    }
    return "{" + entries.joined(separator: ",") + "}"
}
