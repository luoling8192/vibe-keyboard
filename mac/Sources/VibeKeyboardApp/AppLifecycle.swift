import AppKit
import ServiceManagement
import SwiftUI

final class VibeKeyboardAppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        false
    }
}

struct VibeKeyboardMenu: View {
    @ObservedObject var model: AppModel
    @Environment(\.openWindow) private var openWindow
    @State private var launchAtLoginEnabled: Bool
    @State private var loginItemMessage: String?

    init(model: AppModel) {
        self.model = model
        _launchAtLoginEnabled = State(initialValue: SMAppService.mainApp.status == .enabled)
    }

    var body: some View {
        Button("Show Vibe Keyboard") {
            showMainWindow()
        }

        Divider()

        Label(model.connection.title, systemImage: connectionSymbol)
            .disabled(true)

        Toggle("Launch at login", isOn: Binding(
            get: { launchAtLoginEnabled },
            set: updateLaunchAtLogin
        ))

        if let loginItemMessage {
            Text(loginItemMessage)
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 260, alignment: .leading)
        }

        Divider()

        Button("Quit Vibe Keyboard") {
            NSApp.terminate(nil)
        }
        .keyboardShortcut("q")
        .onAppear {
            launchAtLoginEnabled = SMAppService.mainApp.status == .enabled
        }
    }

    private var connectionSymbol: String {
        switch model.connection {
        case .ready:
            "checkmark.circle.fill"
        case .connecting:
            "arrow.triangle.2.circlepath"
        case .disconnected:
            "circle.dashed"
        case .incompatible, .failed:
            "exclamationmark.triangle.fill"
        }
    }

    private func showMainWindow() {
        if let window = NSApp.windows.first(where: { $0.title == "Vibe Keyboard" }) {
            window.makeKeyAndOrderFront(nil)
        } else {
            openWindow(id: "main")
        }
        NSApp.activate(ignoringOtherApps: true)
    }

    private func updateLaunchAtLogin(_ enabled: Bool) {
        do {
            if enabled {
                try SMAppService.mainApp.register()
            } else {
                try SMAppService.mainApp.unregister()
            }
            launchAtLoginEnabled = SMAppService.mainApp.status == .enabled
            loginItemMessage = nil
        } catch {
            launchAtLoginEnabled = SMAppService.mainApp.status == .enabled
            loginItemMessage = "Could not update login item: \(error.localizedDescription)"
        }
    }
}
