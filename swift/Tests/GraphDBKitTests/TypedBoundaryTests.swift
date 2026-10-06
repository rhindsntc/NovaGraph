import Foundation
import XCTest
@testable import GraphDBKit

final class TypedBoundaryTests: XCTestCase {
    private func withDatabase(_ body: (GraphDatabase) throws -> Void) throws {
        let path = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: path) }
        try body(GraphDatabase(path: path))
    }

    func testScalarKindsAndExtremesSurviveColdRead() throws {
        try withDatabase { db in
            let values: [String: GraphValue] = ["zero": .int(0), "one": .int(1), "min": .int(.min),
                "max": .int(.max), "exact": .int(9007199254740993), "real": .double(1.0),
                "precise": .double(1.23456789), "negativeZero": .double(-0.0), "flag": .bool(true),
                "empty": .null, "escaped": .string("雪\n\"\\\u{0}tail")]
            try db.upsertNode(label: "Value", id: "v", properties: values)
            let hot = try XCTUnwrap(db.node(id: "v"))
            XCTAssertEqual(hot.properties, values)
            XCTAssertEqual(hot["negativeZero"]?.doubleValue?.sign, .minus)
            _ = try db.trimMemory()
            XCTAssertEqual(try db.node(id: "v")?.properties, values)
        }
    }

    func testCodableRejectsNestedPropertiesBeforeMutation() throws {
        struct Nested: Encodable { let count = 1; let child = ["n": 2] }
        try withDatabase { db in
            XCTAssertThrowsError(try db.upsertNode(label: "Value", id: "nested", model: Nested()))
            XCTAssertNil(try db.node(id: "nested"))
        }
    }

    func testTraversalBuilderCopiesAreIndependent() throws {
        try withDatabase { db in
            try db.transaction { tx in
                for id in ["a", "b", "c"] { tx.upsertNode(label: "N", id: id) }
                tx.upsertEdge(type: "LINK", from: "a", to: "b")
                tx.upsertEdge(type: "LINK", from: "b", to: "c")
            }
            let base = db.from("a").out("LINK")
            let deeper = base.depth(2)
            XCTAssertEqual(try base.collect().map(\.id), ["b"])
            XCTAssertEqual(try deeper.collect().map(\.id), ["b", "c"])
            _ = base.limit(0)
            XCTAssertEqual(try base.collect().count, 1)
        }
    }

    func testEmbeddedNulIdentityCannotAliasPrefix() throws {
        try withDatabase { db in
            try db.upsertNode(label: "Value", id: "a\u{0}b", properties: ["text": "kept"])
            XCTAssertNil(try db.node(id: "a"))
            XCTAssertEqual(try db.node(id: "a\u{0}b")?.id, "a\u{0}b")
        }
    }

    func testTransactionRejectsInjectedIdentifiersAtomically() throws {
        try withDatabase { db in
            XCTAssertThrowsError(try db.transaction { tx in
                tx.upsertNode(label: "N", id: "first")
                tx.upsertNode(label: "N injected; upsert node N", id: "second")
            })
            XCTAssertNil(try db.node(id: "first"))
            XCTAssertNil(try db.node(id: "injected"))
        }
    }
}

extension TypedBoundaryTests {
    func testCodableScalarEncoderPreservesIntegralDoubles() throws {
        struct Values: Codable { let zero: Int64 = 0; let real: Double = 1; let negative: Double = -0.0 }
        try withDatabase { db in
            try db.upsertNode(label: "N", id: "model", model: Values())
            let node = try XCTUnwrap(db.node(id: "model"))
            XCTAssertEqual(node["zero"], .int(0))
            XCTAssertEqual(node["real"], .double(1))
            XCTAssertEqual(node["negative"]?.doubleValue?.sign, .minus)
        }
    }

