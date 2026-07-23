import SwiftUI

@main
struct VibeKeyboardApplication: App {
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup("Vibe Keyboard") {
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
