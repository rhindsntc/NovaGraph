import SwiftUI
import NovaSample

@main struct NovaGraphApp: App {
    @StateObject private var model = SampleModel(path: storageLocation())
    #if os(macOS)
    @Environment(\.openWindow) private var openWindow
    @NSApplicationDelegateAdaptor(SampleAppDelegate.self) private var delegate
    #endif
    var body: some Scene {
        #if os(macOS)
        delegate.model = model
        delegate.showWindow = { openWindow(id: "sample") }
        return Window("NovaGraph Sample", id: "sample") { SampleView(model: model) }
            .defaultSize(width: 680, height: 760)
        #else
        return WindowGroup { SampleView(model: model) }
        #endif
    }
    static func storageLocation() -> URL {
        #if DEBUG
        // Development override for isolated acceptance runs. Release builds ignore it.
        let args = ProcessInfo.processInfo.arguments
        if let index = args.firstIndex(of: "--database"), args.indices.contains(index + 1) {
            return URL(fileURLWithPath: args[index + 1])
        }
        #endif
        return URL.applicationSupportDirectory.appendingPathComponent("NovaGraphSample/database", isDirectory: true)
    }
}

#if os(macOS)
@MainActor final class SampleAppDelegate: NSObject, NSApplicationDelegate {
    var model: SampleModel?
    var showWindow: (() -> Void)?
    func applicationDidFinishLaunching(_ notification: Notification) {
        Task {
            await Task.yield()
            if !NSApp.windows.contains(where: { $0.isVisible }) { showWindow?() }
        }
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let model else { return .terminateNow }
        Task {
            await model.perform(.close).value
            let closed = model.state == .closed
            sender.reply(toApplicationShouldTerminate: closed)
            if !closed && !sender.windows.contains(where: { $0.isVisible }) {
                showWindow?()
                sender.activate(ignoringOtherApps: true)
            }
        }
        return .terminateLater
    }
}
#endif
