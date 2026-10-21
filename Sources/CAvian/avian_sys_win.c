/* The Windows implementation of avian_sys.h, and of the POSIX-only parts of
 * avian_extra.c. avian_sys.c is the POSIX one; the comments in avian_sys.h say
 * what each call promises, and this file keeps those promises with Windows'
 * own calls rather than restating them.
 *
 * What differs in kind rather than in spelling:
 *
 *   - The poller is wepoll (vendor/wepoll): epoll's API over the AFD driver.
 *     It watches sockets only, so every descriptor the server polls is a
 *     socket here -- the pipes included, which are loopback socket pairs.
 *   - Winsock reports through WSAGetLastError, not errno. Every socket call
 *     below translates its error into the errno value the POSIX build would
 *     have set, so av_errno, av_err_is_again and the callers' errno checks
 *     work unchanged.
 *   - Sockets are created non-inheritable, which is close-on-exec here.
 *   - Signals are console control events. A handler writes the matching signal
 *     number into the signal socket, the same byte a POSIX handler writes.
 *   - There is no fork. A worker process is a new copy of the program
 *     (av_spawn_self) that inherits the sockets and shared regions it is
 *     handed and hears its signals over a control socket. fork, exec and
 *     passing a descriptor over a socket fail with ENOSYS.
 */

#if defined(_WIN32)

#include "avian_sys.h"

#include <afunix.h>
#include <bcrypt.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <mstcpip.h>
#include <process.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/wepoll/wepoll.h"

/* ======================================================================== */
/* Winsock start-up and errors                                              */
/* ======================================================================== */

static INIT_ONCE g_net_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK net_start(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

/* Every call that makes a socket goes through here first. */
static void net_init(void) { InitOnceExecuteOnce(&g_net_once, net_start, NULL, NULL); }
void av_net_init(void) { net_init(); }

static int errno_from_wsa(int e) {
    switch (e) {
    case WSAEWOULDBLOCK:     return EAGAIN;
    case WSAEINPROGRESS:     return EINPROGRESS;
    case WSAEALREADY:        return EALREADY;
    case WSAEINTR:           return EINTR;
    case WSAEBADF:
    case WSAENOTSOCK:        return EBADF;
    case WSAEACCES:          return EACCES;
    case WSAEFAULT:          return EFAULT;
    case WSAEINVAL:          return EINVAL;
    case WSAEMFILE:          return EMFILE;
    case WSAEMSGSIZE:        return EMSGSIZE;
    case WSAENOPROTOOPT:     return ENOPROTOOPT;
    case WSAEPROTONOSUPPORT: return EPROTONOSUPPORT;
    case WSAEOPNOTSUPP:      return EOPNOTSUPP;
    case WSAEAFNOSUPPORT:    return EAFNOSUPPORT;
    case WSAEADDRINUSE:      return EADDRINUSE;
    case WSAEADDRNOTAVAIL:   return EADDRNOTAVAIL;
    case WSAENETDOWN:        return ENETDOWN;
    case WSAENETUNREACH:     return ENETUNREACH;
    case WSAENETRESET:       return ENETRESET;
    case WSAECONNABORTED:    return ECONNABORTED;
    case WSAECONNRESET:      return ECONNRESET;
    case WSAENOBUFS:         return ENOBUFS;
    case WSAEISCONN:         return EISCONN;
    case WSAENOTCONN:        return ENOTCONN;
    /* A send on a socket shut down for writing: POSIX says EPIPE. */
    case WSAESHUTDOWN:       return EPIPE;
    case WSAETIMEDOUT:       return ETIMEDOUT;
    case WSAECONNREFUSED:    return ECONNREFUSED;
    case WSAEHOSTUNREACH:    return EHOSTUNREACH;
    case WSAENAMETOOLONG:    return ENAMETOOLONG;
    case WSANOTINITIALISED:  return EINVAL;
    default:                 return EIO;
    }
}

/* Sets errno from the last Winsock error and returns -1, for `return wsa_fail();`. */
static int wsa_fail(void) {
    errno = errno_from_wsa(WSAGetLastError());
    return -1;
}

static int errno_from_win32(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:       return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:  return EACCES;
    case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:        return ENOMEM;
    case ERROR_INVALID_HANDLE:     return EBADF;
    case ERROR_DIRECTORY:          return ENOTDIR;
    default:                       return EIO;
    }
}

static int win32_fail(void) {
    errno = errno_from_win32(GetLastError());
    return -1;
}

/* UTF-8, which is what Swift passes, to the UTF-16 every W call takes. NULL on
 * failure; the caller frees. */
static wchar_t *wide(const char *s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) { errno = EINVAL; return NULL; }
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) { errno = ENOMEM; return NULL; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n);
    return w;
}

/* A socket that is non-inheritable from the start: no window in which a
 * process started by another thread could pick it up. */
static av_sock new_socket(int family, int type, int protocol) {
    net_init();
    SOCKET s = WSASocketW(family, type, protocol, NULL, 0,
                          WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s == INVALID_SOCKET) { wsa_fail(); return AV_BAD_SOCK; }
    return s;
}

/* ======================================================================== */
/* Poller                                                                   */
/* ======================================================================== */

av_poll_t av_poll_create(void) {
    net_init();
    HANDLE h = epoll_create1(0);
    if (!h) { errno = EMFILE; return AV_BAD_POLL; }
    return (av_poll_t)h;
}

int av_poll_close(av_poll_t pfd) { return epoll_close((HANDLE)pfd); }

static uint32_t to_epoll(uint32_t mask) {
    uint32_t e = EPOLLRDHUP;   /* peer half-close must be visible, not silent */
    if (mask & AV_POLL_READ)  e |= EPOLLIN;
    if (mask & AV_POLL_WRITE) e |= EPOLLOUT;
    return e;
}

static int epoll_ctl_op(av_poll_t pfd, int op, av_sock fd, uint32_t mask, uint64_t token) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = to_epoll(mask);
    ev.data.u64 = token;
    return epoll_ctl((HANDLE)pfd, op, fd, &ev);
}

int av_poll_add(av_poll_t pfd, av_sock fd, uint32_t mask, uint64_t token) {
    return epoll_ctl_op(pfd, EPOLL_CTL_ADD, fd, mask, token);
}
/* wepoll has no exclusive wake-up: as on kqueue, every poller watching a
 * shared listener wakes, and the ones that lose the race to accept get EAGAIN. */
int av_poll_add_exclusive(av_poll_t pfd, av_sock fd, uint32_t mask, uint64_t token) {
    return epoll_ctl_op(pfd, EPOLL_CTL_ADD, fd, mask, token);
}
int av_poll_mod(av_poll_t pfd, av_sock fd, uint32_t mask, uint64_t token) {
    return epoll_ctl_op(pfd, EPOLL_CTL_MOD, fd, mask, token);
}
int av_poll_del(av_poll_t pfd, av_sock fd, uint32_t last_mask) {
    (void)last_mask;
    return epoll_ctl((HANDLE)pfd, EPOLL_CTL_DEL, fd, NULL);
}

