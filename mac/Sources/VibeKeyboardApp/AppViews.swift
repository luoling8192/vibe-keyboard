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
    }

    @ViewBuilder private var page: some View {
        switch model.selectedPage {
        case .device: DevicePage(model: model)
        case .screen: ScreenPage(model: model)
        case .pets: PetsPage(model: model)
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

private struct DevicePreview: View {
    let mode: String
    let pixels: [UInt16]?
    let layout: ScreenLayout?

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("428×142 preview").font(.headline)
            GeometryReader { proxy in
                ZStack(alignment: .topLeading) {
                    Color.black
                    if let pixels, let image = previewImage(pixels) {
                        Image(decorative: image, scale: 1)
                            .resizable()
                            .interpolation(.none)
                            .scaledToFit()
                    } else if let layout {
                        layoutPreview(layout, size: proxy.size)
                    } else {
                        Text("No canonical preview")
                            .foregroundStyle(.white.opacity(0.65))
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                }
                .clipShape(RoundedRectangle(cornerRadius: 8))
            }
            .aspectRatio(428 / 142, contentMode: .fit)
            .accessibilityLabel("Device screen preview, mode \(mode)")
        }
        .padding()
        .background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 12))
    }

    @ViewBuilder private func layoutPreview(_ layout: ScreenLayout, size: CGSize) -> some View {
        let placements = (try? ScreenLayoutGeometry.placements(layout)) ?? []
        let sx = size.width / 428
        let sy = size.height / 142
        ZStack(alignment: .topLeading) {
            Color(red: Double((layout.backgroundRGB888 >> 16) & 0xff) / 255,
                  green: Double((layout.backgroundRGB888 >> 8) & 0xff) / 255,
                  blue: Double(layout.backgroundRGB888 & 0xff) / 255)
            ForEach(Array(placements.enumerated()), id: \.offset) { _, placement in
                RoundedRectangle(cornerRadius: 2)
                    .stroke(Color.cyan.opacity(0.8), lineWidth: 1)
                    .overlay(Text(placement.id).font(.caption2).foregroundStyle(.white))
                    .frame(width: CGFloat(placement.rect.width) * sx, height: CGFloat(placement.rect.height) * sy)
                    .offset(x: CGFloat(placement.rect.x) * sx, y: CGFloat(placement.rect.y) * sy)
            }
        }
    }

    private func previewImage(_ pixels: [UInt16]) -> CGImage? {
        guard pixels.count == 428 * 142 else { return nil }
        var rgba = Data(capacity: pixels.count * 4)
        for pixel in pixels {
            rgba.append(UInt8(((pixel >> 11) & 0x1f) * 255 / 31))
            rgba.append(UInt8(((pixel >> 5) & 0x3f) * 255 / 63))
            rgba.append(UInt8((pixel & 0x1f) * 255 / 31))
            rgba.append(255)
        }
        guard let provider = CGDataProvider(data: rgba as CFData) else { return nil }
        return CGImage(width: 428, height: 142, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: 428 * 4,
                       space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }
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
                ViewThatFits(in: .horizontal) {
                    HStack(alignment: .top, spacing: 20) { preview; connectionCard.frame(width: 330) }
                    VStack(alignment: .leading, spacing: 20) { preview; connectionCard }
                }
            }.padding(24)
        }
    }

    private var preview: some View { DevicePreview(mode: model.screenMode.rawValue.capitalized, pixels: model.previewPixels, layout: model.previewLayout) }

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
            CapabilityRow(name: "Update", value: model.updateCapability)
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

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                HStack { PageTitle(title: "Screen"); Spacer(); Button("Import…") { importerPresented = true }.disabled(!model.assetsCapability.isAvailable) }
                ViewThatFits(in: .horizontal) {
                    HStack(alignment: .top, spacing: 20) { DevicePreview(mode: model.screenMode.rawValue.capitalized, pixels: model.previewPixels, layout: model.previewLayout); inspector.frame(width: 260) }
                    VStack(alignment: .leading, spacing: 20) { DevicePreview(mode: model.screenMode.rawValue.capitalized, pixels: model.previewPixels, layout: model.previewLayout); inspector }
                }
                Text("Upload: \(model.upload.label)").foregroundStyle(.secondary)
                ProgressView(value: model.uploadProgress).opacity(model.activeTransferID == nil ? 0 : 1)
                if model.activeTransferID != nil { Button("Cancel upload", role: .cancel) { model.cancelUpload() } }
            }.padding(24)
        }
        .fileImporter(isPresented: $importerPresented, allowedContentTypes: [.image]) { result in
            if case .success(let url) = result { model.importAndUpload(url: url) }
        }
    }

    private var inspector: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Mode").font(.headline)
            Picker("Mode", selection: $model.screenMode) {
                Text("Image").tag(ScreenMode.image)
                Text("Pet").tag(ScreenMode.pet)
                Text("Dashboard").tag(ScreenMode.dashboard)
                Text("Custom").tag(ScreenMode.custom)
            }.pickerStyle(.radioGroup)
            Divider()
            Button("Query device screen") { model.queryScreen() }.disabled(!model.screenCapability.isAvailable)
            switch model.screenMode {
            case .image:
                Button("Commit uploaded image") { model.activateUploadedImage() }
                    .disabled(!model.canSendScreen || model.lastUploadedAsset?.kind != .image)
            case .pet:
                Button("Commit pet states") { model.activateUploadedPet() }
                    .disabled(!model.canSendScreen || model.lastUploadedAsset?.kind != .animation)
            case .dashboard, .custom:
                TextField("Title (printable ASCII)", text: $model.layoutTitle)
                TextField("Status (printable ASCII)", text: $model.widgetText)
                Button(model.screenMode == .dashboard ? "Commit dashboard" : "Commit custom layout") {
                    model.activateLayout(mode: model.screenMode == .dashboard ? .dashboard : .custom)
                }
                    .disabled(!model.canSendScreen)
                Button("Send widget update") { model.sendStatusWidget() }
                    .disabled(model.availableScreen?.configured != true)
                Text("Use Image mode for Unicode text or fully custom artwork.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Text(model.screenCapability.label).font(.caption).foregroundStyle(.secondary)
        }.padding().background(.quaternary.opacity(0.4), in: RoundedRectangle(cornerRadius: 12))
    }
}

