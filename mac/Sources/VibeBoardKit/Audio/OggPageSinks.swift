import Darwin
import Foundation

public enum OggSinkError: Error, Equatable, Sendable {
    case createFailed(path: String, errno: Int32)
    case writeFailed(errno: Int32)
    case syncFailed(errno: Int32)
    case closeFailed(errno: Int32)
    case renameFailed(errno: Int32)
    case alreadyClosed
}

public final class DataOggPageSink: OggPageSink {
    public private(set) var data = Data()
    public private(set) var isCommitted = false
    public private(set) var isCancelled = false

    public init() {}

    public func write(_ data: Data) throws {
        guard !isCommitted, !isCancelled else { throw OggSinkError.alreadyClosed }
        self.data.append(data)
    }

    public func commit() throws {
        guard !isCommitted, !isCancelled else { throw OggSinkError.alreadyClosed }
        isCommitted = true
    }

    public func cancel() {
        isCancelled = true
        data.removeAll(keepingCapacity: false)
    }
}

protocol OggFileOperations: AnyObject {
    var errorNumber: Int32 { get }

    func open(path: String, flags: Int32, mode: mode_t) -> Int32
    func write(fileDescriptor: Int32, buffer: UnsafeRawPointer, count: Int) -> Int
    func synchronize(fileDescriptor: Int32) -> Int32
    func close(fileDescriptor: Int32) -> Int32
    func rename(source: String, destination: String) -> Int32
    func remove(path: String) -> Int32
}

private final class DarwinOggFileOperations: OggFileOperations {
    var errorNumber: Int32 { errno }

    func open(path: String, flags: Int32, mode: mode_t) -> Int32 {
        Darwin.open(path, flags, mode)
    }

    func write(fileDescriptor: Int32, buffer: UnsafeRawPointer, count: Int) -> Int {
        Darwin.write(fileDescriptor, buffer, count)
    }

    func synchronize(fileDescriptor: Int32) -> Int32 {
        Darwin.fsync(fileDescriptor)
    }

    func close(fileDescriptor: Int32) -> Int32 {
        Darwin.close(fileDescriptor)
    }

    func rename(source: String, destination: String) -> Int32 {
        Darwin.rename(source, destination)
    }

    func remove(path: String) -> Int32 {
        Darwin.unlink(path)
    }
}

public final class AtomicFileOggPageSink: OggPageSink {
    public let destinationURL: URL
    public let temporaryURL: URL

    private let operations: any OggFileOperations
    private var fileDescriptor: Int32
    private var isClosed = false

    public convenience init(destinationURL: URL) throws {
        try self.init(destinationURL: destinationURL, operations: DarwinOggFileOperations())
    }

    init(destinationURL: URL, operations: any OggFileOperations) throws {
        self.destinationURL = destinationURL
        self.operations = operations
        let directory = destinationURL.deletingLastPathComponent()
        let name = ".\(destinationURL.lastPathComponent).\(UUID().uuidString).tmp"
        temporaryURL = directory.appendingPathComponent(name, isDirectory: false)

        fileDescriptor = operations.open(
            path: temporaryURL.path,
            flags: O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
            mode: S_IRUSR | S_IWUSR
        )
        guard fileDescriptor >= 0 else {
            throw OggSinkError.createFailed(path: temporaryURL.path, errno: operations.errorNumber)
        }
    }

    deinit {
        cancel()
    }

    public func write(_ data: Data) throws {
        guard !isClosed else { throw OggSinkError.alreadyClosed }
        do {
            try data.withUnsafeBytes { rawBuffer in
                guard let baseAddress = rawBuffer.baseAddress else { return }
                var written = 0
                while written < rawBuffer.count {
                    let result = operations.write(
                        fileDescriptor: fileDescriptor,
                        buffer: baseAddress.advanced(by: written),
                        count: rawBuffer.count - written
                    )
                    if result > 0 {
                        written += result
                    } else if result < 0, operations.errorNumber == EINTR {
                        continue
                    } else {
                        let failure = operations.errorNumber == 0 ? EIO : operations.errorNumber
                        throw OggSinkError.writeFailed(errno: failure)
                    }
                }
            }
        } catch {
            closeAndRemove()
            throw error
        }
    }

    public func commit() throws {
        guard !isClosed else { throw OggSinkError.alreadyClosed }
        if operations.synchronize(fileDescriptor: fileDescriptor) != 0 {
            let failure = operations.errorNumber
            closeAndRemove()
            throw OggSinkError.syncFailed(errno: failure)
        }
        if operations.close(fileDescriptor: fileDescriptor) != 0 {
            let failure = operations.errorNumber
            fileDescriptor = -1
            isClosed = true
            _ = operations.remove(path: temporaryURL.path)
            throw OggSinkError.closeFailed(errno: failure)
        }
        fileDescriptor = -1
        isClosed = true

        if operations.rename(source: temporaryURL.path, destination: destinationURL.path) != 0 {
            let failure = operations.errorNumber
            _ = operations.remove(path: temporaryURL.path)
            throw OggSinkError.renameFailed(errno: failure)
        }
    }

    public func cancel() {
        guard !isClosed else { return }
        closeAndRemove()
    }

    private func closeAndRemove() {
        if fileDescriptor >= 0 {
            _ = operations.close(fileDescriptor: fileDescriptor)
            fileDescriptor = -1
        }
        isClosed = true
        _ = operations.remove(path: temporaryURL.path)
    }
}