int av_poll_wait(av_poll_t pfd, av_event *out, int max_events, int timeout_ms) {
    struct epoll_event evs[256];
    if (max_events > 256) max_events = 256;
    int n = epoll_wait((HANDLE)pfd, evs, max_events, timeout_ms);
    if (n < 0) return errno == EINTR ? 0 : -1;
    for (int i = 0; i < n; i++) {
        uint32_t m = 0;
        uint32_t e = evs[i].events;
        if (e & EPOLLIN)  m |= AV_POLL_READ;
        if (e & EPOLLOUT) m |= AV_POLL_WRITE;
        if (e & EPOLLERR) m |= AV_POLL_ERR;
        if (e & (EPOLLHUP | EPOLLRDHUP)) m |= AV_POLL_HUP;
        out[i].token = evs[i].data.u64;
        out[i].mask = m;
        out[i]._pad = 0;
    }
    return n;
}

int av_poll_single(av_sock fd, int for_write, int timeout_ms) {
    WSAPOLLFD p;
    p.fd = fd;
    p.events = (SHORT)(for_write ? POLLWRNORM : POLLRDNORM);
    p.revents = 0;
    int r = WSAPoll(&p, 1, timeout_ms);
    if (r == SOCKET_ERROR) return wsa_fail();
    if (r == 0) return 0;
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
    return 1;
}

/* ======================================================================== */
/* Sockets                                                                  */
/* ======================================================================== */

int av_set_nonblock(av_sock fd) {
    u_long on = 1;
    return ioctlsocket(fd, FIONBIO, &on) == 0 ? 0 : wsa_fail();
}

int av_set_cloexec(av_sock fd) {
    return SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, 0) ? 0 : win32_fail();
}

int av_clear_cloexec(av_sock fd) {
    return SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)
        ? 0 : win32_fail();
}

int av_set_nodelay(av_sock fd, int on) {
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on) == 0
        ? 0 : wsa_fail();
}

int av_shutdown_write(av_sock fd) { return shutdown(fd, SD_SEND) == 0 ? 0 : wsa_fail(); }
int av_close(av_sock fd) { return closesocket(fd) == 0 ? 0 : wsa_fail(); }
int av_close_file(int fd) { return _close(fd); }

ssize_t av_read_file(int fd, void *buf, size_t n) {
    if (n > INT_MAX) n = INT_MAX;
    return (ssize_t)_read(fd, buf, (unsigned)n);
}

av_sock av_listen_tcp(const char *host, uint16_t port, int backlog, int reuseport, int v6only) {
    /* SO_REUSEPORT has no Windows counterpart: the multi-process model there
     * shares one listening socket instead of opening one per worker. */
    (void)reuseport;
    net_init();
    char portbuf[8];
    snprintf(portbuf, sizeof portbuf, "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    struct addrinfo *res = NULL;
    const char *h = (host && host[0]) ? host : NULL;
    if (getaddrinfo(h, portbuf, &hints, &res) != 0) { errno = EINVAL; return AV_BAD_SOCK; }

    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        av_sock fd = new_socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == AV_BAD_SOCK) continue;
        /* SO_REUSEADDR on Windows lets another socket bind the same port while
         * this one listens -- the opposite of what it means on POSIX. Exclusive
         * use is the safe default, and a restarted server can still bind at
         * once, because Windows does not hold a closed listener's port. */
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof one);
        if (ai->ai_family == AF_INET6) {
            int v = v6only ? 1 : 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v, sizeof v);
        }
        if (bind(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0 && listen(fd, backlog) == 0) {
            av_set_nonblock(fd);
            freeaddrinfo(res);
            return fd;
        }
        int saved = errno_from_wsa(WSAGetLastError());
        closesocket(fd);
        errno = saved;
    }
    freeaddrinfo(res);
    return AV_BAD_SOCK;
}

static int unix_address(const char *path, struct sockaddr_un *sa) {
    memset(sa, 0, sizeof *sa);
    sa->sun_family = AF_UNIX;
    size_t n = strlen(path);
    if (n >= sizeof sa->sun_path) { errno = ENAMETOOLONG; return -1; }
    memcpy(sa->sun_path, path, n);
    return 0;
}

av_sock av_listen_unix(const char *path, int backlog, int unlink_existing) {
    struct sockaddr_un sa;
    if (unix_address(path, &sa) != 0) return AV_BAD_SOCK;
    if (unlink_existing) av_unlink(path);
    av_sock fd = new_socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == AV_BAD_SOCK) return AV_BAD_SOCK;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, backlog) != 0) {
        int saved = errno_from_wsa(WSAGetLastError());
        closesocket(fd);
        errno = saved;
        return AV_BAD_SOCK;
    }
    av_set_nonblock(fd);
    return fd;
}

/* connect() on a non-blocking socket: WSAEWOULDBLOCK is Winsock's EINPROGRESS. */
static av_sock finish_connect(av_sock fd, const struct sockaddr *sa, int len, int *in_progress) {
    if (connect(fd, sa, len) == 0) return fd;
    int e = WSAGetLastError();
    if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS) {
        if (in_progress) *in_progress = 1;
        return fd;
    }
    closesocket(fd);
    errno = errno_from_wsa(e);
    return AV_BAD_SOCK;
}

av_sock av_connect_tcp(const char *host, uint16_t port, int *in_progress) {
    if (in_progress) *in_progress = 0;
    if (!host || !host[0]) { errno = EINVAL; return AV_BAD_SOCK; }
    net_init();
    char portbuf[8];
    snprintf(portbuf, sizeof portbuf, "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portbuf, &hints, &res) != 0) { errno = EINVAL; return AV_BAD_SOCK; }
    av_sock fd = new_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == AV_BAD_SOCK) { int s = errno; freeaddrinfo(res); errno = s; return AV_BAD_SOCK; }
    av_set_nonblock(fd);
    av_sock r = finish_connect(fd, res->ai_addr, (int)res->ai_addrlen, in_progress);
    int saved = errno;
    freeaddrinfo(res);
    errno = saved;
    return r;
}

av_sock av_connect_unix(const char *path, int *in_progress) {
    if (in_progress) *in_progress = 0;
    struct sockaddr_un sa;
    if (unix_address(path, &sa) != 0) return AV_BAD_SOCK;
    av_sock fd = new_socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == AV_BAD_SOCK) return AV_BAD_SOCK;
    av_set_nonblock(fd);
    return finish_connect(fd, (struct sockaddr *)&sa, sizeof sa, in_progress);
}

int av_connect_error(av_sock fd) {
    int err = 0;
    int len = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0) {
        return errno_from_wsa(WSAGetLastError());
    }
    return err ? errno_from_wsa(err) : 0;
}

static void fill_peer(const struct sockaddr_storage *ss, int slen,
                      char *peer, size_t peer_len, uint16_t *port) {
    if (peer && peer_len) peer[0] = 0;
    if (port) *port = 0;
    if (ss->ss_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)ss;
        if (peer) getnameinfo((const struct sockaddr *)ss, slen, peer, (DWORD)peer_len,
                              NULL, 0, NI_NUMERICHOST);
        if (port) *port = ntohs(a->sin_port);
    } else if (ss->ss_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)ss;
        if (peer) getnameinfo((const struct sockaddr *)ss, slen, peer, (DWORD)peer_len,
                              NULL, 0, NI_NUMERICHOST);
        if (port) *port = ntohs(a->sin6_port);
    } else if (ss->ss_family == AF_UNIX) {
        if (peer && peer_len > 4) memcpy(peer, "unix", 5);
    }
}

