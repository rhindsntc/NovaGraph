import Foundation
import XCTest
@testable import GraphDBKit

struct UserProfile: Codable, Sendable, Equatable {
    let name: String
    let email: String
    let age: Int
}

final class GraphDBKitTests: XCTestCase {

    func testTypedOperationsAndCodable() throws {
        let path = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("nova-graph-swift-typed-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: path) }

        let db = try GraphDatabase(path: path, inactivityInterval: 600, sweepInterval: 100)
        try db.createIndex(label: "Person", property: "email")

        // Test Codable upsert
        let profile = UserProfile(name: "Ada Lovelace", email: "ada@analytical.engine", age: 36)
        try db.upsertNode(label: "Person", id: "user:ada", model: profile)

        // Test typed node retrieval
        let node = try db.node(id: "user:ada")
        XCTAssertNotNil(node)
        XCTAssertEqual(node?.id, "user:ada")
        XCTAssertEqual(node?.label, "Person")
        XCTAssertEqual(node?["name"], "Ada Lovelace")

        // Test generic Codable decode
        let typedNode = try db.node(id: "user:ada", as: UserProfile.self)
        XCTAssertNotNil(typedNode)
        XCTAssertEqual(typedNode?.properties.name, "Ada Lovelace")
        XCTAssertEqual(typedNode?.properties.email, "ada@analytical.engine")
        XCTAssertEqual(typedNode?.properties.age, 36)

        // Test typed indexed find
        let found = try db.nodes(label: "Person", where: "email", equals: "ada@analytical.engine")
        XCTAssertEqual(found.count, 1)
        XCTAssertEqual(found.first?.id, "user:ada")

        let typedFound = try db.nodes(label: "Person", where: "email", equals: "ada@analytical.engine", as: UserProfile.self)
        XCTAssertEqual(typedFound.count, 1)
        XCTAssertEqual(typedFound.first?.properties, profile)
    }

    func testFluentTraversalAndInbound() throws {
        let path = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("nova-graph-swift-traversal-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: path) }

        let db = try GraphDatabase(path: path)

        let ada = UserProfile(name: "Ada", email: "ada@example.com", age: 36)
        let babbage = UserProfile(name: "Charles", email: "charles@example.com", age: 60)
        let grace = UserProfile(name: "Grace", email: "grace@example.com", age: 40)

        try db.upsertNode(label: "Person", id: "ada", model: ada)
        try db.upsertNode(label: "Person", id: "charles", model: babbage)
        try db.upsertNode(label: "Person", id: "grace", model: grace)

        try db.upsertEdge(type: "COLLABORATES", from: "ada", to: "charles")
        try db.upsertEdge(type: "COLLABORATES", from: "charles", to: "grace")

        // Fluent outbound walk
        let outbound = try db.from("ada").out("COLLABORATES").depth(2).collect(as: UserProfile.self)
        XCTAssertEqual(outbound.count, 2)
        let names = Set(outbound.map { $0.properties.name })
        XCTAssertTrue(names.contains("Charles"))
        XCTAssertTrue(names.contains("Grace"))

        // Fluent inbound walk from grace back to charles
        let inbound = try db.from("grace").in("COLLABORATES").depth(1).collect(as: UserProfile.self)
        XCTAssertEqual(inbound.count, 1)
        XCTAssertEqual(inbound.first?.properties.name, "Charles")
    }

    func testTransactionsAndDeletions() throws {
        let path = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("nova-graph-swift-tx-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: path) }

        let db = try GraphDatabase(path: path)

        // Batch transaction
        try db.transaction { tx in
            tx.upsertNode(label: "Item", id: "item:1", properties: ["title": "Alpha"])
            tx.upsertNode(label: "Item", id: "item:2", properties: ["title": "Beta"])
            tx.upsertEdge(type: "LINKS", from: "item:1", to: "item:2")
        }

        XCTAssertNotNil(try db.node(id: "item:1"))
        XCTAssertNotNil(try db.node(id: "item:2"))
        XCTAssertNotNil(try db.edge(from: "item:1", type: "LINKS", to: "item:2"))

        // Delete edge
        try db.deleteEdge(type: "LINKS", from: "item:1", to: "item:2")
        XCTAssertNil(try db.edge(from: "item:1", type: "LINKS", to: "item:2"))

        // Delete node
        try db.deleteNode(id: "item:1")
        XCTAssertNil(try db.node(id: "item:1"))
    }

    func testPersistenceAndRecovery() throws {
        let path = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("nova-graph-swift-persist-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: path) }

        // Session 1
        do {
            let db = try GraphDatabase(path: path, inactivityInterval: 1, sweepInterval: 100)
            try db.createIndex(label: "Device", property: "serial")
            try db.upsertNode(label: "Device", id: "dev:100", properties: ["serial": "SN-999", "model": "NovaAir"])
            try db.checkpoint()
        }

        // Session 2: reopen and verify recovered catalog and index
        do {
            let db = try GraphDatabase(path: path)
            let dev = try db.node(id: "dev:100")
            XCTAssertNotNil(dev)
            XCTAssertEqual(dev?["serial"], "SN-999")
            XCTAssertEqual(dev?["model"], "NovaAir")

            let found = try db.nodes(label: "Device", where: "serial", equals: "SN-999")
            XCTAssertEqual(found.count, 1)
        }
    }

    func testAsyncConcurrency() async throws {
        let path = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("nova-graph-swift-async-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: path) }

        let db = try GraphDatabase(path: path)
        _ = try await db.execute("upsert node Server s1 set host=\"10.0.0.1\", port=8080")

        let node = try await db.node(id: "s1")
        XCTAssertNotNil(node)
        XCTAssertEqual(node?["host"], "10.0.0.1")

        let servers = try await db.nodes(label: "Server")
        XCTAssertEqual(servers.count, 1)
    }

    func testMemoryTrimming() throws {
        let path = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("nova-graph-swift-mem-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: path) }

        let db = try GraphDatabase(path: path, inactivityInterval: 600, sweepInterval: 100)
        try db.upsertNode(label: "Cache", id: "c1", properties: ["val": "temp"])

        // Explicit trim must evict recent records, independently of the TTL worker.
        _ = try db.sweepOnce()
        let evicted = try db.trimMemory()
        XCTAssertEqual(evicted, 1)
        XCTAssertEqual(try db.trimMemory(), 0)
        XCTAssertEqual(try db.node(id: "c1")?["val"], "temp")

        db.handleMemoryWarning()
    }

    func testNGQLParametersExplainAndRebuild() throws {
        let path = URL(fileURLWithPath: NSTemporaryDirectory())
            .appendingPathComponent("nova-graph-swift-ngql-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: path) }

        let db = try GraphDatabase(path: path)
        try db.createIndex(label: "Employee", property: "email")
        try db.upsertNode(label: "Employee", id: "emp:1", properties: [
            "name": "Sarah",
            "email": "sarah@nova.io",
            "role": "Engineer"
        ])

        // Parameterized query
        let result = try db.query("find nodes Employee where email = $searchEmail limit 1;", parameters: [
            "searchEmail": "sarah@nova.io"
        ])
        XCTAssertTrue(result.contains("Sarah"))

        // Query Explain
        let explain = try db.explain("find nodes Employee where email = \"sarah@nova.io\"")
        XCTAssertTrue(explain.contains("\"explain\":true"))
        XCTAssertTrue(explain.contains("PropertyIndexScan"))

        // Rebuild Indexes
        try db.rebuildIndexes()
        let afterRebuild = try db.nodes(label: "Employee", where: "email", equals: "sarah@nova.io")
        XCTAssertEqual(afterRebuild.count, 1)
        XCTAssertEqual(afterRebuild.first?["name"], "Sarah")
    }
}

extension GraphDBKitTests {
    func testTypedInspectionPages() throws {
        let path = FileManager.default.temporaryDirectory.appendingPathComponent("nova-inspection-\(UUID())")
        defer { try? FileManager.default.removeItem(at: path) }
        let db = try GraphDatabase(path: path)
        defer { try? db.close() }
        _ = try db.queryResult("upsert node N a set count=9223372036854775807; upsert node N b")
        struct Row: Decodable { let id: String; let properties: [String: GraphValue] }
        struct Page: Decodable { let items: [Row]; let nextCursor: String? }
        let page = try db.inspect(.nodes, limit: 1).decode(as: Page.self)
        XCTAssertEqual(page.items.map(\.id), ["a"])
        XCTAssertEqual(page.items[0].properties["count"], .int(Int64.max))
        let next = try db.inspect(.nodes, cursor: page.nextCursor, limit: 1).decode(as: Page.self)
        XCTAssertEqual(next.items.map(\.id), ["b"]); XCTAssertNil(next.nextCursor)
    }
}
