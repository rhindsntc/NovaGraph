import Foundation
import GraphDBKit

// Each setup group uses a fresh directory and closes before cleanup.
let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
defer { try? FileManager.default.removeItem(at: directory) }
let db = try GraphDatabase(path: directory)
try db.createIndex(label: "Person", property: "email")
try db.upsertNode(label: "Person", id: "ada", properties: [
    "name": .string("Ada"), "email": .string("ada@example.com")
])
let ada = try db.node(id: "ada")
let matches = try db.nodes(label: "Person", where: "email", equals: .string("ada@example.com"))
precondition(ada?.properties["name"] == .string("Ada") && matches.map(\.id) == ["ada"])
