import Foundation

public protocol ConfigurationDataStore: Sendable {
    func read() async throws -> Data?
    func replaceAtomically(with data: Data) async throws
}

public actor FileConfigurationDataStore: ConfigurationDataStore {
    private let url: URL
    private let fileManager: FileManager

    public init(url: URL, fileManager: FileManager = .default) {
        self.url = url
        self.fileManager = fileManager
    }

    public func read() throws -> Data? {
        guard fileManager.fileExists(atPath: url.path) else { return nil }
        do {
            return try Data(contentsOf: url, options: .mappedIfSafe)
        } catch {
            throw InputConfigurationError.persistenceRead(String(describing: error))
        }
    }

    public func replaceAtomically(with data: Data) throws {
        let directory = url.deletingLastPathComponent()
        let temporaryURL = directory.appendingPathComponent(".\(url.lastPathComponent).\(UUID().uuidString).tmp")
        do {
            try fileManager.createDirectory(at: directory, withIntermediateDirectories: true)
            try data.write(to: temporaryURL, options: [.atomic])
            if fileManager.fileExists(atPath: url.path) {
                _ = try fileManager.replaceItemAt(url, withItemAt: temporaryURL)
            } else {
                try fileManager.moveItem(at: temporaryURL, to: url)
            }
        } catch {
            try? fileManager.removeItem(at: temporaryURL)
            throw InputConfigurationError.persistenceWrite(String(describing: error))
        }
    }
}

public actor KeyMappingRepository {
    private let store: any ConfigurationDataStore
    private let encoder: JSONEncoder
    private let decoder: JSONDecoder

    public init(store: any ConfigurationDataStore) {
        self.store = store
        self.encoder = JSONEncoder()
        self.encoder.outputFormatting = [.sortedKeys]
        self.decoder = JSONDecoder()
    }

    public func load() async throws -> KeyMappingProfile {
        guard let data = try await store.read() else {
            return .vendorDefault()
        }
        do {
            let header = try decoder.decode(SchemaHeader.self, from: data)
            switch header.schemaVersion {
            case KeyMappingProfile.currentSchemaVersion:
                return try decoder.decode(KeyMappingProfile.self, from: data)
            case 0:
                return try migrate(decoder.decode(LegacyProfileV0.self, from: data))
            default:
                throw InputConfigurationError.unsupportedSchemaVersion(header.schemaVersion)
            }
        } catch let error as InputConfigurationError {
            throw error
        } catch {
            throw InputConfigurationError.invalidStoredConfiguration(String(describing: error))
        }
    }

    public func save(_ profile: KeyMappingProfile) async throws {
        try profile.validate()
        do {
            try await store.replaceAtomically(with: encoder.encode(profile))
        } catch let error as InputConfigurationError {
            throw error
        } catch {
            throw InputConfigurationError.persistenceWrite(String(describing: error))
        }
    }

    private func migrate(_ legacy: LegacyProfileV0) throws -> KeyMappingProfile {
        try KeyMappingProfile(
            mappings: legacy.mappings.mapValues {
                KeyBindings(single: $0.single, double: $0.double, long: .none)
            }
        )
    }
}

private struct SchemaHeader: Decodable {
    let schemaVersion: Int

    enum CodingKeys: String, CodingKey {
        case schemaVersion = "schema_version"
    }
}

private struct LegacyProfileV0: Decodable {
    let mappings: [CanonicalKey: LegacyBindingsV0]
}

private struct LegacyBindingsV0: Decodable {
    let single: HostAction
    let double: HostAction
}
