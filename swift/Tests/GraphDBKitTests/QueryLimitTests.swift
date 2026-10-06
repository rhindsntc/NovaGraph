import XCTest
@testable import GraphDBKit

final class QueryLimitTests: XCTestCase {
    func testConfiguredDepthAndResultLimitsReachCore() throws {
        let path=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: path) }
        var configuration=GraphDatabaseConfiguration(path:path, sweepInterval:0.01)
        configuration.queryOptions.maxDepth=1
        configuration.queryOptions.maxResults=1
        let db=try GraphDatabase(configuration:configuration)
        XCTAssertThrowsError(try db.query("walk from missing depth 2"))
        XCTAssertThrowsError(try db.query("find nodes Missing limit 2"))
        XCTAssertEqual(try db.query("find nodes Missing"),"[]")
    }
    func testInvalidQueryConfigurationThrowsBeforeOpening() throws {
        let path=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: path) }
        var configuration=GraphDatabaseConfiguration(path:path, sweepInterval:0.01)
        configuration.queryOptions.workingBytes = -1
        XCTAssertThrowsError(try GraphDatabase(configuration:configuration))
        XCTAssertFalse(FileManager.default.fileExists(atPath:path.path))
        configuration.queryOptions.workingBytes=1024
        configuration.queryOptions.timeoutMilliseconds=0
        XCTAssertThrowsError(try GraphDatabase(configuration:configuration))
    }

    func testIdentifiersContainingCommentMarkersRemainLiteral() throws {
        let path=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at:path) }
        let db=try GraphDatabase(path:path,sweepInterval:0.01)
        try db.upsertNode(label:"Link",id:"https://nova.test/a",properties:["value":1])
        let node=try XCTUnwrap(db.node(id:"https://nova.test/a"))
        XCTAssertEqual(node.id,"https://nova.test/a")
        try db.transaction { tx in tx.upsertNode(label:"Link",id:"x//y") }
        XCTAssertEqual(try db.node(id:"x//y")?.id,"x//y")
    }
}
