#define _GNU_SOURCE 1
#include "avian_watch.h"
#include "avian_sys.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#  include <sys/inotify.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) \
    || defined(__OpenBSD__) || defined(__DragonFly__)
#  define AV_WATCH_KQUEUE 1
#  include <sys/event.h>
#  include <sys/time.h>
#endif

#if defined(__linux__)

int av_watch_open(void) {
    return inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
}

int av_watch_add(int wfd, const char *dir) {
    /* Everything an editor does to a file, whether it writes in place or
     * writes a copy and renames it over the original. */
    uint32_t mask = IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_CREATE | IN_DELETE
                  | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF
                  | IN_ONLYDIR;
    return inotify_add_watch(wfd, dir, mask) < 0 ? -1 : 0;
}

int av_watch_drain(int wfd) {
    char buf[4096];
    int any = 0;
    for (;;) {
        ssize_t n = read(wfd, buf, sizeof buf);
        if (n > 0) { any = 1; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return any;
        return any ? 1 : -1;
    }
}

void av_watch_close(int wfd) {
    if (wfd >= 0) close(wfd);
}

#elif defined(AV_WATCH_KQUEUE)

/* kqueue watches descriptors, not paths, so each directory stays open for as
 * long as it is watched. There is one watcher in a process, the supervisor's,
 * so the list is process-wide. */
static int *g_dirs = NULL;
static int g_dir_count = 0;
static int g_dir_capacity = 0;

int av_watch_open(void) {
    int fd = kqueue();
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

int av_watch_add(int wfd, const char *dir) {
    int flags = O_RDONLY | O_CLOEXEC;
#  if defined(O_EVTONLY)
    flags = O_EVTONLY | O_CLOEXEC;
#  endif
    int fd = open(dir, flags);
    if (fd < 0) return -1;
    if (g_dir_count == g_dir_capacity) {
        int capacity = g_dir_capacity ? g_dir_capacity * 2 : 64;
        int *grown = realloc(g_dirs, (size_t)capacity * sizeof *g_dirs);
        if (!grown) { close(fd); return -1; }
        g_dirs = grown;
        g_dir_capacity = capacity;
    }
    /* A directory's own vnode changes when an entry is added, removed or
     * renamed -- which is what saving through a temporary file does. A file
     * written in place does not change its directory; the caller's periodic
     * scan is what notices that. */
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB | NOTE_LINK | NOTE_RENAME | NOTE_DELETE,
           0, NULL);
    if (kevent(wfd, &ev, 1, NULL, 0, NULL) < 0) { close(fd); return -1; }
    g_dirs[g_dir_count++] = fd;
    return 0;
}

int av_watch_drain(int wfd) {
    struct kevent evs[64];
    struct timespec zero = { 0, 0 };
    int any = 0;
    for (;;) {
        int n = kevent(wfd, NULL, 0, evs, 64, &zero);
        if (n > 0) { any = 1; if (n < 64) return 1; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return any ? 1 : -1;
        return any;
    }
}

void av_watch_close(int wfd) {
    for (int i = 0; i < g_dir_count; i++) close(g_dirs[i]);
    free(g_dirs);
    g_dirs = NULL;
    g_dir_count = g_dir_capacity = 0;
    if (wfd >= 0) close(wfd);
}

#elif defined(_WIN32)

/* ReadDirectoryChangesW on each directory, overlapped, all completing on one
 * port that a thread waits on. A completion writes a byte to the watch socket
 * and the read is issued again, so the socket is readable whenever something
 * changed since it was last drained -- the shape inotify has. The directories
 * are associated with the port, so their reads survive the thread that issued
 * them, which is the caller's and may be gone. */

#include <process.h>

struct watched {
    HANDLE dir;
    OVERLAPPED ov;
    DWORD buf[1024];   /* DWORD-aligned, as the call requires */
};

static HANDLE g_port = NULL;
static av_sock g_pair[2] = { AV_BAD_SOCK, AV_BAD_SOCK };
static struct watched **g_watched = NULL;
static int g_watched_count = 0;
static HANDLE g_thread = NULL;

#define WATCH_FILTER (FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME \
                      | FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE \
                      | FILE_NOTIFY_CHANGE_LAST_WRITE)

static int arm(struct watched *w) {
    memset(&w->ov, 0, sizeof w->ov);
    return ReadDirectoryChangesW(w->dir, w->buf, sizeof w->buf, FALSE, WATCH_FILTER,
                                 NULL, &w->ov, NULL) ? 0 : -1;
}

static unsigned __stdcall watch_thread(void *arg) {
    (void)arg;
    for (;;) {
        DWORD n = 0;
        ULONG_PTR key = 0;
        OVERLAPPED *ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(g_port, &n, &key, &ov, INFINITE);
        if (key == 0 && ov == NULL) return 0;   /* av_watch_close */
        struct watched *w = (struct watched *)key;
        if (!ok && GetLastError() == ERROR_OPERATION_ABORTED) continue;
        unsigned char b = 1;
        send(g_pair[1], (const char *)&b, 1, 0);
        if (w) arm(w);
    }
}

av_sock av_watch_open(void) {
    if (g_port) return g_pair[0];
    if (av_pipe(g_pair) != 0) return AV_BAD_SOCK;
    g_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!g_port) goto fail;
    uintptr_t t = _beginthreadex(NULL, 0, watch_thread, NULL, 0, NULL);
    if (!t) { CloseHandle(g_port); g_port = NULL; goto fail; }
    g_thread = (HANDLE)t;
    return g_pair[0];
fail:
    av_close(g_pair[0]);
    av_close(g_pair[1]);
    g_pair[0] = g_pair[1] = AV_BAD_SOCK;
    return AV_BAD_SOCK;
}