    func testTypedBindingsProjectionAndMutationReceipt() throws {
        struct Projected: Decodable { let n: Int64; let text: String; let real: Double }
        try withDatabase { db in
            let text = "\u{0}雪\n\"\\; delete node v; //"
            let receipt = try db.mutate("upsert node N v set n=$n, text=$text, real=$real", parameters: ["n": .int(.max), "text": .string(text), "real": .double(1.0)])
            XCTAssertFalse(receipt.transactionId.isEmpty)
            XCTAssertGreaterThan(receipt.committedLSN, 0)
            let result = try db.queryResult("get node v return n, text, real")
            XCTAssertNil(result.receipt)
            let decoded = try result.decode(as: Projected.self)
            XCTAssertEqual(decoded.n, Int64.max); XCTAssertEqual(decoded.text, text); XCTAssertEqual(decoded.real, 1)
            XCTAssertEqual(try db.node(id: "v")?["real"], .double(1.0))
            XCTAssertThrowsError(try result.node())
            XCTAssertThrowsError(try db.query("get node v return text", as: Projected.self))
        }
    }

    func testTypedEdgesAndActualInboundPaths() throws {
        struct Relation: Codable, Sendable { let weight: Double; let order: Int64 }
        try withDatabase { db in
            let receipt = try db.transactionReceipt { tx in
                tx.upsertNode(label: "N", id: "a"); tx.upsertNode(label: "N", id: "b")
            }
            XCTAssertNotNil(receipt)
            try db.upsertEdge(type: "LINK", from: "a", to: "b", model: Relation(weight: 1.0, order: .max))
            let edge = try XCTUnwrap(db.edge(from: "a", type: "LINK", to: "b", as: Relation.self))
            XCTAssertEqual(edge.properties.order, .max); XCTAssertEqual(edge.properties.weight, 1)
            let paths = try db.from("b").in("LINK").paths()
            XCTAssertEqual(paths.count, 1)
            XCTAssertEqual(paths[0].nodes.map(\.id), ["b", "a"])
            XCTAssertEqual(paths[0].edges.map(\.from), ["a"])
            XCTAssertEqual(paths[0].edges[0]["weight"], .double(1.0))
        }
    }

    func testFailedBatchAndOptionsLeaveNoPartialWrites() throws {
        try withDatabase { db in
            XCTAssertThrowsError(try db.mutate("upsert node N first; upsert edge LINK first -> missing")) { error in
                let status = error as? GraphCoreError
                XCTAssertEqual(status?.context?.statementIndex, 2)
            }
            XCTAssertNil(try db.node(id: "first"))
            var options = GraphQueryOptions(); options.resultBytes = 1
            XCTAssertThrowsError(try db.mutate("upsert node N rejected", options: options)) { error in
                XCTAssertEqual((error as? GraphCoreError)?.code, "limitExceeded")
            }
            XCTAssertNil(try db.node(id: "rejected"))
            XCTAssertThrowsError(try db.queryResult(""))
            XCTAssertThrowsError(try db.queryResult("upsert node N nan set v=$v", parameters: ["v": .double(.nan)]))
            XCTAssertNil(try db.node(id: "nan"))
            XCTAssertNil(try db.transactionReceipt { _ in })
        }
    }

