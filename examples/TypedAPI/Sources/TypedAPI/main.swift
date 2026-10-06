import Foundation
import GraphDBKit

struct Person: Codable, Sendable { let name: String; let rank: Int64 }
struct Link: Codable, Sendable { let weight: Double }
struct Projection: Decodable { let name: String; let rank: Int64 }

let directory = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent(UUID().uuidString)
defer { try? FileManager.default.removeItem(at: directory) }
do {
    let db = try GraphDatabase(path: directory)
    try db.upsertNode(label: "Person", id: "ada", model: Person(name: "Ada", rank: .max))
    try db.upsertNode(label: "Person", id: "bob", model: Person(name: "Bob", rank: 0))
    try db.upsertEdge(type: "KNOWS", from: "ada", to: "bob", model: Link(weight: 1.0))

    let receipt = try db.mutate("upsert node Person grace set name=$name, rank=$rank",
        parameters: ["name": .string("Grace"), "rank": .int(1)])
    let projected: [Projection] = try db.query("find nodes Person where rank=$rank return name, rank",
        parameters: ["rank": .int(.max)], as: [Projection].self)
    let edge = try db.edge(from: "ada", type: "KNOWS", to: "bob", as: Link.self)
    let base = db.from("ada").out("KNOWS")
    let paths = try base.depth(2).paths()

    precondition(receipt.committedLSN > 0 && !receipt.transactionId.isEmpty)
    precondition(projected.count == 1 && projected[0].name == "Ada" && projected[0].rank == .max)
    precondition(edge?.properties.weight == 1.0)
    precondition(paths.count == 1 && paths[0].nodes.map(\.id) == ["ada", "bob"])
    precondition(paths[0].edges[0]["weight"] == .double(1.0))
    print("Typed API example passed: exact scalars, projection, receipt, edge and real path.")
}
