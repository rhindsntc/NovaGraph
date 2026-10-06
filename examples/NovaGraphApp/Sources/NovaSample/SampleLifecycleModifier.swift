import SwiftUI
import GraphDBKit
#if os(iOS)
import UIKit
#endif

/// The same modifier used by the runnable app and compiled documentation.
@MainActor public struct SampleLifecycleModifier: ViewModifier {
    @Environment(\.scenePhase) private var phase
    private let send: @MainActor @Sendable (GraphLifecycle.Event) -> Void
    #if os(macOS)
    @State private var pressure: DispatchSourceMemoryPressure?
    #endif

    public init(send: @escaping @MainActor @Sendable (GraphLifecycle.Event) -> Void) { self.send = send }

    public func body(content: Content) -> some View {
        content.onChange(of: phase) { next in
            switch next {
            case .background: send(.background)
            case .active: send(.foreground)
            default: break
            }
        }
        #if os(iOS)
        .onReceive(NotificationCenter.default.publisher(for: UIApplication.didReceiveMemoryWarningNotification)) { _ in
            send(.memoryPressure)
        }
        #elseif os(macOS)
        .onAppear {
            let source = DispatchSource.makeMemoryPressureSource(eventMask: [.warning, .critical], queue: .main)
            source.setEventHandler { Task { @MainActor in send(.memoryPressure) } }
            pressure = source; source.resume()
        }
        .onDisappear { pressure?.cancel(); pressure = nil }
        #endif
    }
}
