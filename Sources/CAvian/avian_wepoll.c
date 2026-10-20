/* wepoll, vendored: epoll's API on Windows, over the AFD driver. Compiled only
 * there; vendor/wepoll/VENDORING.md says where it came from. */
#if defined(_WIN32)
#  include "vendor/wepoll/wepoll.c"
#endif