av_sock av_accept(av_sock lfd, char *peer, size_t peer_len, uint16_t *peer_port) {
    struct sockaddr_storage ss;
    int slen = sizeof ss;
    SOCKET fd = accept(lfd, (struct sockaddr *)&ss, &slen);
    if (fd == INVALID_SOCKET) { wsa_fail(); return AV_BAD_SOCK; }
    /* An accepted socket takes its listener's non-blocking mode but not
     * WSA_FLAG_NO_HANDLE_INHERIT, so both are set here. */
    av_set_nonblock(fd);
    av_set_cloexec(fd);
    fill_peer(&ss, slen, peer, peer_len, peer_port);
    return fd;
}

int av_local_addr(av_sock fd, char *host, size_t host_len, uint16_t *port) {
    struct sockaddr_storage ss;
    int slen = sizeof ss;
    if (getsockname(fd, (struct sockaddr *)&ss, &slen) != 0) return wsa_fail();
    fill_peer(&ss, slen, host, host_len, port);
    return 0;
}

uint16_t av_local_port(av_sock fd) {
    struct sockaddr_storage ss;
    int len = sizeof ss;
    if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) return 0;
    if (ss.ss_family == AF_INET) return ntohs(((struct sockaddr_in *)&ss)->sin_port);
    if (ss.ss_family == AF_INET6) return ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    return 0;
}

av_sock av_connect_udp(const char *host, uint16_t port) {
    if (!host || !host[0]) { errno = EINVAL; return AV_BAD_SOCK; }
    net_init();
    char portbuf[8];
    snprintf(portbuf, sizeof portbuf, "%u", (unsigned)port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portbuf, &hints, &res) != 0) { errno = EINVAL; return AV_BAD_SOCK; }
    av_sock fd = new_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == AV_BAD_SOCK) { int s = errno; freeaddrinfo(res); errno = s; return AV_BAD_SOCK; }
    av_set_nonblock(fd);
    int rc = connect(fd, res->ai_addr, (int)res->ai_addrlen);
    int saved = rc == 0 ? 0 : errno_from_wsa(WSAGetLastError());
    freeaddrinfo(res);
    if (rc == 0) return fd;
    closesocket(fd);
    errno = saved;
    return AV_BAD_SOCK;
}

ssize_t av_read(av_sock fd, void *buf, size_t n) {
    if (n > INT_MAX) n = INT_MAX;
    int r = recv(fd, (char *)buf, (int)n, 0);
    if (r == SOCKET_ERROR) return wsa_fail();
    return r;
}

ssize_t av_write(av_sock fd, const void *buf, size_t n) {
    if (n > INT_MAX) n = INT_MAX;
    int r = send(fd, (const char *)buf, (int)n, 0);
    if (r == SOCKET_ERROR) return wsa_fail();
    return r;
}

ssize_t av_writev(av_sock fd, const struct iovec *iov, int iovcnt) {
    /* WSABUF holds the same two fields as iovec in the other order. 64 is
     * more than any caller gathers; a longer list sends its first 64, which
     * is a short write the caller already handles. */
    WSABUF bufs[64];
    if (iovcnt > 64) iovcnt = 64;
    for (int i = 0; i < iovcnt; i++) {
        size_t len = iov[i].iov_len;
        bufs[i].len = len > ULONG_MAX ? ULONG_MAX : (ULONG)len;
        bufs[i].buf = (CHAR *)iov[i].iov_base;
    }
    DWORD sent = 0;
    if (WSASend(fd, bufs, (DWORD)iovcnt, &sent, 0, NULL, NULL) == SOCKET_ERROR) return wsa_fail();
    return (ssize_t)sent;
}

/* A read from the file at *offset, then a send of what was read. Not
 * TransmitFile: that wants an overlapped socket operation with a completion to
 * wait for, and this has to behave like sendfile on a non-blocking socket --
 * send what fits now, report how much, EAGAIN when nothing does. The bytes go
 * through user space once, which is what TLS already costs on every platform. */
ssize_t av_sendfile(av_sock out_fd, int in_fd, av_off_t *offset, size_t count) {
    static _Thread_local char chunk[65536];
    HANDLE h = (HANDLE)_get_osfhandle(in_fd);
    if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (count > sizeof chunk) count = sizeof chunk;
    OVERLAPPED at;
    memset(&at, 0, sizeof at);
    at.Offset = (DWORD)((uint64_t)*offset & 0xffffffffu);
    at.OffsetHigh = (DWORD)((uint64_t)*offset >> 32);
    DWORD got = 0;
    if (!ReadFile(h, chunk, (DWORD)count, &got, &at)) {
        if (GetLastError() == ERROR_HANDLE_EOF) return 0;
        return win32_fail();
    }
    if (got == 0) return 0;
    int sent = send(out_fd, chunk, (int)got, 0);
    if (sent == SOCKET_ERROR) return wsa_fail();
    *offset += sent;
    return sent;
}

/* ------------------------------------------------------------------------ */
/* Files                                                                    */
/* ------------------------------------------------------------------------ */

/* FILETIME (100 ns since 1601) to nanoseconds since 1970. */
static long long filetime_ns(FILETIME ft) {
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (long long)(t - 116444736000000000ULL) * 100LL;
}

/* The path a handle actually refers to, symlinks and junctions resolved, in
 * the \\?\ form. NULL on failure; the caller frees. */
static wchar_t *final_path(HANDLE h) {
    DWORD n = GetFinalPathNameByHandleW(h, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0) return NULL;
    wchar_t *p = (wchar_t *)malloc(((size_t)n + 1) * sizeof(wchar_t));
    if (!p) return NULL;
    DWORD m = GetFinalPathNameByHandleW(h, p, n + 1, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (m == 0 || m > n) { free(p); return NULL; }
    return p;
}

/* A regular file opened for reading, as a CRT descriptor, with its size and
 * modification time. -1 for a directory, a device or anything else. */
static int open_regular(HANDLE h, long long *size, long long *mtime) {
    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileType(h) != FILE_TYPE_DISK || !GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(h);
        errno = EINVAL;
        return -1;
    }
    if (size) *size = ((long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    if (mtime) *mtime = filetime_ns(info.ftLastWriteTime);
    int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY);
    if (fd < 0) { CloseHandle(h); return -1; }
    return fd;
}

static HANDLE open_for_read(const wchar_t *path, int directory) {
    return CreateFileW(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       OPEN_EXISTING,
                       directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL,
                       NULL);
}

/* The file is opened first and its containment checked afterwards, against
 * the path the open actually reached: the root and the file are each resolved
 * through their open handles, so nothing can be swapped between a check and
 * the open the way it can between realpath and open. This is the guarantee
 * openat2's RESOLVE_BENEATH gives on Linux. */
int av_static_open(const char *root, const char *relative,
                   long long *size, long long *mtime) {
    while (*relative == '/' || *relative == '\\') relative++;
    /* A drive or a UNC prefix in the request would make the join below name
     * a different tree entirely; ':' is never part of a served name here. */
    if (strchr(relative, ':')) return -1;

    wchar_t *wroot = wide(root);
    if (!wroot) return -1;
    HANDLE rh = open_for_read(wroot, 1);
    free(wroot);
    if (rh == INVALID_HANDLE_VALUE) return -1;
    wchar_t *real_root = final_path(rh);
    CloseHandle(rh);
    if (!real_root) return -1;

    size_t rlen = strlen(relative);
    char *joined = (char *)malloc(strlen(root) + rlen + 2);
    if (!joined) { free(real_root); return -1; }
    sprintf(joined, "%s\\%s", root, relative);
    wchar_t *wjoined = wide(joined);
    free(joined);
    if (!wjoined) { free(real_root); return -1; }
    HANDLE fh = open_for_read(wjoined, 0);
    free(wjoined);
    if (fh == INVALID_HANDLE_VALUE) { free(real_root); return -1; }

    wchar_t *real = final_path(fh);
    if (!real) { CloseHandle(fh); free(real_root); return -1; }
    size_t root_len = wcslen(real_root);
    /* Windows paths compare without case; CompareStringOrdinal does that the
     * way the file system does, not the way a locale would. */
    int inside = wcslen(real) > root_len &&
                 CompareStringOrdinal(real, (int)root_len, real_root, (int)root_len, TRUE) == CSTR_EQUAL &&
                 (real[root_len] == L'\\' || real_root[root_len - 1] == L'\\');
    free(real);
    free(real_root);
    if (!inside) { CloseHandle(fh); return -1; }
    return open_regular(fh, size, mtime);
}

int av_open_read(const char *path) {
    if (!path) { errno = EINVAL; return -1; }
    wchar_t *w = wide(path);
    if (!w) return -1;
    HANDLE h = open_for_read(w, 0);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return win32_fail();
    return open_regular(h, NULL, NULL);
}

int av_unlink(const char *path) {
    wchar_t *w = wide(path);
    if (!w) return -1;
    BOOL ok = DeleteFileW(w);
    free(w);
    return ok ? 0 : win32_fail();
}

const char *av_getenv(const char *name) { return getenv(name); }

static int attributes(const char *path, WIN32_FILE_ATTRIBUTE_DATA *out) {
    wchar_t *w = wide(path);
    if (!w) return 0;
    BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, out);
    free(w);
    return ok ? 1 : 0;
}

