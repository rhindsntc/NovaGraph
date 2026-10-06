import Foundation
import GraphDBKit

enum SmokeFailure: Error { case unexpected(String) }
func require(_ condition: Bool, _ message: String) throws {
    if !condition { throw SmokeFailure.unexpected(message) }
}
guard CommandLine.arguments.count == 3 else { throw SmokeFailure.unexpected("Use create|reopen DATABASE") }
let mode = CommandLine.arguments[1]
let db = try GraphDatabase(path: URL(fileURLWithPath: CommandLine.arguments[2]))
if mode == "create" {
    try db.upsertNode(label: "Person", id: "ada", properties: ["name": .string("Ada"), "rank": .int(.max), "active": .bool(true)])
    try db.upsertNode(label: "Person", id: "grace")
    try db.upsertEdge(type: "KNOWS", from: "ada", to: "grace", properties: ["weight": .double(1.0)])
} else { try require(mode == "reopen", "unknown mode") }
let ada = try db.node(id: "ada")
try require(ada?["rank"] == .int(.max) && ada?["active"] == .bool(true), "scalar fidelity")
let rows: [Person] = try db.query("find nodes Person where name=$name return name limit 10", parameters: ["name": .string("Ada")], as: [Person].self)
try require(rows.map(\.name) == ["Ada"], "parameter/projection result")
let paths = try db.from("ada").out("KNOWS").depth(1).paths()
try require(paths.count == 1 && paths[0].nodes.map(\.id) == ["ada", "grace"], "path nodes")
try require(paths[0].edges[0]["weight"] == .double(1.0), "edge scalar")
try db.close()
print("IntegrationSmoke \(mode): values, query and path passed")
struct Person: Decodable { let name: String }
