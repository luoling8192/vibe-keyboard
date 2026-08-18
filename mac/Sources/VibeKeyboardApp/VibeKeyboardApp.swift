import SwiftUI

@main
struct VibeKeyboardApplication: App {
    @StateObject private var model = AppModel()
    @NSApplicationDelegateAdaptor(VibeKeyboardAppDelegate.self) private var appDelegate

    var body: some Scene {
        WindowGroup(id: "main") {
            RootView(model: model)
        }
        .defaultSize(width: 1040, height: 700)
        .commands {
            CommandGroup(after: .newItem) {
                Button("Import Asset…") {
                    model.selectedPage = .screen
                }
                .keyboardShortcut("o", modifiers: [.command])
                .disabled(!model.canUploadAssets)
            }
        }

        MenuBarExtra("Vibe Keyboard", systemImage: "keyboard") {
            VibeKeyboardMenu(model: model)
        }

        Settings {
            Form {
                Text("USB Serial/JTAG is the only device transport.")
                Text("Destructive firmware, storage, and LED calibration actions require separate authorization.")
            }
            .padding(24)
            .frame(width: 520)
        }
    }
}