int av_watch_add(av_sock wfd, const char *dir) {
    (void)wfd;
    if (!g_port) return -1;
    int n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, NULL, 0);
    if (n <= 0) return -1;
    wchar_t *wdir = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!wdir) return -1;
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, n);
    HANDLE h = CreateFileW(wdir, FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    free(wdir);
    if (h == INVALID_HANDLE_VALUE) return -1;
    struct watched *w = (struct watched *)calloc(1, sizeof *w);
    struct watched **grown = (struct watched **)realloc(g_watched,
                                 (size_t)(g_watched_count + 1) * sizeof *g_watched);
    if (!w || !grown) { free(w); if (grown) g_watched = grown; CloseHandle(h); return -1; }
    g_watched = grown;
    w->dir = h;
    if (!CreateIoCompletionPort(h, g_port, (ULONG_PTR)w, 0) || arm(w) != 0) {
        CloseHandle(h);
        free(w);
        return -1;
    }
    g_watched[g_watched_count++] = w;
    return 0;
}

int av_watch_drain(av_sock wfd) {
    char buf[256];
    int any = 0;
    for (;;) {
        ssize_t n = av_read(wfd, buf, sizeof buf);
        if (n > 0) { any = 1; continue; }
        if (n < 0 && av_err_is_again(errno)) return any;
        return any ? 1 : -1;
    }
}

void av_watch_close(av_sock wfd) {
    (void)wfd;
    if (!g_port) return;
    for (int i = 0; i < g_watched_count; i++) {
        CancelIoEx(g_watched[i]->dir, NULL);
        CloseHandle(g_watched[i]->dir);
    }
    PostQueuedCompletionStatus(g_port, 0, 0, NULL);
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    for (int i = 0; i < g_watched_count; i++) free(g_watched[i]);
    free(g_watched);
    g_watched = NULL;
    g_watched_count = 0;
    CloseHandle(g_port);
    g_port = NULL;
    av_close(g_pair[0]);
    av_close(g_pair[1]);
    g_pair[0] = g_pair[1] = AV_BAD_SOCK;
}

#else

int av_watch_open(void) { return -1; }
int av_watch_add(int wfd, const char *dir) { (void)wfd; (void)dir; return -1; }
int av_watch_drain(int wfd) { (void)wfd; return 0; }
void av_watch_close(int wfd) { (void)wfd; }

#endif

#if defined(_WIN32)

int av_poll_either(av_sock a, av_sock b, int timeout_ms) {
    WSAPOLLFD p[2];
    int n = 1;
    p[0].fd = a;
    p[0].events = POLLRDNORM;
    p[0].revents = 0;
    if (b != AV_BAD_SOCK) {
        p[1].fd = b;
        p[1].events = POLLRDNORM;
        p[1].revents = 0;
        n = 2;
    }
    int r = WSAPoll(p, (ULONG)n, timeout_ms);
    if (r == SOCKET_ERROR) return -1;
    int mask = 0;
    if (p[0].revents & (POLLRDNORM | POLLHUP)) mask |= 1;
    if (n == 2 && (p[1].revents & (POLLRDNORM | POLLHUP))) mask |= 2;
    return mask;
}

#else

int av_poll_either(int a, int b, int timeout_ms) {
    struct pollfd p[2];
    p[0].fd = a;
    p[0].events = POLLIN;
    p[0].revents = 0;
    /* A negative descriptor is skipped by poll(2) itself. */
    p[1].fd = b;
    p[1].events = POLLIN;
    p[1].revents = 0;
    int r = poll(p, 2, timeout_ms);
    if (r < 0) return errno == EINTR ? 0 : -1;
    int mask = 0;
    if (p[0].revents & POLLIN) mask |= 1;
    if (b >= 0 && (p[1].revents & POLLIN)) mask |= 2;
    return mask;
}

#endif