int av_path_exists(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    return attributes(path, &a);
}

int64_t av_mtime_ns(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!attributes(path, &a)) return -1;
    return filetime_ns(a.ftLastWriteTime);
}

int av_is_dir(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!attributes(path, &a)) return 0;
    return (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
}

/* ------------------------------------------------------------------------ */
/* Pipes: loopback socket pairs                                             */
/* ------------------------------------------------------------------------ */

/* Windows has no socketpair. A listener on 127.0.0.1, port 0, takes one
 * connection and is closed. Anything else on the machine could race to that
 * port, so the accepted socket's peer address has to be the connecting
 * socket's own before the pair is trusted. */
static int loopback_pair(av_sock out[2]) {
    av_sock lst = new_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lst == AV_BAD_SOCK) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int alen = sizeof a;
    av_sock c = AV_BAD_SOCK, s = AV_BAD_SOCK;
    if (bind(lst, (struct sockaddr *)&a, sizeof a) != 0 ||
        getsockname(lst, (struct sockaddr *)&a, &alen) != 0 ||
        listen(lst, 1) != 0) goto fail;
    c = new_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c == AV_BAD_SOCK) goto fail;
    if (connect(c, (struct sockaddr *)&a, sizeof a) != 0) goto fail;
    for (;;) {
        struct sockaddr_in peer, mine;
        int plen = sizeof peer, mlen = sizeof mine;
        s = accept(lst, (struct sockaddr *)&peer, &plen);
        if (s == INVALID_SOCKET) { s = AV_BAD_SOCK; goto fail; }
        if (getsockname(c, (struct sockaddr *)&mine, &mlen) == 0 &&
            peer.sin_port == mine.sin_port && peer.sin_addr.s_addr == mine.sin_addr.s_addr) break;
        closesocket(s);   /* someone else's connection: not ours to keep */
        s = AV_BAD_SOCK;
    }
    closesocket(lst);
    av_set_cloexec(s);
    for (int i = 0; i < 2; i++) {
        av_sock x = i ? s : c;
        int one = 1;
        setsockopt(x, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        av_set_nonblock(x);
    }
    /* [0] reads and [1] writes, as pipe(2) has it; both work either way. */
    out[0] = s;
    out[1] = c;
    return 0;
fail:
    {
        int saved = errno_from_wsa(WSAGetLastError());
        closesocket(lst);
        if (c != AV_BAD_SOCK) closesocket(c);
        if (s != AV_BAD_SOCK) closesocket(s);
        errno = saved;
    }
    return -1;
}

int av_pipe(av_sock fds[2]) { return loopback_pair(fds); }

/* ======================================================================== */
/* Errors                                                                   */
/* ======================================================================== */

int av_errno(void) { return errno; }
void av_set_errno(int e) { errno = e; }
const char *av_strerror(int e) { return strerror(e); }
int av_err_is_again(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
int av_err_is_intr(int e) { return e == EINTR; }

/* ======================================================================== */
/* Time                                                                     */
/* ======================================================================== */

uint64_t av_monotonic_ms(void) {
    /* The tick count is the coarse clock here: second-granularity idle
     * timeouts are all this serves. */
    return (uint64_t)GetTickCount64();
}

uint64_t av_monotonic_us(void) {
    static LARGE_INTEGER freq;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart / freq.QuadPart) * 1000000u +
           (uint64_t)((now.QuadPart % freq.QuadPart) * 1000000 / freq.QuadPart);
}

uint64_t av_realtime_us(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return (uint64_t)(filetime_ns(ft) / 1000);
}

int64_t av_unix_seconds(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (int64_t)(filetime_ns(ft) / 1000000000LL);
}

/* Windows has no per-datagram receive timestamps on a stream socket: the
 * header's "a platform without it". */
int av_set_rx_timestamps(av_sock fd) { (void)fd; errno = ENOSYS; return -1; }

ssize_t av_read_stamped(av_sock fd, void *buf, size_t n, uint64_t *arrived_us) {
    *arrived_us = 0;
    return (long)av_read(fd, buf, n);
}

/* ------------------------------------------------------------------------ */
/* What a process av_spawn_self started was given                           */
/* ------------------------------------------------------------------------ */

/* The parent describes the child's inheritance in one environment variable:
 *
 *   AVIAN_SPAWN=1;<parent pid>;<control socket>;<role>;<s1>,<s2>,...;<h1>:<bytes1>,...
 *
 * Handle values are the same in the child, because inherited handles keep
 * theirs. The variable is removed once read, so that nothing this process
 * starts sees it; a process that finds it anyway, carried in a copied
 * environment, is told apart by the parent pid, which is not its parent. */

#include <tlhelp32.h>

#define AV_SPAWN_MAX_SOCKS 64
#define AV_SHM_MAX 32

static struct {
    int spawned;
    av_sock control;
    char role[64];
    av_sock socks[AV_SPAWN_MAX_SOCKS];
    int sock_count;
    HANDLE shm[AV_SHM_MAX];
    size_t shm_bytes[AV_SHM_MAX];
    int shm_count;
    int shm_next;
} g_spawn;
static INIT_ONCE g_spawn_once = INIT_ONCE_STATIC_INIT;

static DWORD parent_pid(void) {
    DWORD self = GetCurrentProcessId(), parent = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e;
    e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        if (e.th32ProcessID == self) { parent = e.th32ParentProcessID; break; }
    }
    CloseHandle(snap);
    return parent;
}

