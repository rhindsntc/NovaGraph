import Foundation
import GraphDBKit

// Synchronous opening/recovery runs on this actor, never the UI actor.
actor DatabaseOpener {
    func open(_ path: URL) throws -> GraphDatabase {
        try FileManager.default.createDirectory(at: path.deletingLastPathComponent(), withIntermediateDirectories: true)
        var options = GraphQueryOptions()
        options.maxResults = 100
        options.resultBytes = 1_048_576
        return try GraphDatabase(configuration: .init(path: path, queryOptions: options))
    }
    // Cleanup must run even when the view-owned opening task was cancelled.
    func close(_ database: GraphDatabase) throws { try database.close() }
}

