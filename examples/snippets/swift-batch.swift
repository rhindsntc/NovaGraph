try db.transaction { tx in
    tx.upsertNode(label: "Person", id: "ada", properties: ["name": "Ada"])
    tx.upsertNode(label: "Person", id: "grace", properties: ["name": "Grace"])
    tx.upsertEdge(type: "FOLLOWS", from: "ada", to: "grace")
}
// The batch commits atomically; a failed statement publishes none of it.
let grace = try db.node(id: "grace")
precondition(grace != nil)
