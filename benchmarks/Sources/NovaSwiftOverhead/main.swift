import Foundation
import GraphDBKit
import CGraphDB

func check(_ condition: Bool, file: String = #fileID, line: Int = #line) throws {
    if !condition { throw NSError(domain: "NovaSwiftOverhead", code: 1, userInfo: [NSLocalizedDescriptionKey: "Correctness check failed at \(file):\(line)"]) }
}
func consume(_ value: GraphDBString) throws -> Data {
    defer { graphdb_string_free(value) }
    guard let bytes = value.data, value.len > 0 else { throw NSError(domain: "NovaSwiftOverhead", code: 2) }
    return Data(bytes: bytes, count: Int(value.len))
}
func decode(_ data: Data) throws -> [String: Any] {
    let envelope = try JSONSerialization.jsonObject(with: data) as? [String: Any]
    try check(envelope?["ok"] as? Bool == true)
    return envelope?["data"] as? [String: Any] ?? [:]
}
func scenario(payloadBytes: Int, orderOffset: Int) throws -> [String: Any] {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("nova-swift-overhead-\(UUID())")
    defer { try? FileManager.default.removeItem(at: root) }
    let swiftDB = try GraphDatabase(path: root.appendingPathComponent("swift"))
    defer { try? swiftDB.close() }
    let opened = root.appendingPathComponent("c").path.withCString { graphdb_open_result($0, 600000, 30000) }
    graphdb_string_free(opened.error)
    guard let cDB = opened.handle else { throw NSError(domain: "NovaSwiftOverhead", code: 3) }
    defer { graphdb_release(cDB) }
    func raw(_ query: String) throws -> Data {
        try query.withCString { try consume(graphdb_execute_query_v2(cDB, $0, UInt64(query.utf8.count), nil, 0, nil)) }
    }
    // ASCII payload sizes match the small, message and knowledge native profiles.
    let payload = String(repeating: "x", count: payloadBytes)
    for index in 0..<32 {
        try swiftDB.upsertNode(label: "Record", id: "n:\(index)", properties: ["payload": .string(payload), "ordinal": .int(Int64(index))])
        _ = try decode(raw("upsert node Record n:\(index) set payload=\"\(payload)\", ordinal=\(index)"))
    }
    func validate(_ record: [String: Any], index: Int) throws {
        let properties = record["properties"] as? [String: Any]
        let text = properties?["payload"] as? [String: Any]
        let ordinal = properties?["ordinal"] as? [String: Any]
        try check(record["id"] as? String == "n:\(index)" && record["label"] as? String == "Record"
                  && text?["type"] as? String == "string" && text?["value"] as? String == payload
                  && ordinal?["type"] as? String == "int" && ordinal?["value"] as? Int == index)
    }
    var times = Array(repeating: [Double](), count: 3)
    var visited = Set<Int>()
    for iteration in 0..<120 {
        let index = iteration % 32
        let query = "get node n:\(index)"
        let identifier = "n:\(index)"
        if iteration >= 20 { visited.insert(index) }
        for position in 0..<3 {
            let lane = (iteration + position + orderOffset) % 3
            let elapsed: Double
            switch lane {
            case 0:
                let start = DispatchTime.now().uptimeNanoseconds
                let data = try raw(query)
                elapsed = Double(DispatchTime.now().uptimeNanoseconds - start) / 1000
                try validate(decode(data), index: index)
            case 1:
                let start = DispatchTime.now().uptimeNanoseconds
                let record = try decode(raw(query))
                elapsed = Double(DispatchTime.now().uptimeNanoseconds - start) / 1000
                try validate(record, index: index)
            default:
                let start = DispatchTime.now().uptimeNanoseconds
                let node = try swiftDB.node(id: identifier)
                elapsed = Double(DispatchTime.now().uptimeNanoseconds - start) / 1000
                try check(node?.id == identifier && node?.label == "Record"
                          && node?.properties["payload"] == .string(payload)
                          && node?.properties["ordinal"] == .int(Int64(index)))
            }
            // Setup, expected-value checks and sample bookkeeping are outside each timed lane.
            if iteration >= 20 { times[lane].append(elapsed) }
        }
    }
    try swiftDB.close()
    _ = try decode(consume(graphdb_close_v2(cDB, nil)))
    return ["payloadBytes": payloadBytes, "nodes": 32, "visitedNodes": visited.count,
            "cTransportUs": times[0], "cJSONUs": times[1], "swiftTypedUs": times[2]]
}
func run() async throws {
    var args = Array(CommandLine.arguments.dropFirst())
    let contract = args.last == "--contract"
    if contract { args.removeLast() }
    let offset: Int
    if args.isEmpty { offset = 0 }
    else {
        guard args.count == 2, args[0] == "--order-offset", let parsed = Int(args[1]), (0..<3).contains(parsed)
        else { throw NSError(domain: "Expected --order-offset 0, 1 or 2", code: 4) }
        offset = parsed
    }
    let scenarios = try [64, 512, 1024].map { try scenario(payloadBytes: $0, orderOffset: offset) }
    var operations = [[String: Any]]()
    for size in [64, 512, 1024] { operations.append(try await operationScenario(payloadBytes: size, contract: contract)) }
    let out: [String: Any] = ["schemaVersion": 3, "operationsProtocol": operationsProtocol(contract: contract), "operationScenarios": operations, "validated": true, "warmup": 20, "samples": 100,
        "orderOffset": offset, "scenarios": scenarios,
        "scope": "Hot comparison only: macOS hot reads across 32 records; C transport includes buffer copy/free; C JSON adds Foundation envelope decoding; typed Swift includes decoding and request lifetime. Rotating lane order. Not pure language overhead, a cold-read benchmark or a device guarantee."]
    print(String(decoding: try JSONSerialization.data(withJSONObject: out, options: [.sortedKeys]), as: UTF8.self))
}
do { try await run() }
catch { FileHandle.standardError.write(Data("Swift overhead failed: \(error)\n".utf8)); exit(1) }
