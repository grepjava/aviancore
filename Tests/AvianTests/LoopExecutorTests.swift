//===----------------------------------------------------------------------===//
// LoopExecutor: jobs run only when the loop drains, in order, on the owner's
// thread; a job from another thread is handed over and wakes the pipe; a
// budgeted drain stops at its budget however many jobs the jobs queue; a
// timed drain reports the jobs that ran long, by their task's address.
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
    var addresses: [UnsafeRawPointer] = []
    var slow: [(address: UnsafeRawPointer, micros: UInt64)] = []
}

/// Runs on the calling thread, without waiting, for `micros`.
private func spin(_ micros: UInt64) {
    let until = av_monotonic_us() &+ micros
    while av_monotonic_us() < until {}
}

/// Queues a task on `executor` that records `step` and queues the next, up to
/// `last`: work that keeps adding to the queue it is drained from, the way a
/// task calling `Task.yield()` in a loop does.
@available(macOS 15, *)
private func chain(_ step: Int, upTo last: Int, on executor: LoopExecutor, _ trail: Trail) {
    Task(executorPreference: executor) {
        trail.steps.append(step)
        if step < last { chain(step + 1, upTo: last, on: executor, trail) }
    }
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

    @available(macOS 15, *)
    @Test func aBudgetedDrainStopsAtItsBudgetAndSaysWorkIsLeft() {
        let owner = UnsafeMutableRawPointer.allocate(byteCount: 1, alignment: 1)
        defer { owner.deallocate() }
        let executor = LoopExecutor(owner: owner)
        let trail = Trail()
        asOwner(owner) {
            chain(0, upTo: 99, on: executor, trail)
            // An unbounded drain would never come back from work like this
            // while it kept going; a budgeted one hands the loop back.
            #expect(executor.drain(budget: 10))
            #expect(trail.steps == Array(0..<10))
            #expect(executor.hasWork)
            var drains = 1
            while true {
                drains += 1
                if !executor.drain(budget: 10) { break }
            }
            // The tenth runs the last ten and finds nothing after them.
            #expect(drains == 10)
        }
        #expect(trail.steps == Array(0..<100))
        #expect(!executor.hasWork)
    }

    @available(macOS 15, *)
    @Test func aBudgetedDrainLeavesAJobThatRequeuesBehindTheOthers() {
        let owner = UnsafeMutableRawPointer.allocate(byteCount: 1, alignment: 1)
        defer { owner.deallocate() }
        let executor = LoopExecutor(owner: owner)
        let trail = Trail()
        asOwner(owner) {
            // 1000, 1001, ... keeps queueing itself; 0..<5 were queued once.
            chain(1000, upTo: 2000, on: executor, trail)
            for i in 0..<5 {
                Task(executorPreference: executor) { trail.steps.append(i) }
            }
            #expect(executor.drain(budget: 6))
            // The first of the chain, then everything queued before its
            // second step: the chain cannot cut ahead.
            #expect(trail.steps == [1000, 0, 1, 2, 3, 4])
            while executor.drain(budget: 64) {}
        }
        #expect(trail.steps.count == 1006)
    }

    @available(macOS 15, *)
    @Test func aBudgetedDrainTakesInJobsHandedOverWhileItRuns() {
        let owner = UnsafeMutableRawPointer.allocate(byteCount: 1, alignment: 1)
        defer { owner.deallocate() }
        let executor = LoopExecutor(owner: owner)
        let trail = Trail()
        // From a thread that is nobody's loop, so both are handed over.
        Task(executorPreference: executor) { trail.steps.append(1) }
        Task(executorPreference: executor) { trail.steps.append(2) }
        asOwner(owner) {
            executor.clearWake()
            #expect(!executor.drain(budget: 8))
        }
        #expect(Set(trail.steps) == [1, 2])
        #expect(!executor.hasWork)
    }

    @available(macOS 15, *)
    @Test func aTimedDrainReportsTheJobsThatRanLongByTheirTask() {
        let owner = UnsafeMutableRawPointer.allocate(byteCount: 1, alignment: 1)
        defer { owner.deallocate() }
        let executor = LoopExecutor(owner: owner)
        let trail = Trail()
        executor.slowJobMicros = 25_000
        executor.onSlowJob = { address, micros in trail.slow.append((address, micros)) }
        asOwner(owner) {
            // One quick, one slow, and one slow only after it has waited.
            Task(executorPreference: executor) {
                withUnsafeCurrentTask { trail.addresses.append(LoopExecutor.address(of: $0!)) }
            }
            Task(executorPreference: executor) {
                withUnsafeCurrentTask { trail.addresses.append(LoopExecutor.address(of: $0!)) }
                spin(60_000)
            }
            Task(executorPreference: executor) {
                withUnsafeCurrentTask { trail.addresses.append(LoopExecutor.address(of: $0!)) }
                _ = await withUnsafeContinuation { trail.parked = $0 }
                spin(60_000)
            }
            executor.drain()
            #expect(trail.addresses.count == 3)
            #expect(trail.slow.map(\.address) == [trail.addresses[1]])
            #expect(trail.slow.allSatisfy { $0.micros >= 60_000 })
            // Its resumption is a job of the same task, at the same address.
            trail.parked?.resume(returning: 0)
            executor.drain()
        }
        #expect(trail.slow.map(\.address) == [trail.addresses[1], trail.addresses[2]])
    }

    @available(macOS 15, *)
    @Test func anUntimedDrainReportsNothing() {
        let owner = UnsafeMutableRawPointer.allocate(byteCount: 1, alignment: 1)
        defer { owner.deallocate() }
        let executor = LoopExecutor(owner: owner)
        let trail = Trail()
        executor.onSlowJob = { address, micros in trail.slow.append((address, micros)) }
        asOwner(owner) {
            Task(executorPreference: executor) { spin(10_000) }
            executor.drain()
        }
        #expect(trail.slow.isEmpty)
    }
}
