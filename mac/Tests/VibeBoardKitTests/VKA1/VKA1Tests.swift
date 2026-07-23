import Foundation
import Testing
@testable import VibeBoardKit

@Suite("VKA1 codec")
struct VKA1Tests {
    private let limits = VKA1Limits(maxFrames: 8, minFrameDurationMS: 10, maxFrameDurationMS: 1_000, maxContainerBytes: 1_000_000, maxDecodedBytes: 428 * 142 * 2)

    @Test func deterministicEncodingSelectionAndRoundTrip() throws {
        let one = try VKA1Codec.encode(kind: .image, width: 1, height: 1, frames: [.init(pixels: [0x1234], durationMS: 0)], limits: limits)
        let decodedOne = try VKA1Codec.decode(one, limits: limits)
        #expect(decodedOne.frames[0].encoding == .raw) // 2 raw bytes versus 4 RLE bytes.
        #expect(decodedOne.frames[0].pixels == [0x1234])

        let equal = try VKA1Codec.encode(kind: .image, width: 2, height: 1, frames: [.init(pixels: [7, 7], durationMS: 0)], limits: limits)
        #expect(try VKA1Codec.decode(equal, limits: limits).frames[0].encoding == .raw) // 4 == 4 selects raw.

        let run = try VKA1Codec.encode(kind: .image, width: 428, height: 1, frames: [.init(pixels: Array(repeating: 9, count: 428), durationMS: 0)], limits: limits)
        #expect(try VKA1Codec.decode(run, limits: limits).frames[0].encoding == .rowRLE)

        let mixed = try VKA1Codec.encode(kind: .animation, width: 4, height: 1, frames: [
            .init(pixels: [1, 2, 3, 4], durationMS: 10),
            .init(pixels: [5, 5, 5, 5], durationMS: 20),
        ], limits: limits)
        #expect(mixed[6] == 3)
        #expect(try VKA1Codec.decode(mixed, limits: limits).frames.map(\.encoding) == [.raw, .rowRLE])
    }

    @Test func aggregateDecodedMemoryIsAdmittedBeforeFrameAllocation() throws {
        let permissive = VKA1Limits(maxFrames: 2, minFrameDurationMS: 10, maxFrameDurationMS: 1_000, maxContainerBytes: 1_000, maxDecodedBytes: 16)
        let data = try VKA1Codec.encode(kind: .animation, width: 4, height: 1, frames: [
            .init(pixels: [1, 2, 3, 4], durationMS: 10),
            .init(pixels: [5, 6, 7, 8], durationMS: 10),
        ], limits: permissive)
        let aggregateBound = VKA1Limits(maxFrames: 2, minFrameDurationMS: 10, maxFrameDurationMS: 1_000, maxContainerBytes: 1_000, maxDecodedBytes: 8)
        #expect(throws: VKA1Error.limitExceeded) {
            try VKA1Codec.decode(data, limits: aggregateBound)
        }
    }

    @Test func rowBoundaryAndCanonicalRuns() throws {
        let data = try VKA1Codec.encode(kind: .image, width: 3, height: 2, frames: [.init(pixels: Array(repeating: 0xabcd, count: 6), durationMS: 0)], limits: limits)
        let length = Int(read32(data, 60))
        #expect(length == 8) // One maximal run per row, never merged across rows.
        #expect(try VKA1Codec.decode(data, limits: limits).frames[0].pixels == Array(repeating: 0xabcd, count: 6))
    }

    @Test func rejectsHashRangeHeaderAndDurationMutations() throws {
        let valid = try VKA1Codec.encode(kind: .animation, width: 2, height: 1, frames: [.init(pixels: [1, 2], durationMS: 10)], limits: limits)
        for offset in [0, 4, 5, 6, 7, 8, 12, 14, 16, 20, 24, 56, 60, 64, 66, 67, valid.count - 1] {
            var changed = valid
            changed[offset] ^= 0xff
            #expect(throws: (any Error).self) { try VKA1Codec.decode(changed, limits: limits) }
        }
        #expect(throws: (any Error).self) { try VKA1Codec.decode(valid.dropLast(), limits: limits) }
        #expect(throws: (any Error).self) { try VKA1Codec.decode(valid + Data([0]), limits: limits) }
    }

    @Test func deterministicMutationCorpus() throws {
        let directory = try #require(Bundle.module.url(forResource: "manifest", withExtension: "txt", subdirectory: "Fixtures/VKA1")?.deletingLastPathComponent())
        let valid = try Data(contentsOf: directory.appendingPathComponent("mixed.vka1"))
        let vectors = try String(contentsOf: directory.appendingPathComponent("mutations-v1.csv"), encoding: .utf8)
            .split(separator: "\n").dropFirst()
        #expect(vectors.count == 512)
        for vector in vectors {
            let fields = vector.split(separator: ",")
            #expect(fields.count == 4 && fields[0] == "mixed" && fields[3] == "reject")
            var mutated = valid
            let offset = try #require(Int(fields[1]))
            let bit = try #require(UInt8(fields[2]))
            mutated[offset] ^= UInt8(1 << bit)
            #expect(throws: (any Error).self) { try VKA1Codec.decode(mutated, limits: limits) }
        }
    }

    @Test func sharedCompleteCorpusMatchesWriter() throws {
        let directory = try #require(Bundle.module.url(forResource: "manifest", withExtension: "txt", subdirectory: "Fixtures/VKA1")?.deletingLastPathComponent())
        let cases: [(String, VKA1Kind, UInt16, UInt16, [VKA1SourceFrame])] = [
            ("one-pixel", .image, 1, 1, [.init(pixels: [0x1234], durationMS: 0)]),
            ("equal-raw", .image, 2, 1, [.init(pixels: [7, 7], durationMS: 0)]),
            ("full-run", .image, 428, 1, [.init(pixels: Array(repeating: 9, count: 428), durationMS: 0)]),
            ("row-boundary", .image, 3, 2, [.init(pixels: Array(repeating: 0xabcd, count: 6), durationMS: 0)]),
            ("mixed", .animation, 4, 1, [.init(pixels: [1,2,3,4], durationMS: 10), .init(pixels: [5,5,5,5], durationMS: 20)]),
        ]
        for item in cases {
            let generated = try VKA1Codec.encode(kind: item.1, width: item.2, height: item.3, frames: item.4, limits: limits)
            let fixture = try Data(contentsOf: directory.appendingPathComponent(item.0 + ".vka1"))
            #expect(generated == fixture)
            _ = try VKA1Codec.decode(generated, limits: limits)
        }
    }

    private func read32(_ data: Data, _ offset: Int) -> UInt32 {
        UInt32(data[offset]) | UInt32(data[offset + 1]) << 8 | UInt32(data[offset + 2]) << 16 | UInt32(data[offset + 3]) << 24
    }
}
