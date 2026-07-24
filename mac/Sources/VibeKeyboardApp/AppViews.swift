import AppKit
import SwiftUI
import UniformTypeIdentifiers
import VibeBoardKit

struct RootView: View {
    @ObservedObject var model: AppModel

    var body: some View {
        NavigationSplitView {
            List(AppPage.allCases, selection: $model.selectedPage) { page in
                Label(page.rawValue, systemImage: page.symbol).tag(page)
            }
            .navigationTitle("Vibe Keyboard")
            .navigationSplitViewColumnWidth(min: 160, ideal: 185, max: 210)
        } detail: {
            VStack(spacing: 0) {
                page
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                Divider()
                StatusBar(model: model)
            }
        }
        .frame(minWidth: 900, minHeight: 620)
        .task { model.start() }
        .onReceive(NotificationCenter.default.publisher(for: NSApplication.didBecomeActiveNotification)) { _ in
            model.refreshInputPermission()
        }
    }

    @ViewBuilder private var page: some View {
        switch model.selectedPage {
        case .device: DevicePage(model: model)
        case .screen: ScreenPage(model: model)
        case .keys: KeysPage(model: model)
        case .audio: AudioPage(model: model)
        case .firmware: FirmwarePage(model: model)
        }
    }
}

private struct PageTitle: View {
    let title: String
    var body: some View { Text(title).font(.largeTitle.bold()).accessibilityAddTraits(.isHeader) }
}

private struct CapabilityRow: View {
    let name: String
    let value: CapabilityPresentation
    var body: some View {
        HStack { Text(name); Spacer(); Text(value.label).foregroundStyle(value.isAvailable ? .green : .secondary) }
            .accessibilityElement(children: .combine)
    }
}

private struct DevicePage: View {
    @ObservedObject var model: AppModel
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                PageTitle(title: "Device")
                connectionCard
            }.padding(24)
        }
    }

    private var connectionCard: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Connection").font(.headline)
            LabeledContent("Transport", value: "USB Serial/JTAG")
            LabeledContent("Status", value: model.connection.title)
            if case .ready(let info) = model.connection {
                LabeledContent("Device", value: info.registryDeviceID)
                LabeledContent("Firmware", value: info.firmwareVersion ?? "Unknown")
            }
            Divider()
            Text("Capabilities").font(.headline)
            CapabilityRow(name: "Assets", value: model.assetsCapability)
            CapabilityRow(name: "Screen", value: model.screenCapability)
            CapabilityRow(name: "LED", value: model.ledCapability)
            CapabilityRow(name: "Firmware updates", value: model.updateCapability)
            Button("Reconnect") { model.reconnect() }
                .disabled(model.isConnected)
                .keyboardShortcut("r", modifiers: [.command])
            if let message = model.diagnosticMessage {
                Text(message).font(.caption).foregroundStyle(.red).textSelection(.enabled)
            }
        }
        .padding()
        .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 12))
    }
}

