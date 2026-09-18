//===----------------------------------------------------------------------===//
// The broadcast bus: messages read back by number, the oldest overwritten,
// wakes only for armed slots, and a writer that died holding the ring.
//
// The ring is one per process, so these share it and run in order.
//===----------------------------------------------------------------------===//

#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#endif
import Testing
import CAvian

/// The process ID of a process that has certainly finished, for the test that
/// a claim held by a dead writer is taken over.
///
/// Spawned rather than forked: Swift marks `fork()` unavailable on Darwin, and
/// the whole of this file failed to compile there because of this one line --
/// which is to say no test in it had ever run on macOS. Nothing here needs a
/// copy of this process, only a process ID nothing is using, and picking a
/// number instead would be picking one some live process may hold.
private func pidThatHasExited() -> pid_t? {
    for path in ["/usr/bin/true", "/bin/true"] {
        var child: pid_t = 0
        let spawned = path.withCString { executable -> Int32 in
            var argv: [UnsafeMutablePointer<CChar>?] = [strdup(executable), nil]
            defer { free(argv[0]) }
            return posix_spawn(&child, executable, nil, nil, &argv, nil)
        }
        guard spawned == 0 else { continue }
        var status: Int32 = 0
        while waitpid(child, &status, 0) < 0 && errno == EINTR {}
        return child
    }
    return nil
}

private let ringReady: Bool = {
    av_bus_init(1 << 16, 2) == 0 && av_bus_attach(0) >= 0
}()

private func publish(_ topic: String, _ event: String = "", _ data: [UInt8], wakeSelf: Bool = false) -> UInt64 {
    var topic = topic
    var event = event
    return topic.withUTF8 { t in
        event.withUTF8 { e in
            data.withUnsafeBufferPointer { d in
                av_bus_publish(t.baseAddress, UInt32(t.count), e.baseAddress, UInt32(e.count),
                               d.baseAddress, UInt32(d.count), wakeSelf ? 1 : 0)
            }
        }
    }
}

private struct Read {
    var result: Int32
    var topic = ""
    var event = ""
    var data: [UInt8] = []
    var needed = 0
}

private func read(_ sequence: UInt64, capacity: Int = 1 << 16) -> Read {
    var buffer = [UInt8](repeating: 0, count: capacity)
    var message = av_bus_message()
    let rc = buffer.withUnsafeMutableBufferPointer {
        av_bus_read(sequence, $0.baseAddress, UInt32($0.count), &message)
    }
    var out = Read(result: rc)
    if rc == -2 { out.needed = Int(message.data_len) }
    guard rc == 1 else { return out }
    let t = Int(message.topic_len), e = Int(message.event_len), d = Int(message.data_len)
    out.topic = String(decoding: buffer[0..<t], as: UTF8.self)
    out.event = String(decoding: buffer[t..<t + e], as: UTF8.self)
    out.data = Array(buffer[t + e..<t + e + d])
    return out
}

private func readable(_ fd: Int32) -> Bool {
    var p = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
    return poll(&p, 1, 0) == 1
}

@Suite(.serialized)
struct BroadcastBusTests {
    @Test func aMessageIsReadBackByItsNumber() throws {
        try #require(ringReady)
        let before = av_bus_next()
        let sequence = publish("room/1", "said", Array("hello".utf8))
        #expect(sequence == before)
        #expect(av_bus_next() == sequence + 1)
        let message = read(sequence)
        #expect(message.result == 1)
        #expect(message.topic == "room/1")
        #expect(message.event == "said")
        #expect(message.data == Array("hello".utf8))
        #expect(read(sequence + 1).result == 0)
    }

    @Test func numbersStartAtTheTimeTheRingWasMapped() throws {
        try #require(ringReady)
        // Microseconds since the epoch: above anything a restart could have
        // given out before, and far above small counters.
        #expect(av_bus_next() > 1_700_000_000_000_000)
    }

    @Test func aBufferTooSmallSaysHowMuchIsNeeded() throws {
        try #require(ringReady)
        let sequence = publish("t", "e", [UInt8](repeating: 7, count: 100))
        let short = read(sequence, capacity: 10)
        #expect(short.result == -2)
        #expect(short.needed == 102)
        #expect(read(sequence).data.count == 100)
    }

    @Test func aMessageTooLargeIsRefused() throws {
        try #require(ringReady)
        let max = Int(av_bus_max_message())
        #expect(max > 0 && max < 1 << 16)
        // The topic counts: one byte of it, and the rest data.
        #expect(publish("t", "", [UInt8](repeating: 0, count: max - 1)) != 0)
        #expect(publish("t", "", [UInt8](repeating: 0, count: max)) == 0)
    }

    @Test func messagesWrittenOverAreGone() throws {
        try #require(ringReady)
        let first = publish("t", "", Array("first".utf8))
        var last: UInt64 = 0
        for i in 0..<2000 {
            last = publish("t", "", Array("message \(i) of a ring going round".utf8))
        }
        #expect(read(first).result == -1)
        #expect(av_bus_oldest() > first)
        #expect(read(last).result == 1)
        #expect(read(av_bus_oldest()).result == 1)
        #expect(read(av_bus_oldest() - 1).result == -1)
        // Across the end of the ring and back, messages come out whole.
        for sequence in av_bus_oldest()...last {
            let message = read(sequence)
            #expect(message.result == 1)
            let i = 1999 - Int(last - sequence)
            #expect(message.data == Array("message \(i) of a ring going round".utf8))
        }
    }

    @Test func onlyAnArmedSlotIsWoken() throws {
        try #require(ringReady)
        let fd = av_bus_attach(0)
        #expect(!readable(fd))
        _ = publish("t", "", [1], wakeSelf: true)
        #expect(!readable(fd), "not armed")

        #expect(av_bus_arm(av_bus_next()) == 0)
        _ = publish("t", "", [2], wakeSelf: false)
        #expect(!readable(fd), "a publisher on the reading thread is not woken")

        #expect(av_bus_arm(av_bus_next()) == 0)
        _ = publish("t", "", [3], wakeSelf: true)
        #expect(readable(fd))
        _ = publish("t", "", [4], wakeSelf: true)
        av_bus_clear()
        #expect(!readable(fd), "one wake for the burst, and it was disarmed")

        let cursor = av_bus_next()
        _ = publish("t", "", [5], wakeSelf: true)
        #expect(av_bus_arm(cursor) == 1, "published before arming: read now")
    }

    @Test func aWriterThatDiedIsTakenOver() throws {
        try #require(ringReady)
        let child = try #require(pidThatHasExited())

        let before = av_bus_next()
        av_bus_test_abandon(child)
        let sequence = publish("t", "after", Array("the ring still works".utf8))
        #expect(sequence == before)
        let message = read(sequence)
        #expect(message.result == 1)
        #expect(message.event == "after")
        #expect(message.data == Array("the ring still works".utf8))
    }
}
