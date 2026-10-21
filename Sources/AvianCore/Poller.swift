//===----------------------------------------------------------------------===//
// Readiness poller.
//
// A thin, allocation-free wrapper over the C epoll/kqueue shim. The event array
// is owned by the poller and reused for the life of the worker, so a poll cycle
// costs exactly one syscall and zero allocations.
//===----------------------------------------------------------------------===//

import CAvian

/// A socket as aviancore's C layer takes it: an `Int32` file descriptor on
/// Linux and Darwin, a pointer-sized `SOCKET` on Windows.
public typealias AvianSocket = av_sock
/// A readiness poller: an epoll or kqueue descriptor, or wepoll's `HANDLE` on
/// Windows.
public typealias AvianPollHandle = av_poll_t

/// The value that stands for no socket: -1 on POSIX, `INVALID_SOCKET` on
/// Windows. A `SOCKET` is unsigned, so `fd >= 0` there is always true; test
/// with `isSocket` instead.
@inlinable public var noSocket: AvianSocket { av_bad_sock }

/// Whether `s` is a socket rather than `noSocket`.
@inlinable public func isSocket(_ s: AvianSocket) -> Bool { av_sock_ok(s) != 0 }

public struct PollMask: OptionSet, Sendable {
    public let rawValue: UInt32
    @inlinable public init(rawValue: UInt32) { self.rawValue = rawValue }

    public static let read  = PollMask(rawValue: 0x1)
    public static let write = PollMask(rawValue: 0x2)
    public static let error = PollMask(rawValue: 0x4)
    public static let hangup = PollMask(rawValue: 0x8)

    @inlinable public var wantsRead: Bool { rawValue & 0x1 != 0 }
    @inlinable public var wantsWrite: Bool { rawValue & 0x2 != 0 }
    @inlinable public var isFailed: Bool { rawValue & 0xC != 0 }
}

public struct Poller {
    public let fd: AvianPollHandle
    @usableFromInline let events: UnsafeMutablePointer<av_event>
    @usableFromInline let maxEvents: Int32

    public init?(maxEvents: Int = 256) {
        let f = av_poll_create()
        if av_poll_ok(f) == 0 { return nil }
        self.fd = f
        self.maxEvents = Int32(maxEvents)
        self.events = UnsafeMutablePointer<av_event>.allocate(capacity: maxEvents)
    }

    @inlinable
    @discardableResult
    public func add(_ target: AvianSocket, _ mask: PollMask, token: UInt64) -> Bool {
        av_poll_add(fd, target, mask.rawValue, token) == 0
    }

    /// Adds a descriptor several pollers share, so that the kernel wakes one
    /// waiting poller for it rather than every one. It can only be removed
    /// and added again, never modified. See `av_poll_add_exclusive`.
    @inlinable
    @discardableResult
    public func addExclusive(_ target: AvianSocket, _ mask: PollMask, token: UInt64) -> Bool {
        av_poll_add_exclusive(fd, target, mask.rawValue, token) == 0
    }

    @inlinable
    @discardableResult
    public func modify(_ target: AvianSocket, _ mask: PollMask, token: UInt64) -> Bool {
        av_poll_mod(fd, target, mask.rawValue, token) == 0
    }

    @inlinable
    @discardableResult
    public func remove(_ target: AvianSocket, last: PollMask = [.read, .write]) -> Bool {
        av_poll_del(fd, target, last.rawValue) == 0
    }

    /// Blocks for at most `timeoutMillis` (-1 = forever, 0 = poll). Returns the
    /// number of ready events, or -1 on a hard failure. EINTR yields 0.
    @inlinable
    public func wait(timeoutMillis: Int32) -> Int {
        Int(av_poll_wait(fd, events, maxEvents, timeoutMillis))
    }

    @inlinable
    public func event(_ i: Int) -> (token: UInt64, mask: PollMask) {
        let e = events[i]
        return (e.token, PollMask(rawValue: e.mask))
    }

    /// The array `wait` fills and `event` reads, and its length, for a caller
    /// that collects events on another thread and hands the count over --
    /// asyncio's bridge on Windows, where the loop cannot watch the poller
    /// itself. Only one thread may wait into it at a time.
    public var eventBuffer: UnsafeMutablePointer<av_event> { events }
    public var eventCapacity: Int32 { maxEvents }

    public func destroy() {
        events.deallocate()
        _ = av_poll_close(fd)
    }
}
