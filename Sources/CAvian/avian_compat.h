#ifndef AVIAN_COMPAT_H
#define AVIAN_COMPAT_H

/* The handful of POSIX library calls the C files use that Windows spells
 * differently. Internal to CAvian: not under include/, so Swift never sees it.
 * On POSIX every name here is the call it always was. */

#if defined(_WIN32)

#include "avian_platform.h"
#include <string.h>

#define strcasecmp  _stricmp
#define strncasecmp _strnicmp
#define strdup      _strdup

/* strcasestr, which the CRT does not have. ASCII case folding: header names
 * and the like, not text. */
static inline char *av_strcasestr(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0) return (char *)hay;
    for (; *hay; hay++) {
        if (_strnicmp(hay, needle, n) == 0) return (char *)hay;
    }
    return NULL;
}
#define strcasestr av_strcasestr

/* pthread_once */
typedef INIT_ONCE av_once_t;
#define AV_ONCE_INIT INIT_ONCE_STATIC_INIT
static BOOL CALLBACK av_once_trampoline(PINIT_ONCE once, PVOID fn, PVOID *ctx) {
    (void)once; (void)ctx;
    ((void (*)(void))fn)();
    return TRUE;
}
static inline void av_once(av_once_t *once, void (*fn)(void)) {
    InitOnceExecuteOnce(once, av_once_trampoline, (PVOID)fn, NULL);
}

/* dlopen and dlsym, for the libraries opened only if present. */
static inline void *av_dlopen(const char *name) { return (void *)LoadLibraryA(name); }
static inline void *av_dlsym(void *lib, const char *name) {
    return (void *)GetProcAddress((HMODULE)lib, name);
}

#else

#include <dlfcn.h>
#include <pthread.h>
#include <strings.h>

typedef pthread_once_t av_once_t;
#define AV_ONCE_INIT PTHREAD_ONCE_INIT
static inline void av_once(av_once_t *once, void (*fn)(void)) { pthread_once(once, fn); }

static inline void *av_dlopen(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
static inline void *av_dlsym(void *lib, const char *name) { return dlsym(lib, name); }

#endif

#endif
