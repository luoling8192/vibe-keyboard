import Foundation
import VibeBoardKit

enum PetCatalogSource: Equatable, Sendable {
    case local(URL)
    case petdex(URL)

    var label: String {
        switch self {
        case .local: "Local Codex pet"
        case .petdex: "Petdex"
        }
    }
}

struct PetCatalogItem: Identifiable, Equatable, Sendable {
    let id: String
    let slug: String
    let displayName: String
    let kind: String
    let submittedBy: String?
    let source: PetCatalogSource

    var attribution: String {
        guard let submittedBy, !submittedBy.isEmpty else { return source.label }
        return "\(source.label) · \(submittedBy)"
    }
}

enum PetAnimationChoice: String, CaseIterable, Identifiable, Sendable {
    case idle
    case runRight
    case runLeft
    case wave
    case jump
    case failed
    case waiting
    case run
    case review

    var id: String { rawValue }

    var label: String {
        switch self {
        case .idle: "Idle"
        case .runRight: "Run right"
        case .runLeft: "Run left"
        case .wave: "Wave"
        case .jump: "Jump"
        case .failed: "Failed"
        case .waiting: "Waiting"
        case .run: "Run"
        case .review: "Review"
        }
    }

    fileprivate var row: Int {
        switch self {
        case .idle: 0
        case .runRight: 1
        case .runLeft: 2
        case .wave: 3
        case .jump: 4
        case .failed: 5
        case .waiting: 6
        case .run: 7
        case .review: 8
        }
    }

    fileprivate var frameCount: Int {
        switch self {
        case .idle, .waiting, .run, .review: 6
        case .runRight, .runLeft, .failed: 8
        case .wave: 4
        case .jump: 5
        }
    }

    fileprivate var totalDurationMS: Int {
        switch self {
        case .idle: 1_100
        case .runRight, .runLeft: 1_060
        case .wave: 700
        case .jump: 840
        case .failed: 1_220
        case .waiting: 1_010
        case .run: 820
        case .review: 1_030
        }
    }
}

enum PetCatalogError: Error, CustomStringConvertible {
    case invalidManifest
    case invalidURL
    case response(Int)
    case tooLarge
    case invalidSpritesheet

    var description: String {
        switch self {
        case .invalidManifest: "Invalid Petdex manifest"
        case .invalidURL: "Invalid pet asset URL"
        case .response(let status): "Pet download HTTP \(status)"
        case .tooLarge: "Pet data exceeds the local safety limit"
        case .invalidSpritesheet: "Pet spritesheet must be an 8×9 grid"
        }
    }
}

protocol PetCatalogProviding: Sendable {
    func localPets() async -> [PetCatalogItem]
    func petdexPets() async throws -> [PetCatalogItem]
    func animation(
        for item: PetCatalogItem,
        choice: PetAnimationChoice,
        minimumFrameMS: UInt16,
        maximumFrameMS: UInt16
    ) async throws -> DecodedAssetSource
}

struct EmptyPetCatalog: PetCatalogProviding {
    func localPets() async -> [PetCatalogItem] { [] }
    func petdexPets() async throws -> [PetCatalogItem] { [] }
    func animation(
        for item: PetCatalogItem,
        choice: PetAnimationChoice,
        minimumFrameMS: UInt16,
        maximumFrameMS: UInt16
    ) async throws -> DecodedAssetSource {
        throw PetCatalogError.invalidSpritesheet
    }
}