static BOOL CALLBACK spawn_load(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once; (void)param; (void)ctx;
    g_spawn.control = AV_BAD_SOCK;
    char buf[4096];
    DWORD n = GetEnvironmentVariableA("AVIAN_SPAWN", buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) return TRUE;
    SetEnvironmentVariableA("AVIAN_SPAWN", NULL);

    char *save = NULL;
    char *version = strtok_s(buf, ";", &save);
    char *ppid = strtok_s(NULL, ";", &save);
    char *control = strtok_s(NULL, ";", &save);
    char *role = strtok_s(NULL, ";", &save);
    char *socks = strtok_s(NULL, ";", &save);
    char *shm = strtok_s(NULL, ";", &save);
    if (!version || strcmp(version, "1") != 0 || !ppid || !control || !role) return TRUE;
    if ((DWORD)strtoul(ppid, NULL, 10) != parent_pid()) return TRUE;

    g_spawn.control = (av_sock)_strtoui64(control, NULL, 10);
    strncpy_s(g_spawn.role, sizeof g_spawn.role, role, _TRUNCATE);
    if (socks && strcmp(socks, "-") != 0) {
        char *s2 = NULL;
        for (char *t = strtok_s(socks, ",", &s2); t && g_spawn.sock_count < AV_SPAWN_MAX_SOCKS;
             t = strtok_s(NULL, ",", &s2)) {
            g_spawn.socks[g_spawn.sock_count++] = (av_sock)_strtoui64(t, NULL, 10);
        }
    }
    if (shm && strcmp(shm, "-") != 0) {
        char *s3 = NULL;
        for (char *t = strtok_s(shm, ",", &s3); t && g_spawn.shm_count < AV_SHM_MAX;
             t = strtok_s(NULL, ",", &s3)) {
            char *colon = strchr(t, ':');
            if (!colon) continue;
            *colon = 0;
            g_spawn.shm[g_spawn.shm_count] = (HANDLE)(uintptr_t)_strtoui64(t, NULL, 10);
            g_spawn.shm_bytes[g_spawn.shm_count] = (size_t)_strtoui64(colon + 1, NULL, 10);
            g_spawn.shm_count++;
        }
    }
    /* Non-blocking mode is kept in each process's own Winsock state, not in
     * the socket, so an inherited socket arrives blocking: one accept on a
     * listener then waits for a connection instead of saying there is none. */
    net_init();
    av_set_nonblock(g_spawn.control);
    for (int i = 0; i < g_spawn.sock_count; i++) av_set_nonblock(g_spawn.socks[i]);
    g_spawn.spawned = 1;
    return TRUE;
}

static void spawn_info(void) { InitOnceExecuteOnce(&g_spawn_once, spawn_load, NULL, NULL); }

int av_spawned(char *role, size_t cap) {
    spawn_info();
    if (!g_spawn.spawned) return 0;
    if (role && cap > 0) strncpy_s(role, cap, g_spawn.role, _TRUNCATE);
    return 1;
}

int av_spawned_sockets(av_sock *out, int cap) {
    spawn_info();
    int n = g_spawn.sock_count < cap ? g_spawn.sock_count : cap;
    for (int i = 0; i < n; i++) out[i] = g_spawn.socks[i];
    return n;
}

/* ======================================================================== */
/* Signals: console control events                                          */
/* ======================================================================== */

static av_sock g_sigpipe[2] = { AV_BAD_SOCK, AV_BAD_SOCK };

/* Runs on a thread the system starts for the event. Ctrl-C is SIGINT; every
 * other way of being asked to go -- Ctrl-Break, the console closing, logoff,
 * shutdown -- is SIGTERM, a graceful stop. */
static BOOL WINAPI console_event(DWORD type) {
    unsigned char b = (unsigned char)(type == CTRL_C_EVENT ? SIGINT : SIGTERM);
    if (g_sigpipe[1] == AV_BAD_SOCK) return FALSE;
    send(g_sigpipe[1], (const char *)&b, 1, 0);
    return TRUE;
}

/* A spawned worker shares its parent's console, so it sees the same events.
 * It leaves them to the parent, which passes each on over the control socket
 * as a signal, as a POSIX supervisor passes on the ones it receives -- acting
 * on them here as well would stop the worker twice. When the console closes,
 * returning would end the process at once, so the handler waits instead, and
 * the worker drains until the parent's SIGTERM ends it or the system's
 * deadline does. */
static BOOL WINAPI child_console_event(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) return TRUE;
    Sleep(INFINITE);
    return TRUE;
}

av_sock av_signal_pipe_init(void) {
    spawn_info();
    if (g_spawn.spawned && g_spawn.control != AV_BAD_SOCK) {
        SetConsoleCtrlHandler(child_console_event, TRUE);
        return g_spawn.control;
    }
    if (g_sigpipe[0] != AV_BAD_SOCK) return g_sigpipe[0];
    if (loopback_pair(g_sigpipe) != 0) return AV_BAD_SOCK;
    SetConsoleCtrlHandler(console_event, TRUE);
    return g_sigpipe[0];
}

/* No fork and no exec on Windows, so nothing inherits a mask or a handler:
 * these have nothing to do. */
void av_block_piped_signals(void) {}
void av_unblock_piped_signals(void) {}
void av_signals_default(void) {}
void av_ignore_sigpipe(void) {}   /* a send to a closed peer is an error here, never a signal */

/* ======================================================================== */
/* Processes                                                                */
/* ======================================================================== */

/* There is no fork: workers are new copies of the program (av_spawn_self),
 * and these say so. */
pid_t av_fork_worker(void) { errno = ENOSYS; return -1; }
pid_t av_fork(void) { errno = ENOSYS; return -1; }
int av_execv(const char *path, char *const argv[]) { (void)path; (void)argv; errno = ENOSYS; return -1; }
int av_handoff_pair(av_sock fds[2]) { (void)fds; errno = ENOSYS; return -1; }
ssize_t av_send_fd(av_sock chan, av_sock fd, const void *meta, size_t meta_len) {
    (void)chan; (void)fd; (void)meta; (void)meta_len; errno = ENOSYS; return -1;
}
ssize_t av_recv_fd(av_sock chan, av_sock *fd, void *meta, size_t cap) {
    (void)chan; (void)meta; (void)cap; *fd = AV_BAD_SOCK; errno = ENOSYS; return -1;
}

/* Every region av_shm_map made or inherited, so that av_spawn_self can hand
 * them on and av_shm_attached can answer. A server makes a handful, all at
 * start-up. */
static struct { HANDLE h; void *p; size_t bytes; int attached; } g_shm[AV_SHM_MAX];
static SRWLOCK g_shm_lock = SRWLOCK_INIT;

/* The children av_spawn_self started and has not yet reaped. */
struct child { pid_t pid; HANDLE process; av_sock control; };
static struct child *g_children;
static int g_child_count, g_child_cap;
static SRWLOCK g_children_lock = SRWLOCK_INIT;
/* Kills every child when the last handle to it closes, which is when this
 * process ends however it ends: a worker never outlives its supervisor. */
static HANDLE g_job;

static int find_child(pid_t pid) {
    for (int i = 0; i < g_child_count; i++) if (g_children[i].pid == pid) return i;
    return -1;
}

static void drop_child(int i) {
    CloseHandle(g_children[i].process);
    if (g_children[i].control != AV_BAD_SOCK) closesocket(g_children[i].control);
    g_children[i] = g_children[--g_child_count];
}

