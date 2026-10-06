import Foundation

public struct TraversalBuilder: Sendable {
    private let db: GraphDatabase
    private let startNodeId: String
    private var edgeType: String?
    private var direction: TraversalDirection = .outbound
    private var maxDepth: Int = 1
    private var maxLimit: Int = 100

    init(db: GraphDatabase, startNodeId: String) {
        self.db = db
        self.startNodeId = startNodeId
    }

    public func out(_ edgeType: String? = nil) -> Self {
        var copy = self
        copy.direction = .outbound
        copy.edgeType = edgeType
        return copy
    }

    public func `in`(_ edgeType: String? = nil) -> Self {
        var copy = self
        copy.direction = .inbound
        copy.edgeType = edgeType
        return copy
    }

    public func depth(_ depth: Int) -> Self {
        var copy = self
        copy.maxDepth = depth
        return copy
    }

    public func limit(_ limit: Int) -> Self {
        var copy = self
        copy.maxLimit = limit
        return copy
    }

    public func paths() throws -> [Path] {
        try db.paths(from: startNodeId, over: edgeType, direction: direction, depth: maxDepth, limit: maxLimit).paths
    }

    public func collect() throws -> [Node] {
        try db.traverse(
            from: startNodeId,
            over: edgeType,
            direction: direction,
            depth: maxDepth,
            limit: maxLimit
        )
    }

    public func collect<T: Codable & Sendable>(as type: T.Type) throws -> [TypedNode<T>] {
        let nodes = try collect()
        return try nodes.map { node in
            let props = try node.decode(as: T.self)
            return TypedNode<T>(id: node.id, label: node.label, properties: props, version: node.version)
        }
    }

    public func collect() async throws -> [Node] {
        try await Task.detached { [self] in
            try self.collect()
        }.value
    }

    public func collect<T: Codable & Sendable>(as type: T.Type) async throws -> [TypedNode<T>] {
        try await Task.detached { [self] in
            try self.collect(as: type)
        }.value
    }
}
