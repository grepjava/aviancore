#ifndef AVIAN_PLATFORM_H
#define AVIAN_PLATFORM_H

/* ---------------------------------------------------------------------------
 * The few types that differ between POSIX and Windows.
 *
 * On Linux and Darwin every one of these is the type the API always used --
 * a socket is an int, a poller is an int -- so Swift still sees Int32 and no
 * caller changes. On Windows a socket is a SOCKET (a pointer-sized handle) and
 * the poller is wepoll's HANDLE, neither of which fits in an int.
 * ------------------------------------------------------------------------- */

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)

/* winsock2.h has to come before windows.h, or windows.h drags in the legacy
 * winsock.h and the two collide. */
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <basetsd.h>

typedef SOCKET    av_sock;           /* UINT_PTR */
/* wepoll's HANDLE, carried as an integer: Swift would see a HANDLE as an
 * optional pointer, and code shared with POSIX compares it as a number. */
typedef uintptr_t av_poll_t;
typedef SSIZE_T   ssize_t;
typedef int       pid_t;             /* a process id, not a handle */
typedef long long av_off_t;
#  define AV_BAD_SOCK ((av_sock)INVALID_SOCKET)
#  define AV_BAD_POLL ((av_poll_t)0)

/* POSIX's scatter/gather element. Winsock's WSABUF holds the same two fields
 * in the other order, so av_writev converts rather than casting. */
struct iovec {
    void  *iov_base;
    size_t iov_len;
};

#else

#  include <sys/types.h>
#  include <sys/uio.h>

typedef int   av_sock;
typedef int   av_poll_t;
typedef off_t av_off_t;
#  define AV_BAD_SOCK (-1)
#  define AV_BAD_POLL (-1)

#endif

/* Whether a socket or poller is a real one, for Swift, which cannot import the
 * casts in AV_BAD_SOCK and AV_BAD_POLL. */
static inline int av_sock_ok(av_sock s) { return s != AV_BAD_SOCK; }
static inline int av_poll_ok(av_poll_t p) { return p != AV_BAD_POLL; }
static const av_sock av_bad_sock = AV_BAD_SOCK;

#endif
