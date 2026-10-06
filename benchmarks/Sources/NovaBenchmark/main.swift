import Foundation
import GraphDBKit

struct TimingStats {
    let name: String
    let iterations: Int
    let totalSeconds: Double
    let latenciesUs: [Double]

    var p50Us: Double { percentile(0.50) }
    var p95Us: Double { percentile(0.95) }
    var p99Us: Double { percentile(0.99) }
    var opsPerSec: Double { Double(iterations) / max(totalSeconds, 0.000001) }

    private func percentile(_ p: Double) -> Double {
        guard !latenciesUs.isEmpty else { return 0 }
        let sorted = latenciesUs.sorted()
        let idx = Int(Double(sorted.count - 1) * p)
        return sorted[idx]
    }

    func printSummary() {
        print(String(format: "%-30@ | %7d ops | %9.1f ops/s | p50: %6.1f µs | p95: %6.1f µs | p99: %6.1f µs",
                     name, iterations, opsPerSec, p50Us, p95Us, p99Us))
    }
}

func measure(name: String, iterations: Int, operation: (Int) throws -> Void) rethrows -> TimingStats {
    var latencies: [Double] = []
    latencies.reserveCapacity(iterations)

    let startTotal = DispatchTime.now().uptimeNanoseconds
    for i in 0..<iterations {
        let t0 = DispatchTime.now().uptimeNanoseconds
        try operation(i)
        let t1 = DispatchTime.now().uptimeNanoseconds
        latencies.append(Double(t1 - t0) / 1000.0) // microseconds
    }
    let endTotal = DispatchTime.now().uptimeNanoseconds
    let totalSec = Double(endTotal - startTotal) / 1_000_000_000.0

    return TimingStats(name: name, iterations: iterations, totalSeconds: totalSec, latenciesUs: latencies)
}

struct BenchUser: Codable, Sendable {
    let name: String
    let email: String
    let score: Double
}

struct BenchmarkFailure: Error, CustomStringConvertible {
    let description: String
}

// No database handle or closure escapes this function. Its return releases the
// original directory owner before the caller measures a fresh open.
func runDatabasePhases(at tmpDir: URL, smoke: Bool, nodeCount: Int) throws {
    let readRange = min(nodeCount - 1, 500)
    let db = try GraphDatabase(path: tmpDir, inactivityInterval: 0.001, sweepInterval: 100)
    try db.createIndex(label: "User", property: "email")

    // 1. Bulk Node Mutations via Codable
    let nodeMutStats = try measure(name: "1. Bulk Node Upserts (Codable)", iterations: nodeCount) { i in
        let user = BenchUser(name: "User \(i)", email: "user\(i)@nova.bench", score: Double(i * 10))
        try db.upsertNode(label: "User", id: "u:\(i)", model: user)
    }
    nodeMutStats.printSummary()

    // 2. Bulk Edge Mutations
    let edgeMutStats = try measure(name: "2. Bulk Edge Upserts", iterations: nodeCount - 1) { i in
        try db.upsertEdge(type: "FOLLOWS", from: "u:\(i)", to: "u:\(i + 1)", properties: [
            "weight": .double(1.0)
        ])
    }
    edgeMutStats.printSummary()

    // 3. Hot Point Reads
    let hotReadStats = try measure(name: "3. Point Reads (Hot)", iterations: nodeCount) { i in
        _ = try db.node(id: "u:\(i % readRange)")
    }
    hotReadStats.printSummary()

    // 4. Indexed Property Find
    let findStats = try measure(name: "4. Indexed Finds", iterations: smoke ? 16 : 1000) { i in
        let target = i % readRange
        let res = try db.nodes(label: "User", where: "email", equals: .string("user\(target)@nova.bench"), limit: 1)
        if res.isEmpty { throw BenchmarkFailure(description: "Indexed find returned no match") }
    }
    findStats.printSummary()

    // 5. Outbound Graph Walk (depth 1 & 2)
    let walk1Stats = try measure(name: "5. Outbound Walk (Depth 1)", iterations: smoke ? 16 : 1000) { i in
        let start = i % readRange
        _ = try db.from("u:\(start)").out("FOLLOWS").depth(1).limit(10).collect()
    }
    walk1Stats.printSummary()

    let walk2Stats = try measure(name: "6. Outbound Walk (Depth 2)", iterations: smoke ? 8 : 500) { i in
        let start = i % readRange
        _ = try db.from("u:\(start)").out("FOLLOWS").depth(2).limit(25).collect()
    }
    walk2Stats.printSummary()

    // 7. Inbound Graph Walk
    let inWalkStats = try measure(name: "7. Inbound Walk (Depth 1)", iterations: smoke ? 16 : 1000) { i in
        let target = (i % readRange) + 1
        _ = try db.from("u:\(target)").in("FOLLOWS").depth(1).limit(10).collect()
    }
    inWalkStats.printSummary()

    // 8. Tiering & Memory Eviction
    Thread.sleep(forTimeInterval: 0.01)
    let trimStats = try measure(name: "8. Memory Trim / Eviction", iterations: 1) { _ in
        let evicted = try db.trimMemory()
        print("   -> Evicted \(evicted) cold records to disk")
    }
    trimStats.printSummary()

    // 9. Cold Point Reads (disk promotion)
    let coldReadStats = try measure(name: "9. Point Reads (Cold Disk)", iterations: smoke ? 8 : 200) { i in
        _ = try db.node(id: "u:\(i)")
    }
    coldReadStats.printSummary()

    // 10. Checkpoint before releasing the original handle
    let ckptStats = try measure(name: "10. Checkpoint / Catalog", iterations: 1) { _ in
        try db.checkpoint()
    }
    ckptStats.printSummary()

}

func runBenchmarks(smoke: Bool) throws {
    print("==========================================================================================")
    print("                           NovaGraph Embedded Performance Benchmarks                      ")
    if smoke { print("SMOKE WORKLOAD — execution check only; not performance evidence") }
    print("==========================================================================================")

    let tmpDir = URL(fileURLWithPath: NSTemporaryDirectory())
        .appendingPathComponent("nova-benchmark-\(UUID().uuidString)")
    defer { try? FileManager.default.removeItem(at: tmpDir) }
    let nodeCount = smoke ? 32 : 2000
    try runDatabasePhases(at: tmpDir, smoke: smoke, nodeCount: nodeCount)

    let recoveryStats = try measure(name: "11. Clean Reopen / Read / Close", iterations: 1) { _ in
        let reopened = try GraphDatabase(path: tmpDir)
        let n = try reopened.node(id: "u:\(min(100, nodeCount - 1))")
        guard n != nil else { throw BenchmarkFailure(description: "Reopened database is missing the expected node") }
    }
    recoveryStats.printSummary()

    print("==========================================================================================")
    print("                                Benchmark Suite Completed!                                ")
    print("==========================================================================================")
}

do {
    let arguments = Array(CommandLine.arguments.dropFirst())
    guard arguments.isEmpty || arguments == ["--smoke"] else {
        throw BenchmarkFailure(description: "Usage: NovaBenchmark [--smoke]")
    }
    try runBenchmarks(smoke: arguments == ["--smoke"])
} catch {
    FileHandle.standardError.write(Data("Benchmark failed: \(error)\n".utf8))
    exit(1)
}