static int set_inherit(HANDLE h, int on) {
    return SetHandleInformation(h, HANDLE_FLAG_INHERIT, on ? HANDLE_FLAG_INHERIT : 0) ? 0 : -1;
}

static void add_handle(HANDLE *list, int *n, HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return;
    for (int i = 0; i < *n; i++) if (list[i] == h) return;
    list[(*n)++] = h;
}

pid_t av_spawn_self(const char *role, const av_sock *socks, int count) {
    if (!role || strchr(role, ';') || strlen(role) >= sizeof g_spawn.role ||
        count < 0 || count > AV_SPAWN_MAX_SOCKS) {
        errno = EINVAL;
        return -1;
    }
    if (!g_job) {
        g_job = CreateJobObjectW(NULL, NULL);
        if (!g_job) return win32_fail();
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
        memset(&limits, 0, sizeof limits);
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }

    av_sock control[2];
    if (loopback_pair(control) != 0) return -1;

    /* Three standard handles, the control socket, the sockets and the
     * regions. Only these are inherited, whatever else is inheritable. */
    HANDLE list[3 + 1 + AV_SPAWN_MAX_SOCKS + AV_SHM_MAX];
    int n = 0;
    HANDLE std[3] = { NULL, NULL, NULL };
    DWORD which[3] = { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
    for (int i = 0; i < 3; i++) {
        HANDLE h = GetStdHandle(which[i]);
        if (!h || h == INVALID_HANDLE_VALUE) continue;
        if (DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &std[i], 0, TRUE,
                            DUPLICATE_SAME_ACCESS)) {
            add_handle(list, &n, std[i]);
        }
    }
    set_inherit((HANDLE)control[0], 1);
    add_handle(list, &n, (HANDLE)control[0]);
    for (int i = 0; i < count; i++) {
        set_inherit((HANDLE)socks[i], 1);
        add_handle(list, &n, (HANDLE)socks[i]);
    }

    char env[4096];
    int len = snprintf(env, sizeof env, "1;%lu;%llu;%s;", (unsigned long)GetCurrentProcessId(),
                       (unsigned long long)control[0], role);
    for (int i = 0; i < count && len < (int)sizeof env; i++) {
        len += snprintf(env + len, sizeof env - (size_t)len, "%s%llu", i ? "," : "",
                        (unsigned long long)socks[i]);
    }
    if (count == 0 && len < (int)sizeof env) len += snprintf(env + len, sizeof env - (size_t)len, "-");
    if (len < (int)sizeof env) len += snprintf(env + len, sizeof env - (size_t)len, ";");
    int regions = 0;
    AcquireSRWLockShared(&g_shm_lock);
    for (int i = 0; i < AV_SHM_MAX && len < (int)sizeof env; i++) {
        if (!g_shm[i].h) continue;
        len += snprintf(env + len, sizeof env - (size_t)len, "%s%llu:%llu", regions ? "," : "",
                        (unsigned long long)(uintptr_t)g_shm[i].h,
                        (unsigned long long)g_shm[i].bytes);
        add_handle(list, &n, g_shm[i].h);
        regions++;
    }
    ReleaseSRWLockShared(&g_shm_lock);
    if (regions == 0 && len < (int)sizeof env) len += snprintf(env + len, sizeof env - (size_t)len, "-");

    pid_t pid = -1;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    wchar_t *cmd = NULL;
    wchar_t exe[32768];

    if (len >= (int)sizeof env) { errno = E2BIG; goto done; }
    if (GetModuleFileNameW(NULL, exe, (DWORD)(sizeof exe / sizeof exe[0])) == 0) { win32_fail(); goto done; }
    cmd = _wcsdup(GetCommandLineW());   /* CreateProcessW may write to it */
    if (!cmd) { errno = ENOMEM; goto done; }

    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);
    attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(size);
    if (!attrs || !InitializeProcThreadAttributeList(attrs, 1, 0, &size)) {
        free(attrs);
        attrs = NULL;
        errno = ENOMEM;
        goto done;
    }
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list,
                                   (SIZE_T)n * sizeof(HANDLE), NULL, NULL)) {
        win32_fail();
        goto done;
    }

    STARTUPINFOEXW si;
    memset(&si, 0, sizeof si);
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = std[0];
    si.StartupInfo.hStdOutput = std[1];
    si.StartupInfo.hStdError = std[2];
    si.lpAttributeList = attrs;

    /* The child reads its inheritance from the environment it starts with,
     * which is this process's at the moment of creation. */
    static SRWLOCK env_lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&env_lock);
    SetEnvironmentVariableA("AVIAN_SPAWN", env);
    BOOL ok = CreateProcessW(exe, cmd, NULL, NULL, TRUE,
                             EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED,
                             NULL, NULL, &si.StartupInfo, &pi);
    SetEnvironmentVariableA("AVIAN_SPAWN", NULL);
    ReleaseSRWLockExclusive(&env_lock);
    if (!ok) { win32_fail(); goto done; }

    AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    AcquireSRWLockExclusive(&g_children_lock);
    if (g_child_count == g_child_cap) {
        int cap = g_child_cap ? g_child_cap * 2 : 16;
        struct child *grown = (struct child *)realloc(g_children, (size_t)cap * sizeof *grown);
        if (grown) { g_children = grown; g_child_cap = cap; }
    }
    if (g_child_count < g_child_cap) {
        g_children[g_child_count].pid = (pid_t)pi.dwProcessId;
        g_children[g_child_count].process = pi.hProcess;
        g_children[g_child_count].control = control[1];
        g_child_count++;
        control[1] = AV_BAD_SOCK;   /* the registry owns it now */
        pid = (pid_t)pi.dwProcessId;
    } else {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        errno = ENOMEM;
    }
    ReleaseSRWLockExclusive(&g_children_lock);

done:
    if (attrs) { DeleteProcThreadAttributeList(attrs); free(attrs); }
    free(cmd);
    for (int i = 0; i < 3; i++) if (std[i]) CloseHandle(std[i]);
    /* Back to non-inheritable, so that nothing started later -- by this
     * thread or the application's -- picks them up. */
    for (int i = 0; i < count; i++) set_inherit((HANDLE)socks[i], 0);
    closesocket(control[0]);
    if (control[1] != AV_BAD_SOCK) closesocket(control[1]);
    return pid;
}

/* The status as POSIX encodes a normal exit, (code << 8), so a caller's
 * WIFEXITED / WEXITSTATUS reading of it holds. */
static int exit_status(HANDLE h) {
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    return (int)((code & 0xff) << 8);
}

