import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Key mapping persistence")
struct KeyMappingStoreTests {
    @Test func absentStoreLoadsVendorDefault() async throws {
        let store = MemoryConfigurationStore()
        let repository = KeyMappingRepository(store: store)
        #expect(try await repository.load() == .vendorDefault())
    }

    @Test func saveAndLoadRoundTripUsesAtomicBoundary() async throws {
        let store = MemoryConfigurationStore()
        let repository = KeyMappingRepository(store: store)
        let profile = KeyMappingProfile.vendorDefault()
        try await repository.save(profile)
        #expect(await store.replaceCount == 1)
        #expect(try await repository.load() == profile)
    }

    @Test func migratesSchemaZeroByAddingNoneLongBindings() async throws {
        let current = KeyMappingProfile.vendorDefault()
        let encoded = try JSONEncoder().encode(current)
        var object = try #require(JSONSerialization.jsonObject(with: encoded) as? [String: Any])
        object["schema_version"] = 0
        var mappings: [Any] = []
        for key in CanonicalKey.allCases {
            let binding = current.mappings[key]!
            mappings.append(key.rawValue)
            mappings.append([
                "single": try JSONSerialization.jsonObject(with: JSONEncoder().encode(binding.single)),
                "double": try JSONSerialization.jsonObject(with: JSONEncoder().encode(binding.double)),
            ])
        }
        object["mappings"] = mappings
        let store = MemoryConfigurationStore(data: try JSONSerialization.data(withJSONObject: object))
        let migrated = try await KeyMappingRepository(store: store).load()
        #expect(migrated.schemaVersion == 1)
        #expect(migrated.mappings.values.allSatisfy { $0.long == .none })
    }

    @Test func unknownSchemaFailsWithoutFallback() async throws {
        let store = MemoryConfigurationStore(data: Data(#"{"schema_version":9,"mappings":{}}"#.utf8))
        let repository = KeyMappingRepository(store: store)
        await #expect(throws: InputConfigurationError.unsupportedSchemaVersion(9)) {
            try await repository.load()
        }
    }

    @Test func failedAtomicReplaceIsSurfaced() async throws {
        let store = MemoryConfigurationStore(writeError: .persistenceWrite("disk full"))
        let repository = KeyMappingRepository(store: store)
        await #expect(throws: InputConfigurationError.persistenceWrite("disk full")) {
            try await repository.save(.vendorDefault())
        }
    }

    @Test func saveRejectsPostConstructionMissingKeyBeforeWrite() async throws {
        var profile = KeyMappingProfile.vendorDefault()
        profile.mappings.removeValue(forKey: .k1)
        let store = MemoryConfigurationStore()
        let repository = KeyMappingRepository(store: store)

        await #expect(throws: InputConfigurationError.incompleteMapping(missing: [.k1])) {
            try await repository.save(profile)
        }
        #expect(await store.replaceCount == 0)
    }

    @Test func saveRejectsPostConstructionInvalidActionBeforeWrite() async throws {
        var profile = KeyMappingProfile.vendorDefault()
        profile.mappings[.k1]?.single = .pasteText("   ")
        let store = MemoryConfigurationStore()
        let repository = KeyMappingRepository(store: store)

        await #expect(throws: InputConfigurationError.missingAssociatedValue(action: "paste_text")) {
            try await repository.save(profile)
        }
        #expect(await store.replaceCount == 0)
    }
}

private actor MemoryConfigurationStore: ConfigurationDataStore {
    var data: Data?
    var replaceCount = 0
    let writeError: InputConfigurationError?

    init(data: Data? = nil, writeError: InputConfigurationError? = nil) {
        self.data = data
        self.writeError = writeError
    }

    func read() -> Data? { data }

    func replaceAtomically(with data: Data) throws {
        if let writeError { throw writeError }
        self.data = data
        replaceCount += 1
    }
}
