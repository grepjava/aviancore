//===----------------------------------------------------------------------===//
// A task executor for a thread that runs its own event loop.
//
// A server built on a poller already has the one thread it wants its code on:
// the loop's. Swift's default executor would run a task on a pool thread of
// its choosing instead, and hand it back to the loop through a lock on every
// wake. A `LoopExecutor` runs jobs only on its owner's thread, when the loop
// drains it -- so a task that prefers it (`Task(executorPreference:)`) runs
// inline with the loop, in the same turn, until it waits.
//
// A job may still arrive from another thread: the runtime resumes a task
// wherever whatever it waited on was resumed -- its timer thread, a pool
// thread, a thread of a test's. Such a job goes on a list under a lock, and a
// byte down a pipe wakes the loop, whose poller watches `wakeFD`. The owner's
// thread takes the short way: no lock, no syscall.
//
// The owner is whatever `av_worker_set_current` was given on the loop's
// thread. Nothing else about it is used.
//===----------------------------------------------------------------------===//

import CAvian
import Synchronization

@available(macOS 15, *)
public final class LoopExecutor: TaskExecutor, @unchecked Sendable {
    private let owner: UnsafeMutableRawPointer
    private var jobs: UnsafeMutablePointer<UnownedJob>
    /// A power of two, so the ring's indices wrap with a mask.
    private var capacity: Int
    private var head = 0
    /// Jobs queued on the ring, not counting those other threads have left.
    public private(set) var count = 0
    private var draining = false

    /// Jobs other threads have left. `taken` is the empty list swapped in for
    /// them, so collecting costs no allocation once both have grown.
    private let lock: OpaquePointer
    private var intake: [UnownedJob] = []
    private var taken: [UnownedJob] = []
    /// Whether `intake` holds anything, so the loop can ask on every turn
    /// without taking the lock.
    private let handedOver = Atomic<Bool>(false)
    /// The pipe that wakes a loop asleep in its poller: put the read end on
    /// the poller for reading, and call `clearWake` then `drain` when it
    /// fires. -1 when the pipe could not be made: a job from another thread
    /// then waits for the loop's next turn rather than cutting it short.
    public let wakeFD: Int32
    private let wakeWriteFD: Int32

    /// `owner` is the value `av_worker_current()` returns on the loop's
    /// thread. `capacity` is where the ring starts; it grows while jobs are
    /// queued faster than they run.
    public init(owner: UnsafeMutableRawPointer, capacity: Int = 64) {
        self.owner = owner
        var size = 1
        while size < capacity { size <<= 1 }
        self.capacity = size
        jobs = UnsafeMutablePointer<UnownedJob>.allocate(capacity: size)
        // A job from another thread would have nowhere safe to go without it,
        // and a mutex that cannot be allocated means there is no memory left
        // for anything else either.
        guard let lock = av_mutex_new() else {
            preconditionFailure("no lock for a loop executor")
        }
        self.lock = lock
        var fds: (Int32, Int32) = (-1, -1)
        let piped = withUnsafeMutableBytes(of: &fds) { raw in
            av_pipe(raw.baseAddress!.assumingMemoryBound(to: Int32.self))
        }
        if piped != 0 { fds = (-1, -1) }
        wakeFD = fds.0
        wakeWriteFD = fds.1
        if wakeFD < 0 {
            Log.error("no pipe for a loop executor: a task resumed off its loop's thread waits for the next turn of the loop")
        }
    }

    deinit {
        jobs.deallocate()
        if wakeFD >= 0 { _ = av_close(wakeFD) }
        if wakeWriteFD >= 0 { _ = av_close(wakeWriteFD) }
        av_mutex_free(lock)
    }

    public func enqueue(_ job: consuming ExecutorJob) {
        let ready = UnownedJob(job)
        if av_worker_current() == owner {
            push(ready)
        } else {
            handOver(ready)
        }
    }

    /// Whether a drain would run anything. Cheap enough to ask on every turn
    /// of the loop: it reads a count and a flag, not the list.
    public var hasWork: Bool { count > 0 || handedOver.load(ordering: .acquiring) }

    /// Runs queued jobs, and the jobs they queue, until none is left. The
    /// owner's thread only. Called from inside a job it returns at once: the
    /// drain already running picks up whatever that job adds.
    public func drain() {
        if draining { return }
        draining = true
        while true {
            if count == 0 {
                // Including what arrived while this drain was running: a job
                // handed over by the thread that resumed a task here is
                // worth running in the same turn.
                if handedOver.load(ordering: .acquiring) { collect() }
                if count == 0 { break }
            }
            let job = (jobs + head).move()
            head = (head &+ 1) & (capacity &- 1)
            count -= 1
            job.runSynchronously(on: asUnownedTaskExecutor())
        }
        draining = false
    }

    /// Empties the wake pipe. Call it before draining, so that a byte
    /// arriving while the jobs run is not read away with them.
    public func clearWake() {
        guard wakeFD >= 0 else { return }
        var scratch = (UInt64(0), UInt64(0), UInt64(0), UInt64(0),
                       UInt64(0), UInt64(0), UInt64(0), UInt64(0))
        while withUnsafeMutableBytes(of: &scratch, {
            av_read(wakeFD, $0.baseAddress!, $0.count)
        }) > 0 {}
    }

    /// Puts a job on the ring. The owner's thread only.
    private func push(_ job: UnownedJob) {
        if count == capacity { grow() }
        (jobs + ((head &+ count) & (capacity &- 1))).initialize(to: job)
        count += 1
    }

    /// Leaves a job for the loop to run, and wakes it if it is asleep.
    private func handOver(_ job: UnownedJob) {
        av_mutex_lock(lock)
        intake.append(job)
        // The loop takes the whole list when it hears, so only the job that
        // found the list empty has anything to say.
        let first = intake.count == 1
        handedOver.store(true, ordering: .releasing)
        av_mutex_unlock(lock)
        guard first, wakeWriteFD >= 0 else { return }
        var byte: UInt8 = 1
        _ = av_write(wakeWriteFD, &byte, 1)
    }

    /// Moves what other threads left onto the ring. The owner's thread only,
    /// which is what makes `taken` safe to hold outside the lock.
    private func collect() {
        av_mutex_lock(lock)
        swap(&intake, &taken)
        handedOver.store(false, ordering: .releasing)
        av_mutex_unlock(lock)
        for job in taken { push(job) }
        taken.removeAll(keepingCapacity: true)
    }

    /// Only while jobs are queued faster than they run, so the ring stops
    /// growing once the loop is warm.
    private func grow() {
        let larger = UnsafeMutablePointer<UnownedJob>.allocate(capacity: capacity * 2)
        for i in 0..<count {
            (larger + i).initialize(to: (jobs + ((head &+ i) & (capacity &- 1))).move())
        }
        jobs.deallocate()
        jobs = larger
        capacity *= 2
        head = 0
    }
}