pid_t av_waitpid(pid_t pid, int *status, int nohang) {
    if (pid == -1) {
        for (;;) {
            HANDLE waits[MAXIMUM_WAIT_OBJECTS];
            int k = 0;
            AcquireSRWLockExclusive(&g_children_lock);
            if (g_child_count == 0) {
                ReleaseSRWLockExclusive(&g_children_lock);
                errno = ECHILD;
                return -1;
            }
            for (int i = 0; i < g_child_count; i++) {
                if (WaitForSingleObject(g_children[i].process, 0) == WAIT_OBJECT_0) {
                    pid_t done = g_children[i].pid;
                    if (status) *status = exit_status(g_children[i].process);
                    drop_child(i);
                    ReleaseSRWLockExclusive(&g_children_lock);
                    return done;
                }
                if (k < MAXIMUM_WAIT_OBJECTS) waits[k++] = g_children[i].process;
            }
            ReleaseSRWLockExclusive(&g_children_lock);
            if (nohang) return 0;
            /* Any of them exiting is the wake; the scan above then finds it. */
            WaitForMultipleObjects((DWORD)k, waits, FALSE, 1000);
        }
    }

    AcquireSRWLockExclusive(&g_children_lock);
    int i = find_child(pid);
    if (i >= 0) {
        HANDLE h = g_children[i].process;
        ReleaseSRWLockExclusive(&g_children_lock);
        if (WaitForSingleObject(h, nohang ? 0 : INFINITE) == WAIT_TIMEOUT) return 0;
        AcquireSRWLockExclusive(&g_children_lock);
        i = find_child(pid);
        if (i >= 0) {
            if (status) *status = exit_status(g_children[i].process);
            drop_child(i);
        }
        ReleaseSRWLockExclusive(&g_children_lock);
        return pid;
    }
    ReleaseSRWLockExclusive(&g_children_lock);

    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) { errno = ECHILD; return -1; }
    DWORD w = WaitForSingleObject(h, nohang ? 0 : INFINITE);
    if (w == WAIT_TIMEOUT) { CloseHandle(h); return 0; }
    if (status) *status = exit_status(h);
    CloseHandle(h);
    return pid;
}

/* Signal 0 asks whether the process exists and SIGKILL ends it. A child
 * av_spawn_self started is sent any other signal as its number over the
 * control socket; anything else can only be asked those two. */
int av_kill(pid_t pid, int sig) {
    AcquireSRWLockShared(&g_children_lock);
    int i = find_child(pid);
    if (i >= 0 && sig != 0 && sig != SIGKILL) {
        unsigned char b = (unsigned char)sig;
        int sent = send(g_children[i].control, (const char *)&b, 1, 0);
        ReleaseSRWLockShared(&g_children_lock);
        return sent == 1 ? 0 : wsa_fail();
    }
    ReleaseSRWLockShared(&g_children_lock);

    HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) { errno = ESRCH; return -1; }
    int rc = 0;
    if (sig == SIGKILL) {
        if (!TerminateProcess(h, 137)) rc = win32_fail();
    } else if (sig != 0) {
        errno = ENOSYS;
        rc = -1;
    } else if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
        errno = ESRCH;   /* exited, not yet reaped */
        rc = -1;
    }
    CloseHandle(h);
    return rc;
}

ssize_t av_write_stdout(const void *buf, size_t n) {
    return (ssize_t)_write(1, buf, n > 0x7fffffffu ? 0x7fffffffu : (unsigned)n);
}

ssize_t av_write_stderr(const void *buf, size_t n) {
    return (ssize_t)_write(2, buf, n > 0x7fffffffu ? 0x7fffffffu : (unsigned)n);
}

void av_sleep_ms(unsigned ms) { Sleep(ms); }

/* ------------------------------------------------------------------------ */
/* Directory listing                                                        */
/* ------------------------------------------------------------------------ */

struct av_dir {
    HANDLE find;
    int first;          /* the entry FindFirstFileW returned is still unread */
    WIN32_FIND_DATAW data;
    char name[MAX_PATH * 3];
};

av_dir *av_dir_open(const char *path) {
    size_t n = strlen(path);
    char *pattern = (char *)malloc(n + 3);
    if (!pattern) { errno = ENOMEM; return NULL; }
    memcpy(pattern, path, n);
    memcpy(pattern + n, "\\*", 3);
    wchar_t *w = wide(pattern);
    free(pattern);
    if (!w) return NULL;
    av_dir *dir = (av_dir *)malloc(sizeof *dir);
    if (!dir) { free(w); errno = ENOMEM; return NULL; }
    dir->find = FindFirstFileExW(w, FindExInfoBasic, &dir->data, FindExSearchNameMatch, NULL,
                                 FIND_FIRST_EX_LARGE_FETCH);
    free(w);
    if (dir->find == INVALID_HANDLE_VALUE) { win32_fail(); free(dir); return NULL; }
    dir->first = 1;
    return dir;
}

const char *av_dir_next(av_dir *dir) {
    for (;;) {
        if (!dir->first && !FindNextFileW(dir->find, &dir->data)) return NULL;
        dir->first = 0;
        if (WideCharToMultiByte(CP_UTF8, 0, dir->data.cFileName, -1, dir->name,
                                (int)sizeof dir->name, NULL, NULL) > 0) {
            return dir->name;
        }
        /* A name that does not fit is skipped rather than returned cut. */
    }
}

void av_dir_close(av_dir *dir) {
    if (!dir) return;
    FindClose(dir->find);
    free(dir);
}

pid_t av_getpid(void) { return (pid_t)GetCurrentProcessId(); }
void av_set_process_name(const char *name) { (void)name; }

int av_cpu_count(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? (int)n : 1;
}

/* Sockets are kernel handles with no per-process descriptor table to raise.
 * The CRT's table, which holds the static files, is raised to its ceiling. */
ssize_t av_raise_nofile_limit(void) {
    int n = _setmaxstdio(8192);
    return n > 0 ? (long)n : 512;
}

int av_sched_set_slice(uint64_t slice_ns) { (void)slice_ns; errno = ENOSYS; return -1; }
int64_t av_sched_slice(void) { errno = ENOSYS; return -1; }

/* ------------------------------------------------------------------------ */
/* Shutdown watchdog                                                        */
/* ------------------------------------------------------------------------ */

/* SIGALRM's job, done by a thread: wait out the deadline on an event that a
 * cancel sets, and _exit if it was not set. */
static HANDLE g_watchdog_cancel;
static int g_watchdog_code;
static DWORD g_watchdog_ms;

static unsigned __stdcall watchdog(void *arg) {
    HANDLE cancel = (HANDLE)arg;
    if (WaitForSingleObject(cancel, g_watchdog_ms) == WAIT_TIMEOUT) {
        static const char msg[] = "[error] shutdown exceeded its deadline; exiting immediately\n";
        _write(2, msg, (unsigned)(sizeof msg - 1));
        _exit(g_watchdog_code);
    }
    return 0;
}

void av_exit_after(unsigned seconds, int code) {
    av_cancel_exit_timer();
    if (seconds == 0) return;
    g_watchdog_code = code;
    g_watchdog_ms = seconds * 1000u;
    g_watchdog_cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_watchdog_cancel) return;
    uintptr_t t = _beginthreadex(NULL, 0, watchdog, g_watchdog_cancel, 0, NULL);
    if (t) CloseHandle((HANDLE)t);
}

void av_cancel_exit_timer(void) {
    if (g_watchdog_cancel) {
        SetEvent(g_watchdog_cancel);
        /* The thread has its own reference through the wait; the event is
         * left for it rather than closed under it. */
        g_watchdog_cancel = NULL;
    }
}

/* ------------------------------------------------------------------------ */
/* --reload: the executable                                                 */
/* ------------------------------------------------------------------------ */

int av_executable_path(char *out, size_t cap) {
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    if (n == 0 || n >= sizeof buf / sizeof buf[0]) return -1;
    int m = WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, (int)cap, NULL, NULL);
    return m > 0 ? 0 : -1;
}