private struct ScreenPage: View {
    @ObservedObject var model: AppModel
    @State private var importerPresented = false
    @State private var petImporterPresented = false
    @State private var petSheetPresented = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                HStack { PageTitle(title: "Screen"); Spacer(); Button("Import & Upload…") { importerPresented = true }.disabled(!model.canUploadAssets) }
                inspector.frame(maxWidth: 460)
                Text("Upload: \(model.upload.label)").foregroundStyle(.secondary)
                Text("Storage: \(model.assetStorageLabel)")
                    .foregroundStyle(model.canUploadAssets ? Color.secondary : Color.orange)
                ProgressView(value: model.uploadProgress).opacity(model.activeTransferID == nil ? 0 : 1)
                if model.activeTransferID != nil { Button("Cancel upload", role: .cancel) { model.cancelUpload() } }
            }.padding(24)
        }
        .fileImporter(isPresented: $importerPresented, allowedContentTypes: [.image]) { result in
            if case .success(let url) = result { model.importAndUpload(url: url) }
        }
        .fileImporter(isPresented: $petImporterPresented, allowedContentTypes: [.gif, .png]) { result in
            if case .success(let url) = result {
                model.importAndUpload(url: url, pet: true)
            }
        }
    }

    private var inspector: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Mode").font(.headline)
            Picker("Mode", selection: $model.screenMode) {
                Text("Image").tag(ScreenMode.image)
                Text("Dashboard").tag(ScreenMode.dashboard)
            }.pickerStyle(.radioGroup)
            Divider()
            Button("Query device screen") { model.queryScreen() }.disabled(!model.screenCapability.isAvailable)
            switch model.screenMode {
            case .image:
                Button("Commit uploaded image") { model.activateUploadedImage() }
                    .disabled(!model.canSendScreen || model.lastUploadedAsset?.kind != .image)
            case .pet, .dashboard, .custom:
                Text("Two tiles are shown at once. Add as many rotating pages as you need.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                ForEach(model.dashboardPages, id: \.index) { page in
                    GroupBox("Page \(page.index + 1)") {
                        VStack(alignment: .leading, spacing: 8) {
                            dashboardModulePicker(
                                "Left",
                                index: page.index * 2
                            )
                            dashboardModulePicker(
                                "Right",
                                index: page.index * 2 + 1
                            )
                            dashboardPagePreview(page: page)
                            if model.dashboardPages.count > 1 {
                                Button("Remove page", role: .destructive) {
                                    model.removeDashboardPage(at: page.index)
                                }
                                .buttonStyle(.link)
                            }
                        }
                    }
                }
                Button("Add page") { model.addDashboardPage() }
                Picker("Page rotation", selection: $model.dashboardPageDurationSeconds) {
                    ForEach([4, 6, 8, 10, 12], id: \.self) { seconds in
                        Text("\(seconds) seconds").tag(seconds)
                    }
                }
                TextField(
                    "Stocks, up to 12 (sh000001,hk00700,usAAPL)",
                    text: $model.stockSymbols
                )
                dashboardPetPicker
                HStack {
                    Button("Save") { model.saveDashboardSettings() }
                    Button(model.liveDashboardEnabled ? "Reinstall" : "Install & start") {
                        model.saveDashboardSettings()
                        model.activateLiveDashboard()
                    }
                    .disabled(
                        !model.canSendScreen ||
                        (
                            model.dashboardModules.contains(.pet) &&
                            model.lastUploadedAsset?.kind != .animation
                        )
                    )
                }
                if model.liveDashboardEnabled {
                    Button("Pause live updates") { model.stopLiveDashboard() }
                }
                Text("Stock tiles show up to four quotes and advance every 4 seconds.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text("Pet is a regular page module and can be placed in any tile.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text("Codex quota comes from the installed Codex CLI. Claude daily tokens come from local session logs. Credentials are not copied to the device.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Text(model.screenCapability.label).font(.caption).foregroundStyle(.secondary)
        }.padding().background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 12))
    }

    private var dashboardPetPicker: some View {
        GroupBox("Dashboard Pet") {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Button("Choose pet…") { petSheetPresented = true }
                        .disabled(model.pets.isEmpty)
                    Spacer()
                    Button("Refresh") { model.refreshPetdex() }
                }
                if let pet = model.selectedPet {
                    HStack(spacing: 6) {
                        Image(systemName: model.uploadedPetID == pet.id
                            ? "checkmark.circle.fill"
                            : "pawprint.fill")
                            .foregroundStyle(model.uploadedPetID == pet.id ? .green : .secondary)
                        Text(pet.displayName).font(.callout.weight(.semibold))
                    }
                    Text(pet.attribution)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                } else {
                    Text("No pet selected")
                        .foregroundStyle(.secondary)
                        .font(.callout)
                }
                Picker("Animation", selection: $model.petAnimationChoice) {
                    ForEach(PetAnimationChoice.allCases) { choice in
                        Text(choice.label).tag(choice)
                    }
                }
                Button("Upload selected") { model.downloadSelectedPet() }
                    .disabled(model.selectedPet == nil || !model.canUploadAssets)
                Button("Import GIF/APNG…") { petImporterPresented = true }
                    .disabled(!model.canUploadAssets)
                Text(model.petCatalogStatus)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .sheet(isPresented: $petSheetPresented) {
            PetPickerSheet(model: model)
        }
    }

    @ViewBuilder private func dashboardModulePicker(
        _ label: String,
        index: Int
    ) -> some View {
        Picker(
            label,
            selection: Binding(
                get: {
                    LiveDashboardSnapshot
                        .normalizedModules(model.dashboardModules)[index]
                },
                set: { model.setDashboardModule($0, at: index) }
            )
        ) {
            ForEach(DashboardModule.allCases) { module in
                Text(module.label).tag(module)
            }
        }
    }

    private func dashboardPagePreview(page: DashboardPageContent) -> some View {
        HStack(alignment: .top, spacing: 10) {
            dashboardTilePreview(page.left)
            Divider()
            dashboardTilePreview(page.right)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    @ViewBuilder private func dashboardTilePreview(_ tile: DashboardTileContent) -> some View {
        if tile.module == .pet {
            Text("PET")
                .font(.system(.caption2, design: .monospaced))
                .frame(maxWidth: .infinity, alignment: .leading)
        } else {
        VStack(alignment: .leading, spacing: 3) {
            Text(tile.title).fontWeight(.semibold)
                ForEach(Array(tile.lines.prefix(4).enumerated()), id: \.offset) { _, line in
                    Text(line).foregroundStyle(.secondary)
                }
        }
        .font(.system(.caption2, design: .monospaced))
        .lineLimit(1)
        .minimumScaleFactor(0.65)
        .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

private struct KeysPage: View {
    @ObservedObject var model: AppModel
    @State private var selectedGesture: KeyGesture = .single
    @State private var textDraft = "Text"
    @State private var shortcutKey = "k"
    @State private var shortcutCommand = true
    @State private var shortcutControl = false
    @State private var shortcutFunction = false
    @State private var shortcutOption = false
    @State private var shortcutShift = false
    @State private var commandExecutable = "/usr/bin/open"
    @State private var commandArguments = "-a\nTextEdit"
    @State private var commandTimeout = "5000"
    @State private var bundleIdentifier = "com.apple.TextEdit"

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                PageTitle(title: "Keys")
                Text("Physical order: Not calibrated").foregroundStyle(.secondary)
                GroupBox("Shortcut permission") {
                    HStack {
                        Label(
                            model.inputPermissionGranted ? "Accessibility access granted" : "Accessibility access required",
                            systemImage: model.inputPermissionGranted ? "checkmark.circle.fill" : "exclamationmark.triangle.fill"
                        )
                        .foregroundStyle(model.inputPermissionGranted ? .green : .orange)
                        Spacer()
                        if !model.inputPermissionGranted {
                            Button("Open Accessibility Settings") { model.requestInputPermission() }
                        }
                    }
                    .padding(.vertical, 4)
                }
                HStack(spacing: 20) {
                    ForEach(CanonicalKey.allCases, id: \.self) { key in
                        Button(key.rawValue) { model.selectedKey = key }
                            .buttonStyle(.borderedProminent)
                            .tint(model.highlightedKey == key ? .orange : (model.selectedKey == key ? .accentColor : .gray))
                            .accessibilityLabel("Canonical key \(key.rawValue)")
                    }
                }
                GroupBox("Selected: \(model.selectedKey.rawValue)") {
                    VStack(alignment: .leading, spacing: 12) {
                        Picker("Gesture", selection: $selectedGesture) {
                            Text("Click").tag(KeyGesture.single)
                            Text("Double click").tag(KeyGesture.double)
                            Text("Long press").tag(KeyGesture.long)
                        }
                        .pickerStyle(.segmented)

                        Picker("Action", selection: actionChoice) {
                            ForEach(ActionChoice.allCases, id: \.self) { choice in
                                Text(choice.label).tag(choice)
                            }
                        }
                        .frame(maxWidth: 360)

                        actionParameters
                        Divider()
                        Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 6) {
                            ForEach(KeyGesture.allCases, id: \.self) { gesture in
                                GridRow {
                                    Text(gestureLabel(gesture)).foregroundStyle(.secondary)
                                    Text(ActionChoice(binding(model.selectedKey)[gesture]).label)
                                }
                            }
                        }
                    }
                    .padding(.vertical, 6)
                }
                HStack {
                    Button("Test selected action") { model.testAction(currentAction) }
                    Button("Clear selected gesture") {
                        model.setAction(.none, for: model.selectedKey, gesture: selectedGesture)
                    }
                    Spacer()
                    Button("Save mapping") { model.saveMappings() }
                }
                if let result = model.lastActionResult {
                    LabeledContent("Last action", value: result)
                }
                if let message = model.diagnosticMessage {
                    Text(message).font(.caption).foregroundStyle(.red).textSelection(.enabled)
                }
            }.padding(24)
        }
        .onAppear { loadDrafts() }
        .onChange(of: model.selectedKey) { _ in loadDrafts() }
        .onChange(of: selectedGesture) { _ in loadDrafts() }
    }

    private func binding(_ key: CanonicalKey) -> KeyBindings { model.keyProfile.mappings[key] ?? KeyBindings() }
    private var currentAction: HostAction { binding(model.selectedKey)[selectedGesture] }
    private var actionChoice: Binding<ActionChoice> {
        Binding(
            get: { ActionChoice(currentAction) },
            set: {
                model.setAction($0.defaultAction, for: model.selectedKey, gesture: selectedGesture)
                loadDrafts()
            }
        )
    }

    @ViewBuilder private var actionParameters: some View {
        switch currentAction {
        case .pasteText:
            TextField("Text to paste", text: $textDraft)
            Button("Apply text") {
                model.setAction(.pasteText(textDraft), for: model.selectedKey, gesture: selectedGesture)
            }
            .disabled(textDraft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
        case .customShortcut:
            TextField("Key (1, fn, f1…f20, home, pageup)", text: $shortcutKey)
                .frame(maxWidth: 240)
            HStack {
                Toggle("Command", isOn: $shortcutCommand)
                Toggle("Control", isOn: $shortcutControl)
                Toggle("Fn", isOn: $shortcutFunction)
                Toggle("Option", isOn: $shortcutOption)
                Toggle("Shift", isOn: $shortcutShift)
            }
            Button("Apply shortcut") {
                guard let shortcut = try? VibeBoardKit.KeyboardShortcut(
                    modifiers: shortcutModifiers,
                    key: shortcutKey
                ) else { return }
                model.setAction(.customShortcut(shortcut), for: model.selectedKey, gesture: selectedGesture)
            }
            .disabled(!shortcutIsValid)
            Text(shortcutPreview)
                .font(.caption.monospaced())
                .foregroundStyle(.secondary)
        case .customCommand:
            TextField("Absolute executable path", text: $commandExecutable)
            TextEditor(text: $commandArguments)
                .frame(height: 64)
                .overlay(alignment: .topLeading) {
                    if commandArguments.isEmpty {
                        Text("Arguments, one per line").foregroundStyle(.tertiary).padding(6)
                    }
                }
            TextField("Timeout in milliseconds", text: $commandTimeout)
                .frame(maxWidth: 240)
            Text("The executable runs directly without a shell. Enter each literal argument on its own line.")
                .font(.caption)
                .foregroundStyle(.secondary)
            Button("Apply command") {
                guard let timeout = UInt32(commandTimeout),
                      let command = try? CommandSpecification(
                        executable: commandExecutable,
                        arguments: commandArguments.components(separatedBy: .newlines).filter { !$0.isEmpty },
                        timeoutMilliseconds: timeout
                      ) else { return }
                model.setAction(.customCommand(command), for: model.selectedKey, gesture: selectedGesture)
            }
            .disabled(!commandIsValid)
        case .launchApplication:
            TextField("Application bundle identifier", text: $bundleIdentifier)
            Button("Apply application") {
                model.setAction(
                    .launchApplication(bundleIdentifier: bundleIdentifier),
                    for: model.selectedKey,
                    gesture: selectedGesture
                )
            }
            .disabled(bundleIdentifier.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
        default:
            EmptyView()
        }
    }

    private var shortcutModifiers: Set<VibeBoardKit.KeyboardShortcut.Modifier> {
        var result: Set<VibeBoardKit.KeyboardShortcut.Modifier> = []
        if shortcutCommand { result.insert(.command) }
        if shortcutControl { result.insert(.control) }
        if shortcutFunction { result.insert(.function) }
        if shortcutOption { result.insert(.option) }
        if shortcutShift { result.insert(.shift) }
        return result
    }

    private var shortcutIsValid: Bool {
        ProductionHostActionAdapter.keyCode(forShortcutKey: shortcutKey) != nil
    }

    private var commandIsValid: Bool {
        commandExecutable.trimmingCharacters(in: .whitespacesAndNewlines).hasPrefix("/") &&
        UInt32(commandTimeout).map { $0 > 0 && $0 <= 300_000 } == true
    }

    private var shortcutPreview: String {
        let names = shortcutModifiers.sorted().map {
            switch $0 {
            case .command: "Command"
            case .control: "Control"
            case .function: "Fn"
            case .option: "Option"
            case .shift: "Shift"
            }
        }
        return (names + [shortcutKey.lowercased()]).joined(separator: " + ")
    }

    private func gestureLabel(_ gesture: KeyGesture) -> String {
        switch gesture {
        case .single: "Click"
        case .double: "Double click"
        case .long: "Long press"
        }
    }

    private func loadDrafts() {
        switch currentAction {
        case .pasteText(let text):
            textDraft = text
        case .customShortcut(let shortcut):
            shortcutKey = shortcut.key
            shortcutCommand = shortcut.modifiers.contains(.command)
            shortcutControl = shortcut.modifiers.contains(.control)
            shortcutFunction = shortcut.modifiers.contains(.function)
            shortcutOption = shortcut.modifiers.contains(.option)
            shortcutShift = shortcut.modifiers.contains(.shift)
        case .customCommand(let command):
            commandExecutable = command.executable
            commandArguments = command.arguments.joined(separator: "\n")
            commandTimeout = String(command.timeoutMilliseconds)
        case .launchApplication(let identifier):
            bundleIdentifier = identifier
        default:
            break
        }
    }
}

private enum ActionChoice: String, CaseIterable, Hashable {
    case none, voice, enter, copy, interrupt, wake, paste, shortcut, command, launch
    case screenImage, screenDashboard, dashboardNextPage, dashboardNextStocks

    init(_ action: HostAction) {
        switch action {
        case .voiceInput: self = .voice
        case .sendEnter: self = .enter
        case .systemCopy: self = .copy
        case .interruptControlC: self = .interrupt
        case .wakeApplication: self = .wake
        case .pasteText: self = .paste
        case .customShortcut: self = .shortcut
        case .customCommand: self = .command
        case .launchApplication: self = .launch
        case .screenMode(.image): self = .screenImage
        case .screenMode(.dashboard): self = .screenDashboard
        case .screenMode(.pet), .screenMode(.custom): self = .screenDashboard
        case .dashboardNextPage: self = .dashboardNextPage
        case .dashboardNextStocks: self = .dashboardNextStocks
        default: self = .none
        }
    }

    var label: String {
        switch self {
        case .none: "None"
        case .voice: "Voice input"
        case .enter: "Send Enter"
        case .copy: "Copy"
        case .interrupt: "Interrupt"
        case .wake: "Wake application"
        case .paste: "Paste text"
        case .shortcut: "Custom shortcut"
        case .command: "Run command"
        case .launch: "Launch application"
        case .screenImage: "Open image controls"
        case .screenDashboard: "Open dashboard controls"
        case .dashboardNextPage: "Next dashboard page"
        case .dashboardNextStocks: "Next stock page"
        }
    }

    var defaultAction: HostAction {
        switch self {
        case .none: .none
        case .voice: .voiceInput
        case .enter: .sendEnter
        case .copy: .systemCopy
        case .interrupt: .interruptControlC
        case .wake: .wakeApplication
        case .paste: .pasteText("Text")
        case .shortcut:
            (try? VibeBoardKit.KeyboardShortcut(modifiers: [.command], key: "k"))
                .map { .customShortcut($0) } ?? .none
        case .command:
            (try? CommandSpecification(executable: "/usr/bin/open", arguments: ["-a", "TextEdit"], timeoutMilliseconds: 5_000))
                .map { .customCommand($0) } ?? .none
        case .launch: .launchApplication(bundleIdentifier: "com.apple.TextEdit")
        case .screenImage: .screenMode(.image)
        case .screenDashboard: .screenMode(.dashboard)
        case .dashboardNextPage: .dashboardNextPage
        case .dashboardNextStocks: .dashboardNextStocks
        }
    }
}

private struct AudioPage: View {
    @ObservedObject var model: AppModel
    @State private var hotkeyKey = "d"
    @State private var hotkeyCommand = true
    @State private var hotkeyControl = false
    @State private var hotkeyFunction = false
    @State private var hotkeyOption = false
    @State private var hotkeyShift = false

    var body: some View {
        Form {
            PageTitle(title: "Audio")
            LabeledContent("State", value: stateLabel)
            Picker("Voice key", selection: Binding<CanonicalKey?>(
                get: { model.configuredVoiceKey },
                set: { model.setVoiceKey($0) }
            )) {
                Text("Disabled").tag(Optional<CanonicalKey>.none)
                ForEach(CanonicalKey.allCases, id: \.self) { key in
                    Text(key.rawValue.uppercased()).tag(Optional(key))
                }
            }
            Text("The selected key's single-click action becomes Voice input. The device starts and stops capture; the Mac only receives Opus audio over USB.")
                .font(.caption).foregroundStyle(.secondary)
            Picker("Interaction", selection: Binding(
                get: { model.interactionMode },
                set: { model.setInteractionMode($0) }
            )) {
                Text("Hold to talk").tag(InteractionMode.holdToTalk)
                Text("Click to talk").tag(InteractionMode.clickToTalk)
            }
            .disabled(!model.isConnected)

            Divider()
            Toggle("Save recordings to Application Support", isOn: $model.saveRecordings)
            Text("Saved files use an atomic private .ogg write in VibeKeyboard/Recordings. Raw PCM is never retained.")
                .font(.caption).foregroundStyle(.secondary)

            Divider()
            Section {
                Picker("Voice input mode", selection: Binding(
                    get: { model.voiceInputMode },
                    set: {
                        model.voiceInputMode = $0
                        model.saveVoiceInputSettings()
                    }
                )) {
                    ForEach(VoiceInputMode.allCases) { mode in
                        Text(mode.label).tag(mode)
                    }
                }

                if model.voiceInputMode == .blackhole {
                    blackholeSection
                }
            } header: {
                Text("Voice input routing")
            }

            LabeledContent("Last recording", value: model.lastRecording)
            if let message = model.diagnosticMessage {
                Text(message).font(.caption).foregroundStyle(.red).textSelection(.enabled)
            }
        }.formStyle(.grouped).padding(8)
        .onAppear { loadHotkeyDraft() }
    }

    @ViewBuilder private var blackholeSection: some View {
        // Device status
        HStack {
            Label(
                model.blackholeAvailable ? "BlackHole detected" : "BlackHole not found",
                systemImage: model.blackholeAvailable ? "checkmark.circle.fill" : "exclamationmark.triangle.fill"
            )
            .foregroundStyle(model.blackholeAvailable ? .green : .orange)
            Spacer()
            Button("Refresh") { model.refreshBlackHoleAvailability() }
            Button("Download BlackHole") {
                if let url = URL(string: "https://github.com/ExistentialAudio/BlackHole") {
                    NSWorkspace.shared.open(url)
                }
            }
        }
        if !model.blackholeAvailable {
            Text("Install BlackHole, then set it as the input device in your dictation app (Typeless, Vokie, etc.).")
                .font(.caption).foregroundStyle(.secondary)
        }

        // Trigger hotkey
        Divider()
        Text("Trigger hotkey")
            .font(.headline)
        Text("Set this to match the global hotkey in your dictation app. Pressing the voice key will simulate this shortcut.")
            .font(.caption).foregroundStyle(.secondary)
        TextField("Key (1, fn, f1…f20, home, pageup)", text: $hotkeyKey)
            .frame(maxWidth: 240)
        HStack {
            Toggle("Command", isOn: $hotkeyCommand)
            Toggle("Control", isOn: $hotkeyControl)
            Toggle("Fn", isOn: $hotkeyFunction)
            Toggle("Option", isOn: $hotkeyOption)
            Toggle("Shift", isOn: $hotkeyShift)
        }
        HStack {
            Button("Apply hotkey") { applyHotkey() }
                .disabled(!hotkeyIsValid)
            Spacer()
            Button("Clear hotkey") {
                model.setVoiceTriggerHotkey(nil)
                loadHotkeyDraft()
            }
        }
        Text(hotkeyPreview)
            .font(.caption.monospaced())
            .foregroundStyle(.secondary)

        // Setup instructions
        Divider()
        VStack(alignment: .leading, spacing: 4) {
            Text("Setup").font(.caption.weight(.semibold))
            Text("1. Install BlackHole (link above)")
            Text("2. In your dictation app, set input device to BlackHole")
            Text("3. In your dictation app, note the global hotkey")
            Text("4. Set the same hotkey here")
            Text("5. Select a voice key above and press it")
        }
        .font(.caption).foregroundStyle(.secondary)
    }

    private var stateLabel: String {
        switch model.audioState {
        case .ready:
            return model.configuredVoiceKey == nil ? "Disabled — select a voice key" : "Ready"
        case .recording:
            return "Recording"
        case .finalizing:
            return "Finalizing"
        case .completed(_, let packetCount):
            return "Completed — \(packetCount) packets"
        case .cancelled:
            return "Cancelled"
        case .failed(let error):
            return "Failed — \(String(describing: error))"
        }
    }

    // MARK: - Hotkey helpers

    private var hotkeyModifiers: Set<VibeBoardKit.KeyboardShortcut.Modifier> {
        var mods: Set<VibeBoardKit.KeyboardShortcut.Modifier> = []
        if hotkeyCommand { mods.insert(.command) }
        if hotkeyControl { mods.insert(.control) }
        if hotkeyFunction { mods.insert(.function) }
        if hotkeyOption { mods.insert(.option) }
        if hotkeyShift { mods.insert(.shift) }
        return mods
    }

    private var hotkeyIsValid: Bool {
        !hotkeyKey.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    private var hotkeyPreview: String {
        let parts = hotkeyModifiers.sorted().map { $0.rawValue.capitalized }
        return (parts + [hotkeyKey.uppercased()]).joined(separator: " + ")
    }

    private func applyHotkey() {
        guard let shortcut = try? VibeBoardKit.KeyboardShortcut(
            modifiers: hotkeyModifiers,
            key: hotkeyKey
        ) else { return }
        model.setVoiceTriggerHotkey(shortcut)
    }

    private func loadHotkeyDraft() {
        guard let shortcut = model.voiceTriggerHotkey else { return }
        hotkeyKey = shortcut.key
        hotkeyCommand = shortcut.modifiers.contains(.command)
        hotkeyControl = shortcut.modifiers.contains(.control)
        hotkeyFunction = shortcut.modifiers.contains(.function)
        hotkeyOption = shortcut.modifiers.contains(.option)
        hotkeyShift = shortcut.modifiers.contains(.shift)
    }
}

private struct FirmwarePage: View {
    @ObservedObject var model: AppModel
    var body: some View {
        Form {
            PageTitle(title: "Firmware")
            LabeledContent("In-app update", value: model.updateCapability.label)
            Text("Firmware updates use the ROM flash tool. Keys, screen uploads, and audio remain available.")
                .font(.caption).foregroundStyle(.secondary)
            LabeledContent("Local backup evidence", value: "Not verified in this session")
            LabeledContent("Candidate image", value: "Not selected")
            HStack {
                Button("Select image…") {}.disabled(true)
                Button("Validate") {}.disabled(true)
                Button("Stage…") {}.disabled(!model.canMutateFirmware)
                Button("Activate…") {}.disabled(true)
            }
            Text("Stage never activates. Activation requires separate evidence, authorization, and confirmation.")
                .font(.caption).foregroundStyle(.secondary)
        }.formStyle(.grouped).padding(8)
    }
}

private struct StatusBar: View {
    @ObservedObject var model: AppModel
    var body: some View {
        HStack {
            Text(status)
            Spacer()
            if case .recording = model.audioState { Label("Recording", systemImage: "record.circle.fill").foregroundStyle(.red) }
        }
        .font(.caption)
        .padding(.horizontal, 12).padding(.vertical, 7)
        .accessibilityElement(children: .combine)
    }
    private var status: String {
        if case .ready(let info) = model.connection {
            return "VB-\(info.registryDeviceID) · USB · \(info.firmwareVersion ?? "firmware unknown")"
        }
        return "VibeBoard · USB · \(model.connection.title)"
    }
}
