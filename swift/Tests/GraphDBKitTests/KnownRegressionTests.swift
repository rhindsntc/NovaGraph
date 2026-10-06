import Foundation
import XCTest
@testable import GraphDBKit

final class KnownRegressionTests: XCTestCase {
    func testIntegerOneRetainsType() throws {
        let path = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: path) }
        let db = try GraphDatabase(path: path)
        try db.upsertNode(label: "Value", id: "v", properties: ["value": .int(1)])
        let node = try XCTUnwrap(db.node(id: "v"))
        XCTAssertEqual(node["value"], .int(1))
    }

    func testIntegerZeroRetainsType() throws {
        let path = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: path) }
        let db = try GraphDatabase(path: path)
        try db.upsertNode(label: "Value", id: "v", properties: ["value": .int(0)])
        let node = try XCTUnwrap(db.node(id: "v"))
        XCTAssertEqual(node["value"], .int(0))
    }

    func testDoubleRetainsPrecision() throws {
        let path = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: path) }
        let db = try GraphDatabase(path: path)
        try db.upsertNode(label: "Value", id: "v", properties: ["value": .double(1.23456789)])
        let node = try XCTUnwrap(db.node(id: "v"))
        XCTAssertEqual(node["value"], .double(1.23456789))
    }

    func testPointReadPropagatesColdStorageError() throws {
        let path = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: path) }
        // Prepare a valid cold catalog once; the assertion itself has no timing dependency.
        do {
            let db = try GraphDatabase(path: path, inactivityInterval: 600, sweepInterval: 100)
            try db.upsertNode(label: "Value", id: "v", properties: ["name": "kept"])
            let count = try db.trimMemory()
            XCTAssertEqual(count, 1)
            try db.checkpoint()
        }
        let cold = path.appendingPathComponent("cold")
        let payloads = try FileManager.default.contentsOfDirectory(at: cold, includingPropertiesForKeys: nil)
        XCTAssertEqual(payloads.count, 1)
        try FileManager.default.removeItem(at: try XCTUnwrap(payloads.first))
        let db = try GraphDatabase(path: path)
        // Establish the actual underlying error outside the expected-failure scope.
        XCTAssertThrowsError(try db.query("get node v"))
        var readError: Error?
        do { let _: Node? = try db.node(id: "v") } catch { readError = error }
        XCTAssertNotNil(readError)
        XCTAssertEqual((readError as? GraphCoreError)?.code, "corruptData")
    }
}