uint64_t av_file_signature(const char *path, int executable) {
    (void)executable;   /* no execute bit: a regular file is what can be checked */
    wchar_t *w = wide(path);
    if (!w) return 0;
    HANDLE h = open_for_read(w, 0);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION info;
    BOOL ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    uint64_t parts[5] = {
        (uint64_t)info.dwVolumeSerialNumber,
        ((uint64_t)info.nFileIndexHigh << 32) | info.nFileIndexLow,
        ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow,
        ((uint64_t)info.ftLastWriteTime.dwHighDateTime << 32) | info.ftLastWriteTime.dwLowDateTime,
        (uint64_t)info.dwFileAttributes,
    };
    uint64_t h64 = 1469598103934665603ULL;
    for (size_t i = 0; i < sizeof parts; i++) {
        h64 = (h64 ^ ((unsigned char *)parts)[i]) * 1099511628211ULL;
    }
    return h64 ? h64 : 1;
}

int av_probe_executable(const char *path, int timeout_ms) {
    size_t n = strlen(path);
    char *cmd = (char *)malloc(n + 16);
    if (!cmd) return 0;
    sprintf(cmd, "\"%s\" --version", path);
    wchar_t *wcmd = wide(cmd);
    free(cmd);
    if (!wcmd) return 0;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    SetHandleInformation(nul, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = si.hStdOutput = si.hStdError = nul;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(wcmd);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) return 0;
    CloseHandle(pi.hThread);
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, (DWORD)timeout_ms) == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
    } else {
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hProcess);
    return code == 0 ? 1 : 0;
}

/* ======================================================================== */
/* Threads                                                                  */
/* ======================================================================== */

/* SRW locks and condition variables: no allocation inside, no kernel object,
 * and the same shape as pthread's mutex and condition. */
struct av_mutex { SRWLOCK l; };
struct av_cond  { CONDITION_VARIABLE c; };

av_mutex *av_mutex_new(void) {
    av_mutex *m = (av_mutex *)malloc(sizeof *m);
    if (m) InitializeSRWLock(&m->l);
    return m;
}
void av_mutex_free(av_mutex *m) { free(m); }
void av_mutex_lock(av_mutex *m)   { AcquireSRWLockExclusive(&m->l); }
void av_mutex_unlock(av_mutex *m) { ReleaseSRWLockExclusive(&m->l); }

av_cond *av_cond_new(void) {
    av_cond *c = (av_cond *)malloc(sizeof *c);
    if (c) InitializeConditionVariable(&c->c);
    return c;
}
void av_cond_free(av_cond *c) { free(c); }
void av_cond_wait(av_cond *c, av_mutex *m) { SleepConditionVariableSRW(&c->c, &m->l, INFINITE, 0); }
void av_cond_signal(av_cond *c)            { WakeConditionVariable(&c->c); }
void av_cond_broadcast(av_cond *c)         { WakeAllConditionVariable(&c->c); }

struct thread_start { void (*fn)(void *); void *arg; };

/* No signal mask to set: console events arrive on a thread of their own,
 * never on this one. */
static unsigned __stdcall thread_trampoline(void *raw) {
    struct thread_start s = *(struct thread_start *)raw;
    free(raw);
    s.fn(s.arg);
    return 0;
}

static HANDLE start_thread(void (*fn)(void *), void *arg, unsigned stack) {
    struct thread_start *s = (struct thread_start *)malloc(sizeof *s);
    if (!s) return NULL;
    s->fn = fn;
    s->arg = arg;
    uintptr_t t = _beginthreadex(NULL, stack, thread_trampoline, s,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) { free(s); return NULL; }
    return (HANDLE)t;
}

int av_thread_spawn(void (*fn)(void *), void *arg) {
    HANDLE t = start_thread(fn, arg, 1u << 21);   /* 2 MiB is plenty per request */
    if (!t) return -1;
    CloseHandle(t);   /* detached */
    return 0;
}

struct av_thread { HANDLE h; };

av_thread *av_thread_start(void (*fn)(void *), void *arg) {
    av_thread *t = (av_thread *)malloc(sizeof *t);
    if (!t) return NULL;
    /* A long-lived thread runs a whole application: the 8 MiB a main thread
     * would have. */
    t->h = start_thread(fn, arg, 1u << 23);
    if (!t->h) { free(t); return NULL; }
    return t;
}

void av_thread_join(av_thread *t) {
    if (!t) return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

/* ======================================================================== */
/* Randomness                                                               */
/* ======================================================================== */

int av_random_bytes(void *out, size_t n) {
    unsigned char *p = (unsigned char *)out;
    while (n > 0) {
        ULONG chunk = n > 0x7fffffff ? 0x7fffffff : (ULONG)n;
        if (BCryptGenRandom(NULL, p, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return -1;
        p += chunk;
        n -= chunk;
    }
    return 0;
}

/* ======================================================================== */
/* Shared memory and small process helpers                                  */
/* ======================================================================== */

static void *shm_record(HANDLE h, void *p, size_t bytes, int attached) {
    AcquireSRWLockExclusive(&g_shm_lock);
    for (int i = 0; i < AV_SHM_MAX; i++) {
        if (!g_shm[i].h) {
            g_shm[i].h = h;
            g_shm[i].p = p;
            g_shm[i].bytes = bytes;
            g_shm[i].attached = attached;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_shm_lock);
    return p;
}

void *av_shm_map(size_t bytes) {
    /* A spawned worker maps what its parent made, in the order the parent
     * made it, since both run the same start-up. */
    spawn_info();
    if (g_spawn.shm_next < g_spawn.shm_count && g_spawn.shm_bytes[g_spawn.shm_next] == bytes) {
        HANDLE h = g_spawn.shm[g_spawn.shm_next++];
        void *p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
        if (!p) { win32_fail(); return NULL; }
        return shm_record(h, p, bytes, 1);
    }
    /* Inheritable, so a worker started with handle inheritance gets it. */
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                  (DWORD)((uint64_t)bytes >> 32), (DWORD)(bytes & 0xffffffffu), NULL);
    if (!h) { win32_fail(); return NULL; }
    void *p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (!p) { win32_fail(); CloseHandle(h); return NULL; }
    /* The paging file's pages start zeroed, as MAP_ANONYMOUS's do. */
    return shm_record(h, p, bytes, 0);
}

int av_shm_attached(const void *p) {
    int attached = 0;
    AcquireSRWLockShared(&g_shm_lock);
    for (int i = 0; i < AV_SHM_MAX; i++) {
        if (g_shm[i].h && g_shm[i].p == p) { attached = g_shm[i].attached; break; }
    }
    ReleaseSRWLockShared(&g_shm_lock);
    return attached;
}

void av_shm_unmap(void *p, size_t bytes) {
    (void)bytes;
    if (!p) return;
    UnmapViewOfFile(p);
    AcquireSRWLockExclusive(&g_shm_lock);
    for (int i = 0; i < AV_SHM_MAX; i++) {
        if (g_shm[i].p == p) { CloseHandle(g_shm[i].h); g_shm[i].h = NULL; g_shm[i].p = NULL; break; }
    }
    ReleaseSRWLockExclusive(&g_shm_lock);
}

int av_process_alive(pid_t pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;   /* exists, not ours to open */
    int alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

void av_yield(void) { SwitchToThread(); }

uint64_t av_entropy64(void) {
    uint64_t seed = 0;
    if (av_random_bytes(&seed, sizeof seed) == 0) return seed;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32);
}

#endif /* _WIN32 */
