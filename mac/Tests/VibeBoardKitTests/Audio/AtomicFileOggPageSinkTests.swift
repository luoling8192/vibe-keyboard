import Darwin
import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Atomic Ogg page sink")
struct AtomicFileOggPageSinkTests {
    @Test func realCommitAtomicallyReplacesDestinationWithPrivateMode() throws {
        try withTemporaryDestination { destination in
            try Data("old".utf8).write(to: destination)
            let sink = try AtomicFileOggPageSink(destinationURL: destination)
            try sink.write(Data("replacement".utf8))
            let attributes = try FileManager.default.attributesOfItem(atPath: sink.temporaryURL.path)
            #expect((attributes[.posixPermissions] as? NSNumber)?.intValue == 0o600)
            try sink.commit()
            #expect(try Data(contentsOf: destination) == Data("replacement".utf8))
            #expect(!FileManager.default.fileExists(atPath: sink.temporaryURL.path))
        }
    }

    @Test func realCancelRemovesTemporaryAndPreservesDestination() throws {
        try withTemporaryDestination { destination in
            try Data("old".utf8).write(to: destination)
            let sink = try AtomicFileOggPageSink(destinationURL: destination)
            try sink.write(Data("new".utf8))
            let temporary = sink.temporaryURL
            sink.cancel()
            #expect(try Data(contentsOf: destination) == Data("old".utf8))
            #expect(!FileManager.default.fileExists(atPath: temporary.path))
        }
    }

    @Test func openFailureIsTyped() throws {
        let operations = ScriptedOggFileOperations(openResult: -1, errorNumber: EACCES)
        let destination = URL(fileURLWithPath: "/tmp/never-created.ogg")
        do {
            _ = try AtomicFileOggPageSink(destinationURL: destination, operations: operations)
            Issue.record("Expected create failure")
        } catch let OggSinkError.createFailed(path, code) {
            #expect(path.contains(".never-created.ogg."))
            #expect(code == EACCES)
        }
        #expect(operations.closeCalls == 0)
        #expect(operations.removePaths.count == 1)
        #expect(operations.removePaths[0].contains(".never-created.ogg."))
    }

    @Test func partialWritesAdvanceThroughEntireBuffer() throws {
        let operations = ScriptedOggFileOperations(writeSteps: [.success(2), .success(3)])
        let sink = try injectedSink(operations)
        try sink.write(Data([1, 2, 3, 4, 5]))
        #expect(operations.writeRequestedCounts == [5, 3])
        #expect(operations.writtenData == Data([1, 2, 3, 4, 5]))
        sink.cancel()
        #expect(operations.closeCalls == 1)
    }

    @Test func interruptedWriteRetriesWithoutLosingData() throws {
        let operations = ScriptedOggFileOperations(writeSteps: [.failure(EINTR), .success(3)])
        let sink = try injectedSink(operations)
        try sink.write(Data([7, 8, 9]))
        #expect(operations.writeRequestedCounts == [3, 3])
        #expect(operations.writtenData == Data([7, 8, 9]))
        sink.cancel()
        #expect(operations.closeCalls == 1)
    }