actor ProductionPetCatalog: PetCatalogProviding {
    private static let manifestURL = URL(
        string: "https://assets.petdex.dev/manifests/petdex-v1.json"
    )!
    private static let maximumManifestBytes = 12 * 1_024 * 1_024
    private static let maximumSpritesheetBytes = 32 * 1_024 * 1_024
    private static let cacheMaxAge: TimeInterval = 3600  // 1 hour

    private let cacheURL: URL

    init() {
        let support = FileManager.default.urls(
            for: .applicationSupportDirectory,
            in: .userDomainMask
        ).first!.appendingPathComponent("VibeKeyboard", isDirectory: true)
        cacheURL = support.appendingPathComponent("petdex-cache.json", isDirectory: false)
    }

    func localPets() async -> [PetCatalogItem] {
        Self.readLocalPets()
    }

    func petdexPets() async throws -> [PetCatalogItem] {
        // Try network first; fall back to cache on failure.
        do {
            let items = try await fetchRemotePets()
            try? saveCache(items)
            return items
        } catch {
            if let cached = readCache() {
                return cached
            }
            throw error
        }
    }

    private func fetchRemotePets() async throws -> [PetCatalogItem] {
        var request = URLRequest(url: Self.manifestURL)
        request.timeoutInterval = 30
        let data = try await Self.download(
            request,
            maximumBytes: Self.maximumManifestBytes
        )
        let manifest: PetdexManifest
        do {
            manifest = try JSONDecoder().decode(PetdexManifest.self, from: data)
        } catch {
            throw PetCatalogError.invalidManifest
        }
        guard manifest.total == manifest.pets.count else {
            throw PetCatalogError.invalidManifest
        }
        return manifest.pets.compactMap { value in
            guard Self.allowedRemoteURL(value.spritesheetURL) else { return nil }
            return PetCatalogItem(
                id: "petdex:\(value.slug)",
                slug: value.slug,
                displayName: value.displayName,
                kind: value.kind,
                submittedBy: value.submittedBy,
                source: .petdex(value.spritesheetURL)
            )
        }
    }

    private struct CachedPetdex: Codable {
        let timestamp: Date
        let pets: [CachedPet]
    }

    private struct CachedPet: Codable {
        let slug: String
        let displayName: String
        let kind: String
        let submittedBy: String?
        let spritesheetURL: String
    }

    private func saveCache(_ items: [PetCatalogItem]) throws {
        let cached = CachedPetdex(
            timestamp: Date(),
            pets: items.map { pet in
                let url: String
                if case .petdex(let petURL) = pet.source {
                    url = petURL.absoluteString
                } else {
                    url = ""
                }
                return CachedPet(
                    slug: pet.slug,
                    displayName: pet.displayName,
                    kind: pet.kind,
                    submittedBy: pet.submittedBy,
                    spritesheetURL: url
                )
            }
        )
        let dir = cacheURL.deletingLastPathComponent()
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let data = try JSONEncoder().encode(cached)
        try data.write(to: cacheURL, options: .atomic)
    }

    private func readCache() -> [PetCatalogItem]? {
        guard let data = try? Data(contentsOf: cacheURL),
              let cached = try? JSONDecoder().decode(CachedPetdex.self, from: data) else {
            return nil
        }
        // Use cache even if stale — better than nothing when offline.
        return cached.pets.compactMap { pet in
            guard let url = URL(string: pet.spritesheetURL),
                  Self.allowedRemoteURL(url) else { return nil }
            return PetCatalogItem(
                id: "petdex:\(pet.slug)",
                slug: pet.slug,
                displayName: pet.displayName,
                kind: pet.kind,
                submittedBy: pet.submittedBy,
                source: .petdex(url)
            )
        }
    }

    func animation(
        for item: PetCatalogItem,
        choice: PetAnimationChoice,
        minimumFrameMS: UInt16,
        maximumFrameMS: UInt16
    ) async throws -> DecodedAssetSource {
        let data: Data
        switch item.source {
        case .local(let url):
            data = try Data(contentsOf: url, options: .mappedIfSafe)
            guard data.count <= Self.maximumSpritesheetBytes else {
                throw PetCatalogError.tooLarge
            }
        case .petdex(let url):
            guard Self.allowedRemoteURL(url) else { throw PetCatalogError.invalidURL }
            var request = URLRequest(url: url)
            request.timeoutInterval = 60
            data = try await Self.download(
                request,
                maximumBytes: Self.maximumSpritesheetBytes
            )
        }

        let decoded = try AssetSourceDecoder.decode(
            data,
            limits: AssetConversionLimits(
                maxSourceWidth: 4_096,
                maxSourceHeight: 4_096,
                maxSourcePixels: 16_777_216,
                maxFrames: 1,
                maxDecodedBytes: 128 * 1_024 * 1_024
            ),
            minimumFrameMS: minimumFrameMS,
            maximumFrameMS: maximumFrameMS
        )
        guard !decoded.animated, decoded.frames.count == 1 else {
            throw PetCatalogError.invalidSpritesheet
        }
        let sheet = decoded.frames[0].raster
        guard sheet.width % 8 == 0, sheet.height % 9 == 0 else {
            throw PetCatalogError.invalidSpritesheet
        }
        let frameWidth = sheet.width / 8
        let frameHeight = sheet.height / 9
        guard frameWidth > 0, frameHeight > 0 else {
            throw PetCatalogError.invalidSpritesheet
        }
        let rawDuration = choice.totalDurationMS / choice.frameCount
        let duration = UInt16(
            min(Int(maximumFrameMS), max(Int(minimumFrameMS), rawDuration))
        )
        let frames = try (0..<choice.frameCount).map { column in
            DecodedAssetFrame(
                raster: try Self.crop(
                    sheet,
                    x: column * frameWidth,
                    y: choice.row * frameHeight,
                    width: frameWidth,
                    height: frameHeight
                ),
                durationMS: duration
            )
        }
        return DecodedAssetSource(frames: frames, animated: true)
    }

    private static func download(_ request: URLRequest, maximumBytes: Int) async throws -> Data {
        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse else {
            throw PetCatalogError.invalidURL
        }
        guard (200...299).contains(http.statusCode) else {
            throw PetCatalogError.response(http.statusCode)
        }
        guard !data.isEmpty, data.count <= maximumBytes else {
            throw PetCatalogError.tooLarge
        }
        return data
    }

    private static func allowedRemoteURL(_ url: URL) -> Bool {
        guard url.scheme == "https", let host = url.host?.lowercased() else { return false }
        return host == "petdex.dev" || host.hasSuffix(".petdex.dev")
    }

    private static func readLocalPets() -> [PetCatalogItem] {
        let root = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".codex/pets", isDirectory: true)
        guard let directories = try? FileManager.default.contentsOfDirectory(
            at: root,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        ) else { return [] }
        return directories.compactMap { directory in
            let metadataURL = directory.appendingPathComponent("pet.json")
            guard let data = try? Data(contentsOf: metadataURL),
                  let metadata = try? JSONDecoder().decode(LocalPetMetadata.self, from: data) else {
                return nil
            }
            let sheet = directory.appendingPathComponent(metadata.spritesheetPath)
                .standardizedFileURL
            let base = directory.standardizedFileURL.path + "/"
            guard sheet.path.hasPrefix(base),
                  FileManager.default.isReadableFile(atPath: sheet.path) else { return nil }
            return PetCatalogItem(
                id: "local:\(metadata.id)",
                slug: metadata.id,
                displayName: metadata.displayName,
                kind: "local",
                submittedBy: nil,
                source: .local(sheet)
            )
        }.sorted {
            $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending
        }
    }

    static func crop(
        _ raster: AssetRaster,
        x: Int,
        y: Int,
        width: Int,
        height: Int
    ) throws -> AssetRaster {
        guard x >= 0, y >= 0, width > 0, height > 0,
              x <= raster.width - width,
              y <= raster.height - height else {
            throw PetCatalogError.invalidSpritesheet
        }
        var pixels: [AssetRGBA8] = []
        pixels.reserveCapacity(width * height)
        for row in y..<(y + height) {
            let start = row * raster.width + x
            pixels.append(contentsOf: raster.pixels[start..<(start + width)])
        }
        return try AssetRaster(width: width, height: height, pixels: pixels)
    }
}

private struct LocalPetMetadata: Decodable {
    let id: String
    let displayName: String
    let spritesheetPath: String
}

private struct PetdexManifest: Decodable {
    let total: Int
    let pets: [PetdexManifestItem]
}

private struct PetdexManifestItem: Decodable {
    let slug: String
    let displayName: String
    let kind: String
    let submittedBy: String?
    let spritesheetURL: URL

    private enum CodingKeys: String, CodingKey {
        case slug
        case displayName
        case kind
        case submittedBy
        case spritesheetURL = "spritesheetUrl"
    }
}
