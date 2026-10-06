import GraphDBKit
import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

// Retain one coordinator in your app's owner and pass it to the root view.
// UIKit background assertions start synchronously in send(.background).
@MainActor struct DatabaseLifecycleView: View {
    let lifecycle: GraphLifecycle
    @Environment(\.scenePhase) private var phase
    @State private var failure: String?

    var body: some View {
        Text(failure ?? "NovaGraph ready")
            .onChange(of: phase) { next in
                switch next {
                case .background: observe(lifecycle.send(.background))
                case .active: observe(lifecycle.send(.foreground))
                default: break
                }
            }
            #if canImport(UIKit)
            .onReceive(NotificationCenter.default.publisher(for: UIApplication.didReceiveMemoryWarningNotification)) { _ in
                observe(lifecycle.send(.memoryPressure))
            }
            #endif
    }
    private func observe(_ operation: Task<Void, Error>) {
        Task {
            do { try await operation.value }
            catch { failure = error.localizedDescription }
        }
    }
}
