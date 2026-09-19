import Testing
import CAvian
@testable import AvianCore

// What workers use to share connections fairly: the load page, a listener
// several pollers watch, and passing a connection from one process to another.

@Suite("Balancing primitives", .serialized)
struct BalancingPrimitiveTests {

    @Test("the load page reports active slots, and discounts a worker that has gone quiet")
    func loadPage() {
        #expect(av_load_init(4) == 0)
        #expect(av_load_enabled() != 0)
        // One page per process: a second call keeps the first.
        #expect(av_load_init(16) == 0)
        #expect(av_load_slots() == 4)
        for slot in 0..<4 { av_load_leave(Int32(slot)) }

        av_load_join(0, 0)
        av_load_join(2, 1)
        av_load_publish(0, 800, 12)
        av_load_publish(2, 5000, 3)          // clamped to 1000
        av_load_join(3, 2)
        av_load_draining(3)                  // not offered anything

        var views = [av_load_view](repeating: av_load_view(), count: 8)
        let now: UInt64 = 1_000_000
        var n = Int(av_load_snapshot(&views, 8, now))
        #expect(n == 2)
        #expect(views[0].slot == 0 && views[0].channel == 0)
        #expect(views[0].busy == 800 && views[0].conns == 12)
        #expect(views[1].slot == 2 && views[1].channel == 1 && views[1].busy == 1000)

        // Waiting for half the quiet period halves the reading; the whole of it
        // clears it; stopping waiting restores it.
        av_load_waiting(0, now - UInt64(AV_LOAD_QUIET_US) / 2)
        n = Int(av_load_snapshot(&views, 8, now))
        #expect(n == 2 && views[0].busy == 400)
        av_load_waiting(0, now - UInt64(AV_LOAD_QUIET_US))
        n = Int(av_load_snapshot(&views, 8, now))
        #expect(views[0].busy == 0)
        av_load_waiting(0, 0)
        n = Int(av_load_snapshot(&views, 8, now))
        #expect(views[0].busy == 800)

        // A capacity smaller than the active count is respected.
        #expect(av_load_snapshot(&views, 1, now) == 1)
        av_load_leave(0)
        av_load_leave(2)
        av_load_leave(3)
        #expect(av_load_snapshot(&views, 8, now) == 0)

        // A supervisor that reaps a worker clears whatever slot it held.
        av_load_join(1, 5)
        #expect(av_load_snapshot(&views, 8, now) == 1)
        av_load_reap(av_getpid() + 1)
        #expect(av_load_snapshot(&views, 8, now) == 1)
        av_load_reap(av_getpid())
        #expect(av_load_snapshot(&views, 8, now) == 0)
    }

    @Test("a descriptor and its note arrive together, and the sender's copy is its own")
    func handoff() throws {
        var chan: (Int32, Int32) = (-1, -1)
        let paired = withUnsafeMutableBytes(of: &chan) {
            av_handoff_pair($0.baseAddress!.assumingMemoryBound(to: Int32.self))
        }
        try #require(paired == 0)
        defer { _ = av_close(chan.0); _ = av_close(chan.1) }

        var pipe: (Int32, Int32) = (-1, -1)
        _ = withUnsafeMutableBytes(of: &pipe) {
            av_pipe($0.baseAddress!.assumingMemoryBound(to: Int32.self))
        }
        defer { _ = av_close(pipe.0) }

        // Nothing waiting yet.
        var got: Int32 = -1
        var note = [UInt8](repeating: 0, count: 64)
        #expect(av_recv_fd(chan.1, &got, &note, 64) < 0)
        #expect(av_err_is_again(av_errno()) != 0)

        let sent: [UInt8] = Array("hello from 3".utf8)
        #expect(av_send_fd(chan.0, pipe.1, sent, sent.count) == sent.count)
        // The sender closes its copy; the one in flight keeps the pipe open.
        _ = av_close(pipe.1)

        let n = av_recv_fd(chan.1, &got, &note, 64)
        #expect(n == sent.count)
        #expect(Array(note.prefix(Int(n))) == sent)
        try #require(got >= 0)
        let byte: [UInt8] = [42]
        #expect(av_write(got, byte, 1) == 1)
        _ = av_close(got)
        var back: UInt8 = 0
        #expect(av_read(pipe.0, &back, 1) == 1)
        #expect(back == 42)
    }

    @Test("a listener can be watched exclusively by several pollers")
    func exclusiveListener() throws {
        let listener = av_listen_tcp("127.0.0.1", 0, 16, 0, 0)
        try #require(listener >= 0)
        defer { _ = av_close(listener) }
        var port: UInt16 = 0
        _ = av_local_addr(listener, nil, 0, &port)

        let a = try #require(Poller(maxEvents: 8))
        let b = try #require(Poller(maxEvents: 8))
        defer { a.destroy(); b.destroy() }
        #expect(a.addExclusive(listener, .read, token: 7))
        #expect(b.addExclusive(listener, .read, token: 7))

        var progress: Int32 = 0
        let client = av_connect_tcp("127.0.0.1", port, &progress)
        try #require(client >= 0)
        defer { _ = av_close(client) }

        // Whichever poller hears of it can accept it.
        var accepted: Int32 = -1
        for _ in 0..<100 where accepted < 0 {
            for poller in [a, b] where poller.wait(timeoutMillis: 10) > 0 {
                #expect(poller.event(0).token == 7)
                accepted = av_accept(listener, nil, 0, nil)
                if accepted >= 0 { break }
            }
        }
        #expect(accepted >= 0)
        if accepted >= 0 { _ = av_close(accepted) }
        // Removing and adding again is how it is re-armed.
        #expect(a.remove(listener, last: .read))
        #expect(a.addExclusive(listener, .read, token: 7))
    }
}
