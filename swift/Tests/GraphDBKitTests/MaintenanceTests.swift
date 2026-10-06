import XCTest
@testable import GraphDBKit
final class MaintenanceTests: XCTestCase {
    func testOnlineBackupAndOfflineRestorePreserveGraph() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let source = root.appendingPathComponent("source")
        let backup = root.appendingPathComponent("backup")
        let restored = root.appendingPathComponent("restored")
        do {
            let db = try GraphDatabase(path: source)
            _ = try db.execute("upsert node N a set v=7")
            try db.backup(to: backup)
            XCTAssertThrowsError(try GraphMaintenance.verify(source)) { error in
                XCTAssertEqual((error as? GraphMaintenanceError)?.code, "busy")
            }
        }
        XCTAssertEqual(try GraphMaintenance.verify(backup).nodes, 1)
        XCTAssertEqual(try GraphMaintenance.restore(from: backup, to: restored).nodes, 1)
        XCTAssertEqual(try GraphMaintenance.rebuildIndexes(restored).indexes, 0)
        XCTAssertThrowsError(try GraphMaintenance.restore(from: backup, to: restored)) { error in
            XCTAssertEqual((error as? GraphMaintenanceError)?.code, "conflict")
        }
    }
}
