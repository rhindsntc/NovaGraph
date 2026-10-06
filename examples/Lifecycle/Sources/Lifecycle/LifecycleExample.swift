import Foundation
import GraphDBKit

@main struct LifecycleExample {
    @MainActor static func main() async throws {
        let path = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        let db = try GraphDatabase(path: path)
        let receipt = try await db.mutate("upsert node Person ada set name='Ada'")
        let names = try await db.readSnapshot { snapshot in
            let first = try snapshot.node(id: "ada")
            let second = try snapshot.node(id: "ada")
            precondition(first?.properties == second?.properties)
            return first?.properties["name"]?.stringValue
        }
        precondition(names == "Ada" && receipt.committedLSN > 0)
        let cancelled = try GraphRequest(); cancelled.cancel()
        do {
            _ = try db.queryResult("upsert node Person cancelled", request: cancelled)
            preconditionFailure("Cancelled mutation succeeded")
        } catch let error as GraphCoreError { precondition(error.code == "cancelled") }
        let lifecycle = GraphLifecycle(database: db)
        try await lifecycle.send(.memoryPressure).value
        // A command-line program has no UIKit background assertion to acquire.
        #if !canImport(UIKit)
        try await lifecycle.send(.background).value
        try await lifecycle.send(.foreground).value
        #endif
        try await db.close()
        let reopened = try GraphDatabase(path: path)
        let ada = try await reopened.node(id: "ada")
        precondition(ada != nil)
        try await reopened.close()
        try FileManager.default.removeItem(at: path)
        print("Lifecycle example passed")
    }
}
