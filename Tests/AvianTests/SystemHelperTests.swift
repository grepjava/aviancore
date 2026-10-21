//===----------------------------------------------------------------------===//
// The portable helpers the server uses where POSIX and Windows differ in
// spelling: listing a directory, sleeping, and telling a shared region a
// process made from one it inherited.
//===----------------------------------------------------------------------===//

#if canImport(Glibc)
import Glibc
#elseif canImport(Darwin)
import Darwin
#elseif canImport(ucrt)
import ucrt
import WinSDK
#endif
import Testing
import CAvian
import AvianCore

@Suite("System helpers")
struct SystemHelperTests {

    @Test("a directory lists its entries, and a missing one cannot be opened")
    func listsDirectory() throws {
        let dir = try #require(av_dir_open("Sources"))
        defer { av_dir_close(dir) }
        var names = Set<String>()
        while let name = av_dir_next(dir) { names.insert(String(cString: name)) }
        #expect(names.contains("CAvian"))
        #expect(names.contains("AvianCore"))
        #expect(av_dir_open("no-such-directory-here") == nil)
    }

    @Test("a sleep lasts at least as long as asked")
    func sleeps() {
        let start = av_monotonic_us()
        av_sleep_ms(30)
        #expect(av_monotonic_us() - start >= 25_000)
    }

    @Test("a region this process made is not one it inherited")
    func shmMadeHere() throws {
        let p = try #require(av_shm_map(4096))
        defer { av_shm_unmap(p, 4096) }
        #expect(av_shm_attached(p) == 0)
        #expect(p.load(as: UInt64.self) == 0)
    }

    @Test("a process nothing spawned reports no role and no sockets")
    func notSpawned() {
        var role = [CChar](repeating: 0, count: 64)
        #expect(av_spawned(&role, 64) == 0)
        var socks = [AvianSocket](repeating: av_bad_sock, count: 4)
        #expect(av_spawned_sockets(&socks, 4) == 0)
    }
}
