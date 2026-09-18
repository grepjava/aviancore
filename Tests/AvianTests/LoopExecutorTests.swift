//===----------------------------------------------------------------------===//
// LoopExecutor: jobs run only when the loop drains, in order, on the owner's
// thread; a job from another thread is handed over and wakes the pipe.
//===----------------------------------------------------------------------===//

import Testing
import CAvian
import AvianCore

/// What the tasks under test did, in order. Only ever touched on the thread
/// running the test: the tasks run when it drains.
private final class Trail: @unchecked Sendable {
    var steps: [Int] = []
    var threads: [UnsafeMutableRawPointer?] = []
    var parked: UnsafeContinuation<Int, Never>? = nil
}

@Suite struct LoopExecutorTests {
    /// Runs `body` as the loop's thread for `owner`, and stops being it after.
    private func asOwner<R>(_ owner: UnsafeMutableRawPointer, _ body: () throws -> R) rethrows -> R {
        let before = av_worker_current()
        av_worker_set_current(owner)
        defer { av_worker_set_current(before) }
        return try body()
    }

    private func pipeHasByte(_ fd: Int32) -> Bool {
        av_poll_single(fd, 0, 0) != 0
    }

    @available(macOS 15, *)
    @Test func aTaskRunsOnlyWhenTheLoopDrains() {
        let owner = UnsafeMutableRawPointer.allocate(byteCount: 1, alignment: 1)
        defer { owner.deallocate() }
        let executor = LoopExecutor(owner: owner)
        let trail = Trail()
        asOwner(owner) {
            Task(executorPreference: executor) {
                trail.steps.append(1)
                trail.threads.append(av_worker_current())
            }
            // Queued on the ring, and nothing run yet.
            #expect(executor.hasWork)
            #expect(trail.steps.isEmpty)
            // The owner's own job needs no wake.
            if executor.wakeFD >= 0 { #expect(!pipeHasByte(executor.wakeFD)) }
            executor.drain()
        }
        #expect(trail.steps == [1])
        #expect(trail.threads == [owner])
        #expect(!executor.hasWork)
    }

    @available(macOS 15, *)
    @Test func jobsRunInTheOrderTheyCameAndTheRingGrows() {
        let owner = UnsafeMutableRawPointer.allocate(byteCount: 1, alignment: 1)
        defer { owner.deallocate() }
        // Smaller than the jobs queued, so the ring has to grow while some
        // are waiting in it.
        let executor = LoopExecutor(owner: owner, capacity: 4)
        let trail = Trail()
        asOwner(owner) {
            for i in 0..<100 {
                Task(executorPreference: executor) { trail.steps.append(i) }
            }
            #expect(executor.count == 100)
            executor.drain()
        }
        #expect(trail.steps == Array(0..<100))
    }

    @available(macOS 15, *)
    @Test func aJobFromAnotherThreadIsHandedOverAndWakesTheLoop() {
        let owner = UnsafeMutableRawPointer.allocate(byteCount: 1, alignment: 1)
        defer { owner.deallocate() }
        let executor = LoopExecutor(owner: owner)
        let trail = Trail()
        // Not the owner's thread: this thread is nobody's loop.
        Task(executorPreference: executor) {
            trail.steps.append(1)
            trail.threads.append(av_worker_current())
        }
        #expect(executor.hasWork)
        #expect(executor.count == 0)
        #expect(trail.steps.isEmpty)
        if executor.wakeFD >= 0 {
            #expect(pipeHasByte(executor.wakeFD))
            executor.clearWake()
            #expect(!pipeHasByte(executor.wakeFD))
        }
        asOwner(owner) { executor.drain() }
        #expect(trail.steps == [1])
        #expect(trail.threads == [owner])
        #expect(!executor.hasWork)
    }

    @available(macOS 15, *)
    @Test func aTaskThatWaitsIsResumedOnTheLoop() {
        let owner = UnsafeMutableRawPointer.allocate(byteCount: 1, alignment: 1)
        defer { owner.deallocate() }
        let executor = LoopExecutor(owner: owner)
        let trail = Trail()
        asOwner(owner) {
            Task(executorPreference: executor) {
                let value = await withUnsafeContinuation { trail.parked = $0 }
                trail.steps.append(value)
                trail.threads.append(av_worker_current())
            }
            executor.drain()
        }
        // Waiting, and nothing more to run until it is resumed.
        #expect(trail.steps.isEmpty)
        #expect(!executor.hasWork)
        // Resumed from a thread that is not the loop's: the job is handed
        // over rather than run here.
        trail.parked?.resume(returning: 7)
        #expect(executor.hasWork)
        #expect(trail.steps.isEmpty)
        asOwner(owner) {
            executor.clearWake()
            executor.drain()
        }
        #expect(trail.steps == [7])
        #expect(trail.threads == [owner])
    }
}
