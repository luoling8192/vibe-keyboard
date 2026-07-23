import Darwin
import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Darwin serial operations")
struct SerialSystemOperationsTests {
    @Test(arguments: [SyscallFailure.open, .getDescriptorFlags, .setDescriptorFlags])
    func openFailuresAreTypedAndCloseOwnedDescriptorOnce(failure: SyscallFailure) {
        let syscalls = FakeDarwinSerialSyscalls(failure: failure)
        let operations = DarwinSerialSystemOperations(syscalls: syscalls)
        switch operations.open(path: "/dev/cu.fake", flags: USBSession.openFlags) {
        case .value:
            Issue.record("Expected failure")
        case .failure(let code):
            #expect(code == EIO)
        }
        #expect(syscalls.closeCount == (failure == .open ? 0 : 1))
    }

    @Test(arguments: [SyscallFailure.getAttributes, .setAttributes])
    func attributeFailuresAreTyped(failure: SyscallFailure) {
        let syscalls = FakeDarwinSerialSyscalls(failure: failure)
        let operations = DarwinSerialSystemOperations(syscalls: syscalls)
        switch operations.configureRaw(fileDescriptor: 42) {
        case .value:
            Issue.record("Expected failure")
        case .failure(let code):
            #expect(code == EIO)
        }
        #expect(syscalls.closeCount == 0)
    }

    @Test func productionAdapterUsesExactFlagsRawModeAndPreservesSpeed() throws {
        let master = posix_openpt(O_RDWR | O_NOCTTY)
        #expect(master >= 0)
        guard master >= 0 else { return }
        defer { _ = Darwin.close(master) }
        #expect(grantpt(master) == 0)
        #expect(unlockpt(master) == 0)
        guard let pointer = ptsname(master) else {
            Issue.record("Missing pseudo-terminal path")
            return
        }
        let path = String(cString: pointer)
        let operations = DarwinSerialSystemOperations()
        let fd: Int32
        switch operations.open(path: path, flags: USBSession.openFlags) {
        case .value(let value): fd = value
        case .failure(let error):
            Issue.record("Open failed: \(error)")
            return
        }
        defer { _ = operations.close(fileDescriptor: fd) }

        #expect(fcntl(fd, F_GETFD) & FD_CLOEXEC != 0)
        #expect(fcntl(fd, F_GETFL) & O_NONBLOCK != 0)

        var before = termios()
        #expect(tcgetattr(fd, &before) == 0)
        #expect(cfsetispeed(&before, speed_t(B19200)) == 0)
        #expect(cfsetospeed(&before, speed_t(B19200)) == 0)
        #expect(tcsetattr(fd, TCSANOW, &before) == 0)

        switch operations.configureRaw(fileDescriptor: fd) {
        case .value: break
        case .failure(let error):
            Issue.record("Configure failed: \(error)")
            return
        }

        var after = termios()
        #expect(tcgetattr(fd, &after) == 0)
        #expect(after.c_cflag & tcflag_t(CLOCAL | CREAD) == tcflag_t(CLOCAL | CREAD))
        #expect(after.c_lflag & tcflag_t(ICANON | ECHO) == 0)
        #expect(cfgetispeed(&after) == speed_t(B19200))
        #expect(cfgetospeed(&after) == speed_t(B19200))
    }
}

enum SyscallFailure: Equatable, Sendable {
    case open
    case getDescriptorFlags
    case setDescriptorFlags
    case getAttributes
    case setAttributes
}

private final class FakeDarwinSerialSyscalls: DarwinSerialSyscallOperating, @unchecked Sendable {
    private let lock = NSLock()
    private let failure: SyscallFailure
    private var _closeCount = 0

    init(failure: SyscallFailure) { self.failure = failure }

    var closeCount: Int { lock.withLock { _closeCount } }

    func open(path: String, flags: Int32) -> SerialIOResult<Int32> {
        failure == .open ? .failure(EIO) : .value(42)
    }

    func getDescriptorFlags(fileDescriptor: Int32) -> SerialIOResult<Int32> {
        failure == .getDescriptorFlags ? .failure(EIO) : .value(0)
    }

    func setDescriptorFlags(fileDescriptor: Int32, flags: Int32) -> SerialIOResult<Void> {
        failure == .setDescriptorFlags ? .failure(EIO) : .value(())
    }

    func getAttributes(fileDescriptor: Int32) -> SerialIOResult<termios> {
        failure == .getAttributes ? .failure(EIO) : .value(termios())
    }

    func setAttributes(fileDescriptor: Int32, attributes: termios) -> SerialIOResult<Void> {
        failure == .setAttributes ? .failure(EIO) : .value(())
    }

    func close(fileDescriptor: Int32) -> SerialIOResult<Void> {
        lock.withLock { _closeCount += 1 }
        return .value(())
    }
}