    func testVersionAndDecodeErrorsAreNotEmptySuccess() throws {
        XCTAssertThrowsError(try decodeV2(Data(#"{"schemaVersion":99,"ok":true,"data":[]}"#.utf8)))
        XCTAssertThrowsError(try decodeV2(Data(#"{"schemaVersion":2,"ok":true}"#.utf8)))
        XCTAssertThrowsError(try decodeV2(Data(#"{"schemaVersion":2,"ok":false}"#.utf8)))
        XCTAssertThrowsError(try JSONDecoder().decode(GraphValue.self, from: Data("{\"nested\":1}".utf8)))
        try withDatabase { db in
            XCTAssertThrowsError(try db.rawQuery("upsert node N n set v=$v", parameters: ["v": ["nested": 1]]))
            XCTAssertNil(try db.node(id: "n"))
        }
    }
}

extension TypedBoundaryTests {
    func testNonfiniteFiltersAreRejected() throws {
        try withDatabase { db in
            XCTAssertThrowsError(try db.nodes(label: "N", where: "v", equals: .double(.nan)))
            XCTAssertThrowsError(try db.nodes(label: "N", where: "v", equals: .double(.infinity)))
        }
    }
    func testCodableRejectsUnsignedOverflowArraysAndNonfiniteValues() throws {
        struct TooLarge: Encodable { let value = UInt64.max }
        struct ArrayProperty: Encodable { let value = [1, 2] }
        struct Nonfinite: Encodable { let value = Double.infinity }
        try withDatabase { db in
            XCTAssertThrowsError(try db.upsertNode(label: "N", id: "bad", model: TooLarge()))
            XCTAssertThrowsError(try db.upsertNode(label: "N", id: "bad", model: ArrayProperty()))
            XCTAssertThrowsError(try db.upsertNode(label: "N", id: "bad", model: Nonfinite()))
            XCTAssertNil(try db.node(id: "bad"))
        }
    }
}

extension TypedBoundaryTests {
    func testProjectionContainersCannotBeMistakenForScalarTags() throws {
        struct Projection: Decodable { let type: String; let value: String? }
        struct Batch: Decodable { let results: [Projection] }
        struct Route: Decodable { let nodes: [Projection]; let edges: [Projection] }
        struct Traversal: Decodable { let nodes: [Projection]; let paths: [Route] }
        try withDatabase { db in
            try db.transaction { tx in
                tx.upsertNode(label: "null", id: "a")
                tx.upsertNode(label: "string", id: "b")
                tx.upsertEdge(type: "null", from: "a", to: "b")
            }
            let raw = try db.rawQuery("get node a return type, value")
            let projection = try JSONDecoder().decode(Projection.self, from: Data(raw.utf8))
            XCTAssertEqual(projection.type, "null"); XCTAssertNil(projection.value)
            let single = try db.query("get node b return type, value", as: Projection.self)
            XCTAssertEqual(single.type, "string"); XCTAssertNil(single.value)
            let list = try db.query("find nodes null return type, value", as: [Projection].self)
            XCTAssertEqual(list.map(\.type), ["null"])
            let batch = try db.query("get node a return type, value; get node b return type, value", as: Batch.self)
            XCTAssertEqual(batch.results.map(\.type), ["null", "string"])
            let traversal = try db.query("walk from a over null paths return type, value", as: Traversal.self)
            XCTAssertEqual(traversal.nodes.map(\.type), ["string"])
            XCTAssertEqual(traversal.paths[0].nodes.map(\.type), ["null", "string"])
            XCTAssertEqual(traversal.paths[0].edges.map(\.type), ["null"])
        }
    }
}

extension TypedBoundaryTests {
    func testByteDistinctUnicodeKeysArePreservedRawAndRejectedTyped() throws {
        let composed = "\u{00E9}", decomposed = "e\u{0301}"
        try withDatabase { db in
            _ = try db.mutate("upsert node N unicode set \"\(composed)\"=1, \"\(decomposed)\"=2")
            for query in ["get node unicode", "get node unicode return \"\(composed)\", \"\(decomposed)\""] {
                let raw = Array(try db.rawQuery(query).utf8)
                for (key, value) in [(composed, 1), (decomposed, 2)] {
                    let expected = Array("\"\(key)\":\(value)".utf8)
                    XCTAssertTrue(raw.indices.contains { start in start + expected.count <= raw.count && Array(raw[start..<(start + expected.count)]) == expected })
                }
                XCTAssertThrowsError(try db.queryResult(query)) { error in
                    guard case GraphDBError.decodingFailed(let message) = error else { return XCTFail("\(error)") }
                    XCTAssertTrue(message.contains("Unicode"))
                }
            }
            XCTAssertThrowsError(try db.node(id: "unicode"))
        }
    }
}
