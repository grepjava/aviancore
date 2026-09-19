#ifndef AVIAN_SYS_H
#define AVIAN_SYS_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Readiness poller.
 *
 * epoll on Linux, kqueue on Darwin/BSD, presented as one flat API returning a
 * plain array of (token, mask) pairs. Doing the translation here keeps Swift
 * away from `struct epoll_event` (packed on x86-64) and `union epoll_data`.
 *
 * The poller is *level triggered* on purpose: an event not acted on in one pass
 * is reported again on the next wait rather than lost, so nothing has to
 * remember that it owes a read. The price is that interest in bytes nobody will
 * consume has to be dropped, or the wait spins.
 * ------------------------------------------------------------------------- */

#define AV_POLL_READ   0x1u
#define AV_POLL_WRITE  0x2u
#define AV_POLL_ERR    0x4u
#define AV_POLL_HUP    0x8u

typedef struct {
    uint64_t token;
    uint32_t mask;
    uint32_t _pad;
} av_event;

int  av_poll_create(void);
int  av_poll_add(int pfd, int fd, uint32_t mask, uint64_t token);
int  av_poll_mod(int pfd, int fd, uint32_t mask, uint64_t token);
int  av_poll_del(int pfd, int fd, uint32_t last_mask);
/* Adds a descriptor that several pollers watch -- a listener every worker
 * shares -- so that the kernel wakes one waiting poller for it rather than all
 * of them (EPOLLEXCLUSIVE). Only an idle worker is waiting, so a connection
 * goes to a worker with time for it. It cannot be modified afterwards, only
 * removed and added again. On kqueue this is a plain add. */
int  av_poll_add_exclusive(int pfd, int fd, uint32_t mask, uint64_t token);
/* Returns number of events, or -1 with errno (EINTR is reported as 0). */
int  av_poll_wait(int pfd, av_event *out, int max_events, int timeout_ms);

/* ---------------------------------------------------------------------------
 * Sockets
 * ------------------------------------------------------------------------- */

/* Bind + listen. `host` may be NULL/"" (any), an IPv4/IPv6 literal or a name.
 * Returns fd or -1 (errno set). The socket is non-blocking and, when
 * `reuseport` is set, carries SO_REUSEPORT so N worker processes can each own
 * an independent accept queue -- this is what removes the thundering herd and
 * the shared-accept-lock from the multi-process story. */
int av_listen_tcp(const char *host, uint16_t port, int backlog, int reuseport, int v6only);
/* A unix socket cannot be opened twice: binding requires the path to be free,
 * so `unlink_existing` removes a stale one. With several workers the listener
 * is therefore created once by the supervisor and inherited across fork --
 * letting each worker bind for itself would have every worker unlink and
 * replace the socket the previous one just published. */
int av_listen_unix(const char *path, int backlog, int unlink_existing);

/* accept4() where available, accept()+fcntl() elsewhere. Fills `peer` with a
 * printable address and `peer_port`. Returns fd, or -1 with errno. */
int av_accept(int lfd, char *peer, size_t peer_len, uint16_t *peer_port);

/* Handing a connection to another worker process. A datagram unix socket pair,
 * non-blocking and close-on-exec: each message is one descriptor, passed with
 * SCM_RIGHTS, and a few bytes saying what the sender knew about it. Datagrams
 * keep the two together, and a descriptor in flight belongs to the socket, not
 * to either process, so it survives the receiver being replaced. */
int av_handoff_pair(int fds[2]);
/* Sends `fd` with `meta`. Bytes sent, or -1 with errno (EAGAIN when the
 * receiver is behind). The caller still owns and must close its own copy. */
long av_send_fd(int chan, int fd, const void *meta, size_t meta_len);
/* Receives one message: its bytes into `meta`, the descriptor into `*fd`, which
 * is -1 when none came. Bytes received, or -1 with errno. */
long av_recv_fd(int chan, int *fd, void *meta, size_t cap);

