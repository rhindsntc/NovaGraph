import SwiftUI
import NovaSample

struct SampleView: View {
    @ObservedObject var model: SampleModel
    @State private var tab = 0
    @State private var personID = ""
    @State private var name = ""
    @State private var from = ""
    @State private var to = ""
    @State private var query = "find nodes Person limit 100"

    private var available: Bool { model.state == .ready && !model.busy }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Label("NovaGraph", systemImage: "point.3.connected.trianglepath.dotted").font(.title2.bold())
                Spacer()
                Button("Open") { model.perform(.open) }.disabled(model.busy || !model.canOpen).accessibilityIdentifier("open")
                Button("Close") { model.perform(.close) }.disabled(model.state == .closed || model.state == .closing).accessibilityIdentifier("close")
            }
            HStack {
                Circle().fill(model.state == .ready ? Color.green : Color.secondary).frame(width: 8, height: 8)
                Text(model.status).accessibilityIdentifier("operation-status")
                Spacer()
                if model.busy {
                    ProgressView().controlSize(.small)
                    Button("Cancel") { model.cancel() }.disabled(model.state == .closing).accessibilityIdentifier("cancel")
                }
            }.font(.callout)
            if let error = model.error {
                Text(error).font(.callout).foregroundStyle(.red).textSelection(.enabled).accessibilityIdentifier("operation-error")
            }
            HStack {
                tabButton("Graph", index: 0, id: "graph-tab")
                tabButton("Query", index: 1, id: "query-tab")
                tabButton("Lifecycle", index: 2, id: "lifecycle-tab")
            }
            Divider()
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    if tab == 0 { graph }
                    else if tab == 1 { queries }
                    else { lifecycle }
                }.frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding(20)
        .frame(minWidth: 300, idealWidth: 680, minHeight: 480, idealHeight: 720)
        .onAppear { if model.state == .closed { model.perform(.open) } }
        .onDisappear { model.cancel() }
        .modifier(SampleLifecycleModifier { model.lifecycle($0) })
    }

    private func tabButton(_ title: String, index: Int, id: String) -> some View {
        Button(title) { model.cancel(); tab = index }
            .buttonStyle(.bordered).tint(tab == index ? .accentColor : .secondary)
            .accessibilityIdentifier(id)
    }

    private var graph: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("People").font(.headline)
            Text("Start with two people, then connect them. Saving an existing ID replaces its name.").font(.caption).foregroundStyle(.secondary)
            TextField("Person ID", text: $personID).textFieldStyle(.roundedBorder).accessibilityIdentifier("person-id")
            TextField("Name", text: $name).textFieldStyle(.roundedBorder).accessibilityIdentifier("person-name")
            Button("Save person") { model.perform(.save(personID, name)) }.buttonStyle(.borderedProminent).disabled(!available).accessibilityIdentifier("save-person")
            ForEach(model.nodes) { node in
                Text("\(node.id) · \(node["name"]?.stringValue ?? "—")").font(.callout).textSelection(.enabled)
            }
            Text("Showing up to 100 people").font(.caption).foregroundStyle(.secondary)
            Divider()
            Text("Connections & paths").font(.headline)
            TextField("From ID", text: $from).textFieldStyle(.roundedBorder).accessibilityIdentifier("edge-from")
            TextField("To ID", text: $to).textFieldStyle(.roundedBorder).accessibilityIdentifier("edge-to")
            HStack {
                Button("Connect") { model.perform(.connect(from, to)) }.accessibilityIdentifier("connect")
                Button("Show paths") { model.perform(.paths(from)) }.accessibilityIdentifier("show-paths")
            }.buttonStyle(.bordered).disabled(!available)
            ForEach(Array(model.paths.enumerated()), id: \.offset) { _, path in
                VStack(alignment: .leading) {
                    Text(path.nodes.map(\.id).joined(separator: " → ")).font(.headline)
                    Text(path.edges.map(\.type).joined(separator: " → ")).font(.caption).foregroundStyle(.secondary)
                }
            }
            Text("One shortest path per reachable person; depth 8, up to 100 results.").font(.caption).foregroundStyle(.secondary)
        }
    }

    private var queries: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Run NGQL").font(.headline)
            Text("Queries run against this sample's local database. Mutating statements change its data.").font(.caption).foregroundStyle(.secondary)
            TextEditor(text: $query).font(.system(.body, design: .monospaced)).frame(height: 130)
                .overlay(RoundedRectangle(cornerRadius: 6).stroke(Color.secondary.opacity(0.4)))
                .accessibilityIdentifier("query-editor")
            Button("Run query") { model.perform(.query(query)) }.buttonStyle(.borderedProminent).disabled(!available).accessibilityIdentifier("run-query")
            if let transaction = model.lastCommit { Text("Last committed transaction: \(transaction)").font(.caption).textSelection(.enabled) }
            Text(model.output.isEmpty ? "Results appear here." : model.output).font(.system(.caption, design: .monospaced)).textSelection(.enabled)
        }
    }

    private var lifecycle: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Lifecycle controls").font(.headline)
            Text("Exercise pressure and suspension on this database. Storage failures stay visible; acknowledged writes are never automatically retried.")
            Button("Trim memory") { model.perform(.trim) }.disabled(!available).accessibilityIdentifier("trim")
            Button("Suspend") { model.lifecycle(.background) }.disabled(!available).accessibilityIdentifier("suspend")
            Button("Resume") { model.lifecycle(.foreground) }.disabled(model.state != .suspended || model.busy).accessibilityIdentifier("resume")
            Text("The app also forwards scene changes and iOS memory warnings. Use Close to drain work and report checkpoint errors before leaving.").font(.caption).foregroundStyle(.secondary)
            Text("Database location").font(.headline)
            Text(model.path.path).font(.caption).textSelection(.enabled)
            Text("Experimental sample · physical-device and minimum-runtime qualification pending").font(.caption).foregroundStyle(.secondary)
        }.buttonStyle(.bordered)
    }
}