private struct PetsPage: View {
    @ObservedObject var model: AppModel
    @State private var importerPresented = false
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                HStack { PageTitle(title: "Pets"); Spacer(); Button("Import GIF/APNG") { importerPresented = true }.disabled(!model.assetsCapability.isAvailable) }
                TextField("Search", text: .constant("")).disabled(true)
                VStack(spacing: 10) {
                    Image(systemName: "pawprint").font(.largeTitle).foregroundStyle(.secondary)
                    Text("No pet library yet").font(.headline)
                    Text("Import a bounded GIF or APNG to create explicit semantic states.").foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, minHeight: 140)
                GroupBox("Selected animation states") {
                    Grid(alignment: .leading) {
                        GridRow { Text("Idle"); Text("Required") }
                        GridRow { Text("Active"); Text("Idle fallback") }
                        GridRow { Text("Success"); Text("Idle fallback") }
                        GridRow { Text("Error"); Text("Idle fallback") }
                    }
                }
                Button("Commit selected pet") { model.activateUploadedPet() }
                    .disabled(!model.canSendScreen || model.lastUploadedAsset?.kind != .animation)
                Text(model.upload.label).foregroundStyle(.secondary)
            }.padding(24)
        }
        .fileImporter(isPresented: $importerPresented, allowedContentTypes: [.gif, .png]) { result in
            if case .success(let url) = result { model.importAndUpload(url: url, pet: true) }
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
    @State private var shortcutOption = false
    @State private var shortcutShift = false
    @State private var commandExecutable = "/usr/bin/true"
    @State private var commandArguments = ""
    @State private var commandTimeout = "5000"
    @State private var bundleIdentifier = "com.apple.TextEdit"

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                PageTitle(title: "Keys")
                Text("Physical order: Not calibrated").foregroundStyle(.secondary)
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
                    }
                    .padding(.vertical, 6)
                }
                HStack { Spacer(); Button("Save mapping") { model.saveMappings() } }
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
            TextField("Single shortcut key", text: $shortcutKey)
                .frame(maxWidth: 240)
            HStack {
                Toggle("Command", isOn: $shortcutCommand)
                Toggle("Control", isOn: $shortcutControl)
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
        if shortcutOption { result.insert(.option) }
        if shortcutShift { result.insert(.shift) }
        return result
    }

    private var shortcutIsValid: Bool {
        shortcutKey.unicodeScalars.count == 1 &&
        "abcdefghijklmnopqrstuvwxyz1234567890=-[];'\\,./`".contains(shortcutKey.lowercased())
    }

    private var commandIsValid: Bool {
        commandExecutable.trimmingCharacters(in: .whitespacesAndNewlines).hasPrefix("/") &&
        UInt32(commandTimeout).map { $0 > 0 && $0 <= 300_000 } == true
    }

    private func loadDrafts() {
        switch currentAction {
        case .pasteText(let text):
            textDraft = text
        case .customShortcut(let shortcut):
            shortcutKey = shortcut.key
            shortcutCommand = shortcut.modifiers.contains(.command)
            shortcutControl = shortcut.modifiers.contains(.control)
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
    case screenImage, screenPet, screenDashboard, screenCustom

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
        case .screenMode(.pet): self = .screenPet
        case .screenMode(.dashboard): self = .screenDashboard
        case .screenMode(.custom): self = .screenCustom
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
        case .screenPet: "Open pet controls"
        case .screenDashboard: "Open dashboard controls"
        case .screenCustom: "Open custom screen controls"
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
            (try? CommandSpecification(executable: "/usr/bin/true", timeoutMilliseconds: 5_000))
                .map { .customCommand($0) } ?? .none
        case .launch: .launchApplication(bundleIdentifier: "com.apple.TextEdit")
        case .screenImage: .screenMode(.image)
        case .screenPet: .screenMode(.pet)
        case .screenDashboard: .screenMode(.dashboard)
        case .screenCustom: .screenMode(.custom)
        }
    }
}

private struct AudioPage: View {
    @ObservedObject var model: AppModel
    var body: some View {
        Form {
            PageTitle(title: "Audio")
            LabeledContent("State", value: String(describing: model.audioState))
            Text("Replacement: use the configured canonical voice key. No host-start command is sent.")
            Picker("Interaction", selection: Binding(
                get: { model.interactionMode },
                set: { model.setInteractionMode($0) }
            )) {
                Text("Hold to talk").tag(InteractionMode.holdToTalk)
                Text("Click to talk").tag(InteractionMode.clickToTalk)
            }
            .disabled(!model.isConnected)
            Toggle("Save recordings to Application Support", isOn: $model.saveRecordings)
            Text("Saved files use an atomic private .ogg write in VibeKeyboard/Recordings. Raw PCM is never retained.")
                .font(.caption).foregroundStyle(.secondary)
            Text("Recognition: unavailable until a reviewed provider contract exists").foregroundStyle(.secondary)
            LabeledContent("Last recording", value: model.lastRecording)
        }.formStyle(.grouped).padding(8)
    }
}

private struct FirmwarePage: View {
    @ObservedObject var model: AppModel
    var body: some View {
        Form {
            PageTitle(title: "Firmware")
            LabeledContent("Update", value: model.updateCapability.label)
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