/* Starts a TCP connection and returns at once. The socket is non-blocking and
 * close-on-exec, so the caller waits for writability and then asks
 * `av_connect_error` how it went.
 *
 * `host` must be an IPv4 or IPv6 literal: this resolves nothing, because
 * getaddrinfo blocks, and blocking a worker is the one thing an outbound
 * connection on the event loop exists to avoid. A name is EINVAL, and naming
 * is a layer above this one.
 *
 * Returns fd with *in_progress set to 1 when the connection is still being
 * made -- the usual case -- or 0 when it completed immediately, which happens
 * on loopback. Returns -1 with errno on a real failure. */
int av_connect_tcp(const char *host, uint16_t port, int *in_progress);

/* The same for a unix socket, which also usually completes at once. */
int av_connect_unix(const char *path, int *in_progress);

/* SO_ERROR: 0 when the connection is up, otherwise the errno that stopped it.
 * A non-blocking connect reports its outcome here and nowhere else -- the
 * socket simply becomes writable either way. */
int av_connect_error(int fd);

int av_set_nonblock(int fd);
int av_set_nodelay(int fd, int on);
int av_set_cloexec(int fd);
int av_shutdown_write(int fd);
int av_close(int fd);

/* Local address of a listening socket, for reporting which address a request arrived on. */
int av_local_addr(int fd, char *host, size_t host_len, uint16_t *port);

ssize_t av_read(int fd, void *buf, size_t n);
ssize_t av_write(int fd, const void *buf, size_t n);
ssize_t av_writev(int fd, const struct iovec *iov, int iovcnt);
/* Portable sendfile(); advances *offset. Returns bytes sent or -1. */
ssize_t av_sendfile(int out_fd, int in_fd, off_t *offset, size_t count);

/* Opens a regular file under `root` for a static route, reporting its size and
 * modification time in nanoseconds since the epoch.
 *
 * `relative` is the request path with the route prefix removed and already
 * percent-decoded. Both paths are resolved with realpath(3) and the result must
 * still lie inside the resolved root, which is what stops `..` and a symlink
 * pointing out of the tree from reaching anything. Only regular files open:
 * a directory, a fifo or a device is refused rather than served.
 *
 * Returns the descriptor, or -1. */
int av_static_open(const char *root, const char *relative,
                   long long *size, long long *mtime);

/* Opens an absolute path read-only, close-on-exec, for a small system file the
 * server reads whole at start-up -- /etc/resolv.conf and nothing larger.
 *
 * Deliberately not av_static_open, which resolves a relative path against a
 * root and refuses anything outside it. That is what serving files needs and
 * the opposite of what this needs. Only regular files open, so a path that has
 * been swapped for a fifo cannot make start-up block for ever.
 *
 * Returns the descriptor, or -1 with errno set. */
int av_open_read(const char *path);

/* A datagram socket connected to `host` and `port`, both numeric.
 *
 * Connected rather than bare, so the kernel refuses datagrams from anyone but
 * the nameserver that was asked: an off-path answer has to guess the query id
 * and the source port, and this takes the port away as something to guess.
 *
 * There is no in_progress here, unlike av_connect_tcp. connect(2) on a
 * datagram socket only records the peer, so it returns at once or not at all.
 *
 * Returns the descriptor, or -1 with errno set; EINVAL means the host was not
 * an address literal. */
int av_connect_udp(const char *host, uint16_t port);

/* The port a socket is actually bound to, which for a bind to port 0 is the
 * one the kernel chose. Returns 0 on failure.
 *
 * Without this a test that needs a server of its own has to pick a number and
 * hope nothing else on the machine holds it. The outbound tests avoided the
 * problem by using unix sockets; a nameserver cannot be one. */
uint16_t av_local_port(int fd);

/* poll(2) on a single descriptor, for a wait outside the readiness poller: the
 * supervisor checking whether a replacement worker has reported ready. */
int av_poll_single(int fd, int for_write, int timeout_ms);

