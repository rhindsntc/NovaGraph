import SwiftUI
import Foundation
#if WITH_NOVA
import GraphDBKit
#endif
func probe() throws -> String {
#if WITH_NOVA
    let path = FileManager.default.temporaryDirectory.appendingPathComponent("nova-size-" + UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: path) }
    let db = try GraphDatabase(path: path)
    try db.upsertNode(label: "Item", id: "one", properties: ["name": .string("verified")])
    guard try db.node(id: "one")?["name"] == .string("verified") else { throw NSError(domain: "SizeProbe", code: 1) }
    try db.close()
    let reopened = try GraphDatabase(path: path)
    guard try reopened.node(id: "one") != nil else { throw NSError(domain: "SizeProbe", code: 2) }
    try reopened.close()
#endif
#if WITH_NOVA
    return "verified-nova"
#else
    return "verified-baseline"
#endif
}
@main struct FootprintApp: App {
    @State private var status = "Ready"
    init() {
        if CommandLine.arguments.contains("--verify") {
            do { print(try probe()); exit(0) }
            catch { print(error); exit(1) }
        }
    }
    var body: some Scene {
        WindowGroup {
            VStack { Text(status); Button("Verify") {
                do { status = try probe() } catch { status = String(describing: error) }
            }}
        }
    }
}
