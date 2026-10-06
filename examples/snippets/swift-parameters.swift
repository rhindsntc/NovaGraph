let result = try db.queryResult(
    "find nodes Person where email = $email limit 1",
    parameters: ["email": .string("ada@example.com")]
)
// Binding preserves quotes, equals signs and scalar types.
let parameterNodes = try result.nodes()
precondition(parameterNodes.map(\.id) == ["ada"])
precondition(parameterNodes[0].properties["email"] == .string("ada@example.com"))
precondition(parameterNodes[0].properties["name"] == .string("Ada"))