int av_errno(void);
void av_set_errno(int e);
const char *av_strerror(int e);
int av_err_is_again(int e);      /* EAGAIN / EWOULDBLOCK */
int av_err_is_intr(int e);       /* EINTR */

/* ---------------------------------------------------------------------------
 * Time
 * ------------------------------------------------------------------------- */
uint64_t av_monotonic_ms(void);
/* Precise monotonic microseconds. Unlike av_monotonic_ms this never reads a
 * coarse clock: it times a single request, where the coarse clock's few
 * milliseconds of slack would be the whole measurement. */
uint64_t av_monotonic_us(void);
/* Wall-clock microseconds since the Unix epoch. For timestamps another system
 * compares against its own clock; intervals belong to av_monotonic_us. */
uint64_t av_realtime_us(void);

/* Asks the kernel to timestamp data as it arrives on `fd`. Set on a listening
 * socket, it is inherited by every connection accepted from it -- and it has
 * to be set there, before the connection exists, or the first request's
 * packets arrive with no timestamp to report. 0 on success. */
int av_set_rx_timestamps(int fd);

/* read() through recvmsg, also reporting when the kernel received the last
 * of the bytes returned, as wall-clock microseconds. `*arrived_us` is 0 when
 * the kernel recorded nothing (no timestamping, or a platform without it). */
long av_read_stamped(int fd, void *buf, size_t n, uint64_t *arrived_us);
/* IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT". Writes exactly 29 bytes,
 * no NUL. Returns 29. Hand-rolled: strftime() would pull in locale state. */
int av_http_date(char *buf29, int64_t unix_seconds);
int64_t av_unix_seconds(void);

/* ---------------------------------------------------------------------------
 * Process control / signals
 *
 * Signals are funnelled into a self-pipe so the readiness poller is the single
 * place the server ever blocks.
 * ------------------------------------------------------------------------- */
int  av_signal_pipe_init(void);   /* returns readable fd, -1 on failure */
/* fork() for a worker: signals are held across it, and the child gets a signal
 * pipe of its own before they are released, so none sent during start-up is
 * lost. */
pid_t av_fork_worker(void);
/* In a child that is not a worker: default signal dispositions, no pipe. */
void av_signals_default(void);
pid_t av_fork(void);
/* Sets the kernel's short process name (Linux; a no-op elsewhere). */
void av_set_process_name(const char *name);
pid_t av_waitpid(pid_t pid, int *status, int nohang);
int  av_kill(pid_t pid, int sig);
pid_t av_getpid(void);
int  av_cpu_count(void);
/* Raise RLIMIT_NOFILE to its hard limit; returns the resulting soft limit. */
long av_raise_nofile_limit(void);

/* Asks the scheduler to run the calling thread in slices of `slice_ns`
 * nanoseconds rather than its default (EEVDF's custom slice, Linux 6.12 and
 * later; the kernel accepts 100 us to 100 ms). A thread that owns many
 * connections and loses its CPU to another runnable thread keeps all of them
 * waiting until it gets it back, for up to a whole slice -- 2.8 ms by default
 * on an 8-CPU machine. A shorter slice brings it back sooner. Only a thread
 * under the normal or batch policy is changed; its nice value is kept.
 * 0, or -1 with errno: ENOSYS where there is no sched_setattr. An older kernel
 * accepts the call and ignores the slice. */
int  av_sched_set_slice(uint64_t slice_ns);
/* The calling thread's slice as the kernel records it, in nanoseconds: what
 * av_sched_set_slice asked for, or 0 when the kernel ignores custom slices or
 * none was asked for. -1 with errno where it cannot be read. */
int64_t av_sched_slice(void);
/* Arms a SIGALRM that _exit()s the process after `seconds`, so a shutdown
 * that wedges anywhere still terminates. 0 seconds disarms. */
void av_exit_after(unsigned seconds, int code);
void av_cancel_exit_timer(void);

/* Ignore SIGPIPE: a peer that vanishes mid-response must surface as EPIPE from
 * write(), never as a process-killing signal. */
