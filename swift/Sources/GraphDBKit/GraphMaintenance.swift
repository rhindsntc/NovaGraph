import CGraphDB
import Foundation

public struct GraphMaintenanceError: Error, Decodable, Sendable, LocalizedError {
    public let code: String
    public let error: String
    public var errorDescription: String? { "\(code): \(error)" }
}
public struct GraphMaintenanceReport: Decodable, Sendable {
    public let format: String
    public let generation: UInt64
    public let committedLSN: UInt64
    public let nodes: Int
    public let edges: Int
    public let indexes: Int
    public let tailRepairNeeded: Bool
    public let warnings: [String]
}

func maintenanceData(_ value: GraphDBString) throws -> Data {
    defer { graphdb_string_free(value) }
    guard let pointer = value.data, value.len <= UInt64(Int.max) else {
        throw GraphMaintenanceError(code: "ioFailure", error: "Missing maintenance response")
    }
    let data = Data(bytes: pointer, count: Int(value.len))
    struct Envelope: Decodable { let ok: Bool }
    guard try JSONDecoder().decode(Envelope.self, from: data).ok else {
        throw try JSONDecoder().decode(GraphMaintenanceError.self, from: data)
    }
    return data
}
func maintenancePath(_ url: URL) throws -> String {
    guard url.isFileURL, !url.path.utf8.contains(0), !url.path.isEmpty else {
        throw GraphMaintenanceError(code: "invalidArgument", error: "Expected a local file URL")
    }
    return url.path
}

/// Offline operations require the database to be closed. Legacy writers must be stopped.
public enum GraphMaintenance {
    private static func run(_ command: String, _ source: URL, _ destination: URL? = nil) throws -> GraphMaintenanceReport {
        let path = try maintenancePath(source)
        let target = try destination.map(maintenancePath)
        let response = graphdb_maintenance(command, path, target)
        return try JSONDecoder().decode(GraphMaintenanceReport.self, from: maintenanceData(response))
    }
    public static func inspect(_ source: URL) throws -> GraphMaintenanceReport { try run("inspect", source) }
    public static func verify(_ source: URL) throws -> GraphMaintenanceReport { try run("verify", source) }
    public static func rebuildIndexes(_ source: URL) throws -> GraphMaintenanceReport { try run("rebuild-indexes", source) }
    public static func backup(from source: URL, to destination: URL) throws -> GraphMaintenanceReport { try run("backup", source, destination) }
    public static func restore(from source: URL, to destination: URL) throws -> GraphMaintenanceReport { try run("restore", source, destination) }
    public static func migrateV1(from source: URL, to destination: URL) throws -> GraphMaintenanceReport { try run("migrate-v1", source, destination) }
}
