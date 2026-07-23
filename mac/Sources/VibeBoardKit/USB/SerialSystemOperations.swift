import Foundation
#if os(macOS)
import Darwin
#endif

public enum SerialIOResult<Value: Sendable>: Sendable {
    case value(Value)
    case failure(Int32)
}

public protocol SerialSystemOperating: Sendable {
    func open(path: String, flags: Int32) -> SerialIOResult<Int32>
    func configureRaw(fileDescriptor: Int32) -> SerialIOResult<Void>
    func read(fileDescriptor: Int32, maximumCount: Int) -> SerialIOResult<Data>
    func write(fileDescriptor: Int32, data: Data) -> SerialIOResult<Int>
    func wait(fileDescriptor: Int32, events: Int16, timeoutMilliseconds: Int32) -> SerialIOResult<Bool>
    func close(fileDescriptor: Int32) -> SerialIOResult<Void>
}

protocol DarwinSerialSyscallOperating: Sendable {
    func open(path: String, flags: Int32) -> SerialIOResult<Int32>
    func getDescriptorFlags(fileDescriptor: Int32) -> SerialIOResult<Int32>
    func setDescriptorFlags(fileDescriptor: Int32, flags: Int32) -> SerialIOResult<Void>
    func getAttributes(fileDescriptor: Int32) -> SerialIOResult<termios>
    func setAttributes(fileDescriptor: Int32, attributes: termios) -> SerialIOResult<Void>
    func close(fileDescriptor: Int32) -> SerialIOResult<Void>
}

struct DarwinSerialSyscalls: DarwinSerialSyscallOperating {
    func open(path: String, flags: Int32) -> SerialIOResult<Int32> {
        let fd = Darwin.open(path, flags)
        return fd >= 0 ? .value(fd) : .failure(errno)
    }

    func getDescriptorFlags(fileDescriptor: Int32) -> SerialIOResult<Int32> {
        let flags = fcntl(fileDescriptor, F_GETFD)
        return flags >= 0 ? .value(flags) : .failure(errno)
    }

    func setDescriptorFlags(fileDescriptor: Int32, flags: Int32) -> SerialIOResult<Void> {
        fcntl(fileDescriptor, F_SETFD, flags) == 0 ? .value(()) : .failure(errno)
    }

    func getAttributes(fileDescriptor: Int32) -> SerialIOResult<termios> {
        var attributes = termios()
        return tcgetattr(fileDescriptor, &attributes) == 0 ? .value(attributes) : .failure(errno)
    }

    func setAttributes(fileDescriptor: Int32, attributes: termios) -> SerialIOResult<Void> {
        var mutableAttributes = attributes
        return tcsetattr(fileDescriptor, TCSAFLUSH, &mutableAttributes) == 0 ? .value(()) : .failure(errno)
    }

    func close(fileDescriptor: Int32) -> SerialIOResult<Void> {
        Darwin.close(fileDescriptor) == 0 ? .value(()) : .failure(errno)
    }
}

public struct DarwinSerialSystemOperations: SerialSystemOperating {
    private let syscalls: any DarwinSerialSyscallOperating

    public init() { self.syscalls = DarwinSerialSyscalls() }

    init(syscalls: any DarwinSerialSyscallOperating) { self.syscalls = syscalls }

    public func open(path: String, flags: Int32) -> SerialIOResult<Int32> {
        let fd: Int32
        switch syscalls.open(path: path, flags: flags) {
        case .value(let value): fd = value
        case .failure(let code): return .failure(code)
        }
        let descriptorFlags: Int32
        switch syscalls.getDescriptorFlags(fileDescriptor: fd) {
        case .value(let value): descriptorFlags = value
        case .failure(let code):
            _ = syscalls.close(fileDescriptor: fd)
            return .failure(code)
        }
        switch syscalls.setDescriptorFlags(fileDescriptor: fd, flags: descriptorFlags | FD_CLOEXEC) {
        case .value: return .value(fd)
        case .failure(let code):
            _ = syscalls.close(fileDescriptor: fd)
            return .failure(code)
        }
    }

    public func configureRaw(fileDescriptor: Int32) -> SerialIOResult<Void> {
        var settings: termios
        switch syscalls.getAttributes(fileDescriptor: fileDescriptor) {
        case .value(let value): settings = value
        case .failure(let code): return .failure(code)
        }
        cfmakeraw(&settings)
        settings.c_cflag |= tcflag_t(CLOCAL | CREAD)
        return syscalls.setAttributes(fileDescriptor: fileDescriptor, attributes: settings)
    }

    public func read(fileDescriptor: Int32, maximumCount: Int) -> SerialIOResult<Data> {
        var buffer = [UInt8](repeating: 0, count: maximumCount)
        let count = Darwin.read(fileDescriptor, &buffer, maximumCount)
        guard count >= 0 else { return .failure(errno) }
        return .value(Data(buffer.prefix(count)))
    }

    public func write(fileDescriptor: Int32, data: Data) -> SerialIOResult<Int> {
        data.withUnsafeBytes { raw in
            let count = Darwin.write(fileDescriptor, raw.baseAddress, raw.count)
            guard count >= 0 else { return .failure(errno) }
            return .value(count)
        }
    }

    public func wait(fileDescriptor: Int32, events: Int16, timeoutMilliseconds: Int32) -> SerialIOResult<Bool> {
        var descriptor = pollfd(fd: fileDescriptor, events: events, revents: 0)
        let result = poll(&descriptor, 1, timeoutMilliseconds)
        guard result >= 0 else { return .failure(errno) }
        if result == 0 { return .value(false) }
        return .value((descriptor.revents & events) != 0)
    }

    public func close(fileDescriptor: Int32) -> SerialIOResult<Void> {
        syscalls.close(fileDescriptor: fileDescriptor)
    }
}

public protocol USBMonotonicClock: Sendable {
    func nowNanoseconds() -> UInt64
}

public struct ContinuousUSBClock: USBMonotonicClock {
    public init() {}
    public func nowNanoseconds() -> UInt64 { DispatchTime.now().uptimeNanoseconds }
}
