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

private struct DevicePreview: View {
    private struct PreviewLabel {
        let text: String
        let colorRGB888: UInt32
    }

    private struct PreviewGauge {
        let percent: Int
        let backgroundRGB888: UInt32
    }

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
        let labels = previewLabels(layout)
        let gauges = previewGauges(layout)
        let sx = size.width / 428
        let sy = size.height / 142
        ZStack(alignment: .topLeading) {
            color(layout.backgroundRGB888)
            ForEach(Array(placements.enumerated()), id: \.offset) { _, placement in
                if let gauge = gauges[placement.id] {
                    ZStack {
                        Circle()
                            .stroke(
                                color(gauge.backgroundRGB888),
                                lineWidth: max(3, 7 * sy)
                            )
                        Circle()
                            .trim(from: 0, to: CGFloat(gauge.percent) / 100)
                            .stroke(
                                gaugeColor(gauge.percent),
                                style: StrokeStyle(
                                    lineWidth: max(3, 7 * sy),
                                    lineCap: .round
                                )
                            )
                            .rotationEffect(.degrees(-90))
                        Text("\(gauge.percent)%")
                            .font(.system(size: max(7, 12 * sy), design: .monospaced))
                            .foregroundStyle(.white)
                    }
                    .frame(
                        width: CGFloat(placement.rect.width) * sx,
                        height: CGFloat(placement.rect.height) * sy
                    )
                    .offset(
                        x: CGFloat(placement.rect.x) * sx,
                        y: CGFloat(placement.rect.y) * sy
                    )
                } else if let label = labels[placement.id] {
                    Text(label.text)
                        .font(.system(
                            size: max(7, 13 * sy),
                            weight: placement.id.contains("title") ? .semibold : .regular,
                            design: .monospaced
                        ))
                        .foregroundStyle(color(label.colorRGB888))
                        .lineLimit(label.text.contains("\n") ? 4 : 1)
                        .minimumScaleFactor(0.5)
                        .frame(
                            width: CGFloat(placement.rect.width) * sx,
                            height: CGFloat(placement.rect.height) * sy,
                            alignment: .topLeading
                        )
                        .offset(
                            x: CGFloat(placement.rect.x) * sx,
                            y: CGFloat(placement.rect.y) * sy
                        )
                } else {
                    RoundedRectangle(cornerRadius: 2)
                        .stroke(Color.white.opacity(0.35), lineWidth: 1)
                        .overlay(
                            Text(placement.id)
                                .font(.caption2)
                                .foregroundStyle(.white.opacity(0.7))
                        )
                        .frame(
                            width: CGFloat(placement.rect.width) * sx,
                            height: CGFloat(placement.rect.height) * sy
                        )
                        .offset(
                            x: CGFloat(placement.rect.x) * sx,
                            y: CGFloat(placement.rect.y) * sy
                        )
                }
            }
        }
    }

    private func previewLabels(_ layout: ScreenLayout) -> [String: PreviewLabel] {
        var widgetFallbacks: [String: String] = [:]
        for declaration in layout.widgets {
            let widgetID: String
            switch declaration {
            case .text(let id, _, _), .integer(let id, _, _),
                 .number(let id, _, _, _, _, _), .progress(let id, _, _, _, _, _):
                widgetID = id
            }
            if let fallback = try? WidgetPreviewFormatter.fallback(declaration) {
                widgetFallbacks[widgetID] = fallback
            }
        }

        var labels: [String: PreviewLabel] = [:]
        func collect(_ node: ScreenObjectNode) {
            switch node {
            case .staticLabel(let base, _, let color, _, let text):
                if base.visible {
                    labels[base.id] = .init(text: text, colorRGB888: color)
                }
            case .dynamicLabel(let base, _, let color, _, let widgetID):
                if base.visible, let text = widgetFallbacks[widgetID] {
                    labels[base.id] = .init(text: text, colorRGB888: color)
                }
            case .iconText(let base, let color, _, _, _, let widgetID):
                if let text = widgetFallbacks[widgetID] {
                    labels[base.id] = .init(text: text, colorRGB888: color)
                }
            case .pet(let base, _, _, _):
                if base.visible {
                    labels[base.id] = .init(text: "PET", colorRGB888: 0x6ED0FF)
                }
            case .container(_, _, _, _, _, let children):
                children.forEach(collect)
            case .image, .glyphLabel, .progress:
                break
            }
        }
        layout.objects.forEach { collect($0.node) }
        return labels
    }

    private func previewGauges(_ layout: ScreenLayout) -> [String: PreviewGauge] {
        var widgetFallbacks: [String: Int] = [:]
        for declaration in layout.widgets {
            guard case .progress(let id, _, _, _, _, _) = declaration,
                  let fallback = try? WidgetPreviewFormatter.fallback(declaration),
                  let percent = Int(fallback) else {
                continue
            }
            widgetFallbacks[id] = min(max(percent, 0), 100)
        }

        var gauges: [String: PreviewGauge] = [:]
        func collect(_ node: ScreenObjectNode) {
            switch node {
            case .progress(let base, let background, _, let widgetID):
                if base.visible, let percent = widgetFallbacks[widgetID] {
                    gauges[base.id] = .init(
                        percent: percent,
                        backgroundRGB888: background
                    )
                }
            case .container(_, _, _, _, _, let children):
                children.forEach(collect)
            default:
                break
            }
        }
        layout.objects.forEach { collect($0.node) }
        return gauges
    }

    private func gaugeColor(_ percent: Int) -> Color {
        let colors: [UInt32] = [
            0x32D74B, 0x73C944, 0xA8C83A, 0xD7B83B,
            0xF39A3D, 0xFF6B3D, 0xFF453A,
        ]
        return color(colors[min(max(percent, 0) / 15, colors.count - 1)])
    }

    private func color(_ rgb: UInt32) -> Color {
        Color(
            red: Double((rgb >> 16) & 0xff) / 255,
            green: Double((rgb >> 8) & 0xff) / 255,
            blue: Double(rgb & 0xff) / 255
        )
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
                ViewThatFits(in: .horizontal) {
                    HStack(alignment: .top, spacing: 20) { DevicePreview(mode: model.screenMode.rawValue.capitalized, pixels: model.previewPixels, layout: model.previewLayout); inspector.frame(width: 260) }
                    VStack(alignment: .leading, spacing: 20) { DevicePreview(mode: model.screenMode.rawValue.capitalized, pixels: model.previewPixels, layout: model.previewLayout); inspector }
                }
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
    @State private var heldKey = "right_command"
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
                                Text(choice.label)
                                    .tag(choice)
                                    .disabled(choice == .holdKey && selectedGesture != .single)
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
                        .disabled(currentAction.isHeldKey)
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
        case .holdKey:
            TextField("Key (right_command, f2, space, a…)", text: $heldKey)
                .frame(maxWidth: 320)
            Button("Apply held key") {
                model.setAction(.holdKey(heldKey), for: model.selectedKey, gesture: selectedGesture)
            }
            .disabled(!heldKeyIsValid)
            Text("The selected key is held while the physical button is down. Use left_command, right_command, left_shift, right_shift, left_option, right_option, left_control, right_control, fn, a–z, 0–9, F1–F20, space, arrows, or return.")
                .font(.caption)
                .foregroundStyle(.secondary)
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

    private var heldKeyIsValid: Bool {
        ProductionHostActionAdapter.supportsHeldKey(heldKey)
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
        case .holdKey(let key):
            heldKey = key
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
    case holdKey, screenImage, screenDashboard, dashboardNextPage, dashboardNextStocks

    init(_ action: HostAction) {
        switch action {
        case .voiceInput: self = .voice
        case .sendEnter: self = .enter
        case .systemCopy: self = .copy
        case .interruptControlC: self = .interrupt
        case .wakeApplication: self = .wake
        case .pasteText: self = .paste
        case .holdKey: self = .holdKey
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
        case .holdKey: "Hold single key"
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
        case .holdKey: .holdKey("right_command")
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

private extension HostAction {
    var isHeldKey: Bool {
        if case .holdKey = self { return true }
        return false
    }
}

private struct AudioPage: View {
    @ObservedObject var model: AppModel
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
            Toggle("Save recordings to Application Support", isOn: $model.saveRecordings)
            Text("Saved files use an atomic private .ogg write in VibeKeyboard/Recordings. Raw PCM is never retained.")
                .font(.caption).foregroundStyle(.secondary)
            Text("Recognition: unavailable until a reviewed provider contract exists").foregroundStyle(.secondary)
            LabeledContent("Last recording", value: model.lastRecording)
            if let message = model.diagnosticMessage {
                Text(message).font(.caption).foregroundStyle(.red).textSelection(.enabled)
            }
        }.formStyle(.grouped).padding(8)
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
