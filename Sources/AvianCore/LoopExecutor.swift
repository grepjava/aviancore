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
//
// Everything else on the loop waits while a job runs, so a job that runs long
// without waiting -- a task doing CPU work inline -- holds up every socket
// and timer the loop has. Set `slowJobMicros` and a drain times each job and
// tells `onSlowJob` about those that ran past it, by the job's address: for a
// task, the task's own (`address(of:)`), so the loop can say whose it was.
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
    /// fires. `av_bad_sock` when the pipe could not be made: a job from another thread
    /// then waits for the loop's next turn rather than cutting it short.
    public let wakeFD: AvianSocket
    private let wakeWriteFD: AvianSocket

    /// A job that runs at least this many microseconds is reported to
    /// `onSlowJob`. 0, the default, times nothing: a drain then pays one
    /// branch per job. Set on the owner's thread.
    public var slowJobMicros: UInt64 = 0
    /// Called on the owner's thread, after a job that ran at least
    /// `slowJobMicros`, with the job's address and how long it ran in
    /// microseconds. A task's job has the task's own address, the one
    /// `address(of:)` gives for it, and the task may have ended by now: the
    /// address is something to look up, never to follow.
    public var onSlowJob: ((UnsafeRawPointer, UInt64) -> Void)? = nil

    /// The address a job of `task` has when it is run here, to recognise it
    /// by in `onSlowJob`.
    public static func address(of task: UnsafeCurrentTask) -> UnsafeRawPointer {
        // A task is the job that runs it: the runtime enqueues the task
        // itself, and both types are a single reference to it.
        unsafeBitCast(task, to: UnsafeRawPointer.self)
    }

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
        var fds: (AvianSocket, AvianSocket) = (av_bad_sock, av_bad_sock)
        let piped = withUnsafeMutableBytes(of: &fds) { raw in
            av_pipe(raw.baseAddress!.assumingMemoryBound(to: AvianSocket.self))
        }
        if piped != 0 { fds = (av_bad_sock, av_bad_sock) }
        wakeFD = fds.0
        wakeWriteFD = fds.1
        if av_sock_ok(wakeFD) == 0 {
            Log.error("no pipe for a loop executor: a task resumed off its loop's thread waits for the next turn of the loop")
        }
    }

    deinit {
        jobs.deallocate()
        if av_sock_ok(wakeFD) != 0 { _ = av_close(wakeFD) }
        if av_sock_ok(wakeWriteFD) != 0 { _ = av_close(wakeWriteFD) }
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
    ///
    /// A task that keeps queueing itself -- one calling `Task.yield()` in a
    /// loop -- keeps this from returning for as long as it does. A loop that
    /// has timers and sockets of its own to see to wants `drain(budget:)`.
    public func drain() {
        _ = drain(budget: .max)
    }

    /// Runs queued jobs, and the jobs they queue, until none is left or
    /// `budget` of them have run, and says whether any are still waiting. A
    /// loop that gets true back has work it must not sleep on: it should poll
    /// without blocking and drain again on its next turn. The owner's thread
    /// only. Called from inside a job it returns at once, with false: the
    /// drain already running picks up whatever that job adds.
    ///
    /// Jobs run in the order they were queued, and a job queued by one that
    /// ran goes to the back, so a task that yields over and over takes its
    /// turn behind the others rather than ahead of them.
    @discardableResult
    public func drain(budget: Int) -> Bool {
        if draining { return false }
        draining = true
        var left = budget
        while left > 0 {
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
            left -= 1
            if slowJobMicros == 0 {
                job.runSynchronously(on: asUnownedTaskExecutor())
            } else {
                runTimed(job)
            }
        }
        draining = false
        return hasWork
    }

    /// Runs a job, and reports it if it ran past `slowJobMicros`.
    private func runTimed(_ job: UnownedJob) {
        let address = unsafeBitCast(job, to: UnsafeRawPointer.self)
        let started = av_monotonic_us()
        job.runSynchronously(on: asUnownedTaskExecutor())
        let ran = av_monotonic_us() &- started
        if ran >= slowJobMicros { onSlowJob?(address, ran) }
    }

    /// Empties the wake pipe. Call it before draining, so that a byte
    /// arriving while the jobs run is not read away with them.
    public func clearWake() {
        guard av_sock_ok(wakeFD) != 0 else { return }
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
        guard first, av_sock_ok(wakeWriteFD) != 0 else { return }
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
