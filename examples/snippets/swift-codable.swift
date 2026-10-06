struct PersonProps: Codable, Sendable {
    let name: String
    let email: String
}
try db.upsertNode(label: "Person", id: "ada", model:
    PersonProps(name: "Ada", email: "ada@example.com"))
let person = try db.node(id: "ada", as: PersonProps.self)
precondition(person?.properties.name == "Ada" && person?.properties.email == "ada@example.com")