    @Test(arguments: [WriteFailureCase.zero, .error])
    fileprivate func writeFailureClosesOnceRemovesTemporaryAndPreservesDestination(failure: WriteFailureCase) throws {
        try withOldDestination { destination in
            let steps: [ScriptedOggFileOperations.WriteStep] = failure == .zero ? [.success(0)] : [.failure(EIO)]
            let operations = ScriptedOggFileOperations(writeSteps: steps)
            var temporaryPath = ""
            do {
                let sink = try AtomicFileOggPageSink(destinationURL: destination, operations: operations)
                temporaryPath = sink.temporaryURL.path
                #expect(throws: OggSinkError.writeFailed(errno: EIO)) {
                    try sink.write(Data([1]))
                }
                sink.cancel()
            }
            #expect(operations.closeCalls == 1)
            #expect(operations.removePaths == [temporaryPath])
            #expect(try Data(contentsOf: destination) == Data("old".utf8))
        }
    }

    @Test func syncFailureClosesOnceRemovesTemporaryAndPreservesDestination() throws {
        try assertCommitFailure(
            operations: ScriptedOggFileOperations(syncResult: -1, errorNumber: EIO),
            expected: .syncFailed(errno: EIO),
            expectedCloseCalls: 1
        )
    }

    @Test func closeFailureDoesNotAttemptSecondCloseAndPreservesDestination() throws {
        try assertCommitFailure(
            operations: ScriptedOggFileOperations(closeResult: -1, errorNumber: EIO),
            expected: .closeFailed(errno: EIO),
            expectedCloseCalls: 1
        )
    }

    @Test func renameFailureRemovesTemporaryWithoutSecondCloseAndPreservesDestination() throws {
        try assertCommitFailure(
            operations: ScriptedOggFileOperations(renameResult: -1, errorNumber: EXDEV),
            expected: .renameFailed(errno: EXDEV),
            expectedCloseCalls: 1
        )
    }

    private func assertCommitFailure(
        operations: ScriptedOggFileOperations,
        expected: OggSinkError,
        expectedCloseCalls: Int
    ) throws {
        try withOldDestination { destination in
            var temporaryPath = ""
            do {
                let sink = try AtomicFileOggPageSink(destinationURL: destination, operations: operations)
                temporaryPath = sink.temporaryURL.path
                try sink.write(Data([1, 2]))
                #expect(throws: expected) { try sink.commit() }
                sink.cancel()
            }
            #expect(operations.closeCalls == expectedCloseCalls)
            #expect(operations.removePaths == [temporaryPath])
            #expect(try Data(contentsOf: destination) == Data("old".utf8))
        }
    }
}

private enum WriteFailureCase: Sendable { case zero, error }

private final class ScriptedOggFileOperations: OggFileOperations {
    enum WriteStep { case success(Int), failure(Int32) }

    var errorNumber: Int32
    var writeSteps: [WriteStep]
    var writeRequestedCounts: [Int] = []
    var writtenData = Data()
    var closeCalls = 0
    var removePaths: [String] = []
    var renameCalls = 0

    private let openResult: Int32
    private let syncResult: Int32
    private let closeResult: Int32
    private let renameResult: Int32

    init(
        openResult: Int32 = 42,
        writeSteps: [WriteStep] = [],
        syncResult: Int32 = 0,
        closeResult: Int32 = 0,
        renameResult: Int32 = 0,
        errorNumber: Int32 = EIO
    ) {
        self.openResult = openResult
        self.writeSteps = writeSteps
        self.syncResult = syncResult
        self.closeResult = closeResult
        self.renameResult = renameResult
        self.errorNumber = errorNumber
    }

    func open(path: String, flags: Int32, mode: mode_t) -> Int32 { openResult }

    func write(fileDescriptor: Int32, buffer: UnsafeRawPointer, count: Int) -> Int {
        writeRequestedCounts.append(count)
        let step = writeSteps.isEmpty ? .success(count) : writeSteps.removeFirst()
        switch step {
        case .success(let result):
            if result > 0 { writtenData.append(buffer.assumingMemoryBound(to: UInt8.self), count: result) }
            return result
        case .failure(let code):
            errorNumber = code
            return -1
        }
    }

    func synchronize(fileDescriptor: Int32) -> Int32 { syncResult }
    func close(fileDescriptor: Int32) -> Int32 { closeCalls += 1; return closeResult }
    func rename(source: String, destination: String) -> Int32 { renameCalls += 1; return renameResult }
    func remove(path: String) -> Int32 { removePaths.append(path); return 0 }
}

private func injectedSink(_ operations: ScriptedOggFileOperations) throws -> AtomicFileOggPageSink {
    try AtomicFileOggPageSink(
        destinationURL: URL(fileURLWithPath: "/tmp/injected-recording.ogg"),
        operations: operations
    )
}

private func withTemporaryDestination(_ body: (URL) throws -> Void) throws {
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false)
    defer { try? FileManager.default.removeItem(at: directory) }
    try body(directory.appendingPathComponent("recording.ogg"))
}

private func withOldDestination(_ body: (URL) throws -> Void) throws {
    try withTemporaryDestination { destination in
        try Data("old".utf8).write(to: destination)
        try body(destination)
    }
}
