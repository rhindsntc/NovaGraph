import Foundation
import GraphDBKit

struct Person: Codable, Sendable {
    let name: String
    let email: String
    let role: String
    let age: Int
}

struct Project: Codable, Sendable {
    let title: String
    let status: String
}

@main
struct NovaGraphDemoApp {
    static func main() async {
        print("==================================================================")
        print("                 NovaGraph Embedded Demo (GraphDBKit)             ")
        print("==================================================================")

        let dbDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("novagraph_demo_\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: dbDir) }

        do {
            print("\n1. Initializing embedded database at: \(dbDir.path)")
            let db = try GraphDatabase(
                path: dbDir,
                inactivityInterval: 600,
                sweepInterval: 30
            )

            print("2. Declaring schema & property indexes...")
            try db.createIndex(label: "Person", property: "email")
            try db.createIndex(label: "Project", property: "status")

            print("3. Executing atomic batch transaction...")
            try db.transaction { tx in
                tx.upsertNode(label: "Person", id: "user:ada", properties: [
                    "name": "Ada Lovelace",
                    "email": "ada@nova.io",
                    "role": "Chief Architect",
                    "age": 36
                ])
                tx.upsertNode(label: "Person", id: "user:grace", properties: [
                    "name": "Grace Hopper",
                    "email": "grace@nova.io",
                    "role": "Systems Lead",
                    "age": 42
                ])
                tx.upsertNode(label: "Person", id: "user:margaret", properties: [
                    "name": "Margaret Hamilton",
                    "email": "margaret@nova.io",
                    "role": "Reliability Lead",
                    "age": 31
                ])
                tx.upsertNode(label: "Project", id: "proj:compiler", properties: [
                    "title": "Nova C++ Core",
                    "status": "production"
                ])

                tx.upsertEdge(type: "COLLABORATES", from: "user:ada", to: "user:grace", properties: [
                    "since": 2024
                ])
                tx.upsertEdge(type: "COLLABORATES", from: "user:grace", to: "user:margaret", properties: [
                    "since": 2025
                ])
                tx.upsertEdge(type: "LEADS", from: "user:grace", to: "proj:compiler")
            }
            print("   -> 4 nodes and 3 edges inserted atomically.")

            print("\n4. Typed Codable point queries:")
            if let ada = try db.node(id: "user:ada", as: Person.self) {
                print("   Found Node: \(ada.id) [\(ada.label)] -> Name: \(ada.properties.name), Role: \(ada.properties.role)")
            }

            print("\n5. Fluent Traversal DSL (2-hop outbound collaboration from user:ada):")
            let collaborators = try await db.from("user:ada")
                .out("COLLABORATES")
                .depth(2)
                .limit(10)
                .collect(as: Person.self)

            for person in collaborators {
                print("   - Collaborator reached: \(person.id) -> \(person.properties.name) (\(person.properties.role))")
            }

            print("\n6. Parameterized NGQL Query:")
            let paramResult = try await db.queryResult(
                "find nodes Person where email = $targetEmail limit 1;",
                parameters: ["targetEmail": "grace@nova.io"]
            )
            print("   Result: \(paramResult)")

            print("\n7. Query Explain Plan:")
            let plan = try await db.explain("find nodes Person where email = \"ada@nova.io\" limit 1")
            print("   Execution Plan: \(plan)")

            print("\n8. Simulating Apple Memory Pressure Event:")
            _ = try await db.handleMemoryWarning()
            print("   -> Hot tier trimmed to durable cold store successfully.")

            print("\n9. Flushing checkpoint to disk...")
            try await db.checkpoint()
            print("   -> Checkpoint published to the current catalog.")

            try await db.close()

            print("\n==================================================================")
            print("                 NovaGraph Demo Finished Successfully!            ")
            print("==================================================================")
        } catch {
            print("Error running demo: \(error)")
            exit(1)
        }
    }
}
