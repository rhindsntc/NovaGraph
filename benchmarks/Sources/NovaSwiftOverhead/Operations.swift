import Foundation
import GraphDBKit

let operationLanes = ["swiftColdUs", "swiftSingleMutationUs", "swiftBatch250Us", "swiftTraversalUs", "swiftAsyncReadUs", "swiftAsyncTraversalUs", "swiftAsyncMutationUs"]
func operationsProtocol(contract: Bool) -> [String: Any] {
    ["version": 1, "mode": contract ? "contract" : "measure", "warmup": contract ? 1 : 20,
     "samples": contract ? 2 : 100, "batchMutations": 250, "traversalNodes": 32,
     "traversalDepth": 2, "traversalLimit": 100, "graph": "directed-ring",
     "cold": "unique-after-forced-trim", "durability": "default-durable-receipt",
     "mutation": "replace-string-and-integer-properties", "asyncConcurrency": 1, "phaseOrder": ["cold", "single", "batch", "traversal", "asyncRead", "asyncTraversal", "asyncMutation"]]
}
private func elapsed(_ start: UInt64) -> Double { Double(DispatchTime.now().uptimeNanoseconds - start) / 1000 }
private func validateNode(_ node: Node?, index: Int, payload: String, ordinal: Int? = nil) throws {
    try check(node?.id == "n:\(index)" && node?.label == "Record"
        && node?.properties["payload"] == .string(payload)
        && node?.properties["ordinal"] == .int(Int64(ordinal ?? index)))
}
private func readAndValidate(_ db: GraphDatabase, index: Int, payload: String, ordinal: Int? = nil) throws {
    try validateNode(db.node(id: "n:\(index)"), index: index, payload: payload, ordinal: ordinal)
}
private func populate(_ db: GraphDatabase, count: Int, payload: String, ring: Bool = false) throws {
    try db.transaction { tx in
        for i in 0..<count { tx.upsertNode(label: "Record", id: "n:\(i)", properties: ["payload": .string(payload), "ordinal": .int(Int64(i))]) }
    }
    if ring {
        try db.transaction { tx in
            for i in 0..<count { tx.upsertEdge(type: "NEXT", from: "n:\(i)", to: "n:\((i + 1) % count)") }
        }
    }
}
private func validateTraversal(_ nodes: [Node], start: Int, payload: String) throws {
    let expected = Set(["n:\((start + 1) % 32)", "n:\((start + 2) % 32)"])
    try check(nodes.count == 2 && Set(nodes.map(\.id)) == expected)
    for node in nodes {
        guard let index = Int(node.id.dropFirst(2)) else { try check(false); return }
        try validateNode(node, index: index, payload: payload)
    }
}
private func closeDB(_ db: GraphDatabase) throws { try db.close() }
private func reopen(_ db: GraphDatabase, path: URL, count: Int, payload: String, ordinal: Int? = nil, ring: Bool = false) throws {
    try db.close()
    let reopened = try GraphDatabase(path: path)
    defer { try? reopened.close() }
    for i in 0..<count { try readAndValidate(reopened, index: i, payload: payload, ordinal: ordinal) }
    if ring { try validateTraversal(reopened.traverse(from: "n:0", over: "NEXT", depth: 2, limit: 100), start: 0, payload: payload) }
    try reopened.close()
}
private func coldPhase(path: URL, payload: String, warmup: Int, samples: Int) throws -> ([Double], Int) {
    let db = try GraphDatabase(path: path)
    defer { try? db.close() }
    let count = warmup + samples
    try populate(db, count: count, payload: payload)
    // Forced trim is outside timing. Its exact eviction count proves every measured
    // first read is a cold-store promotion; the OS file cache is not flushed.
    let evicted = try db.trimMemory()
    try check(evicted == count)
    var values = [Double]()
    for i in 0..<count {
        let id = "n:\(i)"
        let start = DispatchTime.now().uptimeNanoseconds
        let node = try db.node(id: id)
        let time = elapsed(start)
        try validateNode(node, index: i, payload: payload)
        // Repeated reads verify promotions retain exact scalar values.
        try readAndValidate(db, index: i, payload: payload)
        if i >= warmup { values.append(time) }
    }
    try reopen(db, path: path, count: count, payload: payload)
    return (values, evicted)
}
private func mutationPhase(path: URL, payload: String, count: Int, warmup: Int, samples: Int) throws -> [Double] {
    let db = try GraphDatabase(path: path)
    defer { try? db.close() }
    try populate(db, count: count, payload: payload)
    var values = [Double](), priorLSN: UInt64 = 0
    for iteration in 0..<(warmup + samples) {
        let ordinal = Int64(1000 + iteration)
        let start = DispatchTime.now().uptimeNanoseconds
        let receipt = try db.transactionReceipt { tx in
            for i in 0..<count { tx.upsertNode(label: "Record", id: "n:\(i)", properties: ["payload": .string(payload), "ordinal": .int(ordinal)]) }
        }
        let time = elapsed(start)
        guard let receipt else { throw NSError(domain: "Missing durable receipt", code: 5) }
        try check(!receipt.transactionId.isEmpty && receipt.committedLSN > priorLSN)
        priorLSN = receipt.committedLSN
        for i in 0..<count { try readAndValidate(db, index: i, payload: payload, ordinal: Int(ordinal)) }
        if iteration >= warmup { values.append(time) }
    }
    try reopen(db, path: path, count: count, payload: payload, ordinal: 999 + warmup + samples)
    return values
}
private func syncTraversalPhase(_ db: GraphDatabase, payload: String, warmup: Int, samples: Int) throws -> [Double] {
    var values = [Double]()
    for iteration in 0..<(warmup + samples) {
        let index = iteration % 32, id = "n:\(iteration % 32)"
        let start = DispatchTime.now().uptimeNanoseconds
        let nodes = try db.traverse(from: id, over: "NEXT", depth: 2, limit: 100)
        let time = elapsed(start)
        try validateTraversal(nodes, start: index, payload: payload)
        if iteration >= warmup { values.append(time) }
    }
    return values
}
func operationScenario(payloadBytes: Int, contract: Bool) async throws -> [String: Any] {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("nova-swift-operations-\(UUID())")
    defer { try? FileManager.default.removeItem(at: root) }
    let payload = String(repeating: "x", count: payloadBytes), warmup = contract ? 1 : 20, samples = contract ? 2 : 100
    let (cold, evicted) = try coldPhase(path: root.appendingPathComponent("cold"), payload: payload, warmup: warmup, samples: samples)
    let single = try mutationPhase(path: root.appendingPathComponent("single"), payload: payload, count: 1, warmup: warmup, samples: samples)
    let batch = try mutationPhase(path: root.appendingPathComponent("batch"), payload: payload, count: 250, warmup: warmup, samples: samples)
    let graphPath = root.appendingPathComponent("graph"), asyncPath = root.appendingPathComponent("async-mutation")
    let graph = try GraphDatabase(path: graphPath), mutations = try GraphDatabase(path: asyncPath)
    defer { try? closeDB(graph); try? closeDB(mutations) }
    try populate(graph, count: 32, payload: payload, ring: true)
    try populate(mutations, count: 1, payload: payload)
    let traversal = try syncTraversalPhase(graph, payload: payload, warmup: warmup, samples: samples)
    var asyncRead = [Double](), asyncTraversal = [Double](), asyncMutation = [Double]()
    // Serial awaits measure the public API's request, scheduling, operation and
    // decoding through return to the caller. They are not queue-only overhead.
    for iteration in 0..<(warmup + samples) {
        let index = iteration % 32, id = "n:\(iteration % 32)"
        let start = DispatchTime.now().uptimeNanoseconds
        let node = try await graph.node(id: id)
        let time = elapsed(start)
        try validateNode(node, index: index, payload: payload)
        if iteration >= warmup { asyncRead.append(time) }
    }
    for iteration in 0..<(warmup + samples) {
        let index = iteration % 32, id = "n:\(iteration % 32)"
        let start = DispatchTime.now().uptimeNanoseconds
        let nodes = try await graph.traverse(from: id, over: "NEXT", depth: 2, limit: 100)
        let time = elapsed(start)
        try validateTraversal(nodes, start: index, payload: payload)
        if iteration >= warmup { asyncTraversal.append(time) }
    }
    var priorLSN: UInt64 = 0
    for iteration in 0..<(warmup + samples) {
        let ordinal = 1000 + iteration
        let query = "upsert node Record n:0 set payload=\(GraphValue.string(payload).ngqlLiteral), ordinal=\(ordinal)"
        let start = DispatchTime.now().uptimeNanoseconds
        let receipt = try await mutations.mutate(query)
        let time = elapsed(start)
        try check(!receipt.transactionId.isEmpty && receipt.committedLSN > priorLSN)
        priorLSN = receipt.committedLSN
        try readAndValidate(mutations, index: 0, payload: payload, ordinal: ordinal)
        if iteration >= warmup { asyncMutation.append(time) }
    }
    try reopen(graph, path: graphPath, count: 32, payload: payload, ring: true)
    try reopen(mutations, path: asyncPath, count: 1, payload: payload, ordinal: 999 + warmup + samples)
    return ["payloadBytes": payloadBytes, "coldNodes": warmup + samples, "coldEvicted": evicted,
            "coldVisited": warmup + samples, "reopenValidated": true, "batchMutations": 250,
            "traversalNodes": 32, "traversalEdges": 32, "traversalResults": 2,
            "swiftColdUs": cold, "swiftSingleMutationUs": single, "swiftBatch250Us": batch,
            "swiftTraversalUs": traversal, "swiftAsyncReadUs": asyncRead,
            "swiftAsyncTraversalUs": asyncTraversal, "swiftAsyncMutationUs": asyncMutation]
}