void av_ignore_sigpipe(void);

/* ---------------------------------------------------------------------------
 * Addresses, files, environment
 * ------------------------------------------------------------------------- */

/* inet_pton for both families. Writes 16 bytes (IPv4 left-aligned in the first
 * four) and reports 4 or 6 in *family. Returns 0 on success. */
int av_parse_ip(const char *s, unsigned char out16[16], int *family);

int av_unlink(const char *path);
const char *av_getenv(const char *name);
int av_path_exists(const char *path);
/* Modification time in nanoseconds, or -1. Used by --reload. */
int64_t av_mtime_ns(const char *path);
int av_is_dir(const char *path);
/* Non-blocking, close-on-exec pipe. */
int av_pipe(int fds[2]);

/* --reload, restarting the supervisor on a rebuilt executable. */
/* The running executable's absolute path. 0, or -1 where it cannot be found. */
int av_executable_path(char *out, size_t cap);
/* A digest of what a file is on disk -- device, inode, size, mode and
 * modification time -- or 0 when it does not exist or is not an executable
 * regular file (`executable`) or a regular file at all. */
uint64_t av_file_signature(const char *path, int executable);
/* Runs `path --version` with its output discarded, waiting up to timeout_ms.
 * 1 when it exited 0, which says the file is a whole executable that starts. */
int av_probe_executable(const char *path, int timeout_ms);
int av_clear_cloexec(int fd);
/* execv. Returns only on failure, with errno set. */
int av_execv(const char *path, char *const argv[]);
/* Blocks, and unblocks, the signals the supervisor's pipe carries. The mask
 * survives exec, so a signal arriving before the new image has handlers waits
 * for them instead of taking its default action. */
void av_block_piped_signals(void);
void av_unblock_piped_signals(void);
int av_random_bytes(void *out, size_t n);

/* ---------------------------------------------------------------------------
 * The current worker
 * ------------------------------------------------------------------------- */

/* Where this process's Worker lives. There is one worker per process; the
 * storage is a thread-local, which reads as a register-relative load with no
 * lock. */
typedef struct av_mutex av_mutex;
typedef struct av_cond av_cond;

av_mutex *av_mutex_new(void);
void av_mutex_free(av_mutex *m);
void av_mutex_lock(av_mutex *m);
void av_mutex_unlock(av_mutex *m);

av_cond *av_cond_new(void);
void av_cond_free(av_cond *c);
void av_cond_wait(av_cond *c, av_mutex *m);
void av_cond_signal(av_cond *c);
void av_cond_broadcast(av_cond *c);

/* Starts a detached thread with every signal blocked. Returns 0 on success. */
int av_thread_spawn(void (*fn)(void *), void *arg);

/* The same, joinable, for threads whose exit the caller has to observe, such as a
 * supervisor that must know every worker has let go of its connections before
 * the application is allowed to shut down. Returns
 * NULL on failure; the handle is freed by av_thread_join. */
typedef struct av_thread av_thread;
av_thread *av_thread_start(void (*fn)(void *), void *arg);
void av_thread_join(av_thread *t);

/* Where the current thread's Worker lives.
 *
 * The server used to have exactly one worker per process, so this was a plain
 * global. With several worker threads in one process there is one per thread,
 * and C callbacks that need it are handed nothing but a connection token, so they have to find it themselves. A thread-local is
 * the cheapest way to answer that: a register-relative load, no lock, and no
 * change to any call site. */
void *av_worker_current(void);
void av_worker_set_current(void *worker);

/* ---------------------------------------------------------------------------
 * WebSocket handshake primitives
 * ------------------------------------------------------------------------- */

void av_sha1(const void *data, size_t n, unsigned char out20[20]);
/* Writes 4*ceil(n/3) bytes, no NUL. Returns the number written. */
size_t av_base64(const void *data, size_t n, char *out);

#ifdef __cplusplus
}
#endif
#endif
