/* Response body compression: gzip through zlib, brotli and zstd through
 * whichever shared libraries the system has. See avian_compress.h. */

#include "avian_compress.h"

#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* Levels are chosen for a response being compressed while a client waits, not
 * for an archive. gzip 5 is within a few percent of 9 on text at a fraction of
 * the time; brotli 4 is the last quality before its cost climbs steeply; zstd
 * 3 is its own default. Pre-compressed static files are where the high levels
 * belong, and those are made at build time, not here. */
#define GZIP_LEVEL   5
#define BROTLI_LEVEL 4
#define ZSTD_LEVEL   3

/* --- brotli, by symbol --------------------------------------------------- *
 * Declared here rather than included: the headers are not installed where the
 * runtime library is, and these signatures have been stable since 1.0. */

typedef int (*brotli_set_parameter_fn)(void *, int, uint32_t);
typedef void *(*brotli_create_fn)(void *, void *, void *);
typedef void (*brotli_destroy_fn)(void *);
typedef int (*brotli_compress_stream_fn)(void *, int, size_t *, const uint8_t **,
                                         size_t *, uint8_t **, size_t *);
typedef int (*brotli_bool_fn)(void *);

#define BROTLI_PARAM_QUALITY   1
#define BROTLI_PARAM_LGWIN     2
#define BROTLI_OPERATION_PROCESS 0
#define BROTLI_OPERATION_FLUSH   1
#define BROTLI_OPERATION_FINISH  2

static struct {
    brotli_create_fn create;
    brotli_set_parameter_fn set_parameter;
    brotli_compress_stream_fn compress_stream;
    brotli_bool_fn has_more_output;
    brotli_bool_fn is_finished;
    brotli_destroy_fn destroy;
} brotli;

/* --- zstd, by symbol ----------------------------------------------------- */

typedef struct { const void *src; size_t size; size_t pos; } zstd_in;
typedef struct { void *dst; size_t size; size_t pos; } zstd_out;

typedef void *(*zstd_create_fn)(void);
typedef size_t (*zstd_free_fn)(void *);
typedef size_t (*zstd_set_parameter_fn)(void *, int, int);
typedef size_t (*zstd_compress_stream2_fn)(void *, zstd_out *, zstd_in *, int);
typedef unsigned (*zstd_is_error_fn)(size_t);

#define ZSTD_C_COMPRESSION_LEVEL 100
#define ZSTD_C_WINDOW_LOG        101
#define ZSTD_E_CONTINUE 0
#define ZSTD_E_FLUSH    1
#define ZSTD_E_END      2

static struct {
    zstd_create_fn create;
    zstd_free_fn free;
    zstd_set_parameter_fn set_parameter;
    zstd_compress_stream2_fn compress_stream2;
    zstd_is_error_fn is_error;
} zstd;

static pthread_once_t load_once = PTHREAD_ONCE_INIT;
static int have_brotli;
static int have_zstd;

static void *open_first(const char *const *names) {
    for (; *names; names++) {
        void *h = dlopen(*names, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
    return NULL;
}

static void load_libraries(void) {
    static const char *const brotli_names[] = {
        "libbrotlienc.so.1", "libbrotlienc.so",
        "libbrotlienc.1.dylib", "/opt/homebrew/lib/libbrotlienc.1.dylib",
        "/usr/local/lib/libbrotlienc.1.dylib", NULL,
    };
    static const char *const zstd_names[] = {
        "libzstd.so.1", "libzstd.so",
        "libzstd.1.dylib", "/opt/homebrew/lib/libzstd.1.dylib",
        "/usr/local/lib/libzstd.1.dylib", NULL,
    };

    void *b = open_first(brotli_names);
    if (b) {
        brotli.create = (brotli_create_fn)dlsym(b, "BrotliEncoderCreateInstance");
        brotli.set_parameter = (brotli_set_parameter_fn)dlsym(b, "BrotliEncoderSetParameter");
        brotli.compress_stream = (brotli_compress_stream_fn)dlsym(b, "BrotliEncoderCompressStream");
        brotli.has_more_output = (brotli_bool_fn)dlsym(b, "BrotliEncoderHasMoreOutput");
        brotli.is_finished = (brotli_bool_fn)dlsym(b, "BrotliEncoderIsFinished");
        brotli.destroy = (brotli_destroy_fn)dlsym(b, "BrotliEncoderDestroyInstance");
        have_brotli = brotli.create && brotli.set_parameter && brotli.compress_stream
            && brotli.has_more_output && brotli.is_finished && brotli.destroy;
    }

    void *z = open_first(zstd_names);
    if (z) {
        zstd.create = (zstd_create_fn)dlsym(z, "ZSTD_createCCtx");
        zstd.free = (zstd_free_fn)dlsym(z, "ZSTD_freeCCtx");
        zstd.set_parameter = (zstd_set_parameter_fn)dlsym(z, "ZSTD_CCtx_setParameter");
        zstd.compress_stream2 = (zstd_compress_stream2_fn)dlsym(z, "ZSTD_compressStream2");
        zstd.is_error = (zstd_is_error_fn)dlsym(z, "ZSTD_isError");
        have_zstd = zstd.create && zstd.free && zstd.set_parameter
            && zstd.compress_stream2 && zstd.is_error;
    }
}

int av_enc_available(int codec) {
    switch (codec) {
    case AV_ENC_GZIP: return 1;
    case AV_ENC_BR:   pthread_once(&load_once, load_libraries); return have_brotli;
    case AV_ENC_ZSTD: pthread_once(&load_once, load_libraries); return have_zstd;
    default:          return 0;
    }
}

struct av_enc {
    int codec;
    int finished;
    union {
        z_stream z;
        void *brotli;
        void *zstd;
    } u;
};

void *av_enc_new(int codec) {
    if (!av_enc_available(codec)) return NULL;
    struct av_enc *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->codec = codec;

    switch (codec) {
    case AV_ENC_GZIP:
        /* 15 + 16: a 32 KiB window, wrapped as gzip rather than zlib. */
        if (deflateInit2(&e->u.z, GZIP_LEVEL, Z_DEFLATED, 15 + 16, 8,
                         Z_DEFAULT_STRATEGY) != Z_OK) {
            free(e);
            return NULL;
        }
        return e;
    case AV_ENC_BR:
        e->u.brotli = brotli.create(NULL, NULL, NULL);
        if (!e->u.brotli) { free(e); return NULL; }
        brotli.set_parameter(e->u.brotli, BROTLI_PARAM_QUALITY, BROTLI_LEVEL);
        /* 2^22 bytes. The encoder's default is the same, but a streamed
         * response has no size hint and saying so costs nothing. */
        brotli.set_parameter(e->u.brotli, BROTLI_PARAM_LGWIN, 22);
        return e;
    case AV_ENC_ZSTD:
        e->u.zstd = zstd.create();
        if (!e->u.zstd) { free(e); return NULL; }
        zstd.set_parameter(e->u.zstd, ZSTD_C_COMPRESSION_LEVEL, ZSTD_LEVEL);
        /* RFC 9659: a decoder for the zstd content coding need not accept a
         * window larger than 8 MiB, and Chrome does not. Level 3 stays well
         * under it already; this makes that a promise rather than a
         * coincidence of the level table. */
        zstd.set_parameter(e->u.zstd, ZSTD_C_WINDOW_LOG, 23);
        return e;
    }
    free(e);
    return NULL;
}

void av_enc_free(void *enc) {
    struct av_enc *e = enc;
    if (!e) return;
    switch (e->codec) {
    case AV_ENC_GZIP: deflateEnd(&e->u.z); break;
    case AV_ENC_BR:   brotli.destroy(e->u.brotli); break;
    case AV_ENC_ZSTD: zstd.free(e->u.zstd); break;
    }
    free(e);
}

int av_enc_run(void *enc, const uint8_t *in, size_t n, int mode,
               uint8_t *out, size_t cap, size_t *consumed, size_t *produced) {
    struct av_enc *e = enc;
    *consumed = 0;
    *produced = 0;
    if (!e) return -1;
    if (e->finished) return n == 0 ? 0 : -1;

    switch (e->codec) {
    case AV_ENC_GZIP: {
        z_stream *z = &e->u.z;
        z->next_in = (Bytef *)in;
        z->avail_in = (uInt)n;
        z->next_out = out;
        z->avail_out = (uInt)cap;
        int flush = mode == AV_ENC_FINISH ? Z_FINISH
                  : mode == AV_ENC_FLUSH ? Z_SYNC_FLUSH : Z_NO_FLUSH;
        int rc = deflate(z, flush);
        *consumed = n - z->avail_in;
        *produced = cap - z->avail_out;
        if (rc == Z_STREAM_END) { e->finished = 1; return 0; }
        /* Z_BUF_ERROR only means no progress was possible, which after a
         * flush that already went out is the expected answer. */
        if (rc != Z_OK && rc != Z_BUF_ERROR) return -1;
        if (mode == AV_ENC_FINISH) return 1;
        /* Output space used up means there may be more; zlib says to call
         * again with the same flush until it leaves some unused. */
        return (z->avail_in == 0 && z->avail_out != 0) ? 0 : 1;
    }
    case AV_ENC_BR: {
        size_t avail_in = n, avail_out = cap;
        const uint8_t *next_in = in;
        uint8_t *next_out = out;
        int op = mode == AV_ENC_FINISH ? BROTLI_OPERATION_FINISH
               : mode == AV_ENC_FLUSH ? BROTLI_OPERATION_FLUSH : BROTLI_OPERATION_PROCESS;
        if (!brotli.compress_stream(e->u.brotli, op, &avail_in, &next_in,
                                    &avail_out, &next_out, NULL)) {
            return -1;
        }
        *consumed = n - avail_in;
        *produced = cap - avail_out;
        if (mode == AV_ENC_FINISH && brotli.is_finished(e->u.brotli)) {
            e->finished = 1;
            return 0;
        }
        if (avail_in == 0 && !brotli.has_more_output(e->u.brotli)
            && mode != AV_ENC_FINISH) {
            return 0;
        }
        return 1;
    }
    case AV_ENC_ZSTD: {
        zstd_in zin = { in, n, 0 };
        zstd_out zout = { out, cap, 0 };
        int directive = mode == AV_ENC_FINISH ? ZSTD_E_END
                      : mode == AV_ENC_FLUSH ? ZSTD_E_FLUSH : ZSTD_E_CONTINUE;
        size_t left = zstd.compress_stream2(e->u.zstd, &zout, &zin, directive);
        *consumed = zin.pos;
        *produced = zout.pos;
        if (zstd.is_error(left)) return -1;
        if (mode == AV_ENC_CONTINUE) return zin.pos == zin.size ? 0 : 1;
        if (left == 0 && zin.pos == zin.size) {
            if (mode == AV_ENC_FINISH) e->finished = 1;
            return 0;
        }
        return 1;
    }
    }
    return -1;
}

/* ======================================================================== */
/* Decoding                                                                 */
/* ======================================================================== */

typedef int (*brotli_dec_stream_fn)(void *, size_t *, const uint8_t **, size_t *,
                                    uint8_t **, size_t *);

static struct {
    brotli_create_fn create;
    brotli_dec_stream_fn decompress_stream;
    brotli_destroy_fn destroy;
} brotli_dec;

typedef size_t (*zstd_decompress_stream_fn)(void *, zstd_out *, zstd_in *);

static struct {
    zstd_create_fn create;
    zstd_free_fn free;
    zstd_set_parameter_fn set_parameter;
    zstd_decompress_stream_fn decompress_stream;
    zstd_is_error_fn is_error;
} zstd_dec;

/* BrotliDecoderResult */
#define BROTLI_DECODER_RESULT_SUCCESS           1
#define BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT  2
#define BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT 3
/* ZSTD_d_windowLogMax */
#define ZSTD_D_WINDOW_LOG_MAX 100

static pthread_once_t dec_load_once = PTHREAD_ONCE_INIT;
static int have_brotli_dec;
static int have_zstd_dec;

static void load_decoders(void) {
    static const char *const brotli_names[] = {
        "libbrotlidec.so.1", "libbrotlidec.so",
        "libbrotlidec.1.dylib", "/opt/homebrew/lib/libbrotlidec.1.dylib",
        "/usr/local/lib/libbrotlidec.1.dylib", NULL,
    };
    static const char *const zstd_names[] = {
        "libzstd.so.1", "libzstd.so",
        "libzstd.1.dylib", "/opt/homebrew/lib/libzstd.1.dylib",
        "/usr/local/lib/libzstd.1.dylib", NULL,
    };

    void *b = open_first(brotli_names);
    if (b) {
        brotli_dec.create = (brotli_create_fn)dlsym(b, "BrotliDecoderCreateInstance");
        brotli_dec.decompress_stream = (brotli_dec_stream_fn)dlsym(b, "BrotliDecoderDecompressStream");
        brotli_dec.destroy = (brotli_destroy_fn)dlsym(b, "BrotliDecoderDestroyInstance");
        have_brotli_dec = brotli_dec.create && brotli_dec.decompress_stream && brotli_dec.destroy;
    }

    void *z = open_first(zstd_names);
    if (z) {
        zstd_dec.create = (zstd_create_fn)dlsym(z, "ZSTD_createDCtx");
        zstd_dec.free = (zstd_free_fn)dlsym(z, "ZSTD_freeDCtx");
        zstd_dec.set_parameter = (zstd_set_parameter_fn)dlsym(z, "ZSTD_DCtx_setParameter");
        zstd_dec.decompress_stream = (zstd_decompress_stream_fn)dlsym(z, "ZSTD_decompressStream");
        zstd_dec.is_error = (zstd_is_error_fn)dlsym(z, "ZSTD_isError");
        have_zstd_dec = zstd_dec.create && zstd_dec.free && zstd_dec.set_parameter
            && zstd_dec.decompress_stream && zstd_dec.is_error;
    }
}

int av_dec_available(int codec) {
    switch (codec) {
    case AV_ENC_GZIP:
    case AV_DEC_DEFLATE: return 1;
    case AV_ENC_BR:   pthread_once(&dec_load_once, load_decoders); return have_brotli_dec;
    case AV_ENC_ZSTD: pthread_once(&dec_load_once, load_decoders); return have_zstd_dec;
    default:          return 0;
    }
}

struct av_dec {
    int codec;
    /* For deflate, whether the zlib stream has been set up: zlib or raw is
     * decided from the first two bytes. */
    int started;
    int ended;
    union {
        z_stream z;
        void *brotli;
        void *zstd;
    } u;
};

void *av_dec_new(int codec) {
    if (!av_dec_available(codec)) return NULL;
    struct av_dec *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->codec = codec;

    switch (codec) {
    case AV_ENC_GZIP:
        /* 15 + 16: a 32 KiB window, gzip wrapper only. */
        if (inflateInit2(&d->u.z, 15 + 16) != Z_OK) { free(d); return NULL; }
        d->started = 1;
        return d;
    case AV_DEC_DEFLATE:
        return d;
    case AV_ENC_BR:
        d->u.brotli = brotli_dec.create(NULL, NULL, NULL);
        if (!d->u.brotli) { free(d); return NULL; }
        return d;
    case AV_ENC_ZSTD:
        d->u.zstd = zstd_dec.create();
        if (!d->u.zstd) { free(d); return NULL; }
        /* RFC 9659: the zstd content coding's window is at most 8 MiB, and a
         * decoder need not hold more for a frame that asks for it. */
        zstd_dec.set_parameter(d->u.zstd, ZSTD_D_WINDOW_LOG_MAX, 23);
        return d;
    }
    free(d);
    return NULL;
}

void av_dec_free(void *dec) {
    struct av_dec *d = dec;
    if (!d) return;
    switch (d->codec) {
    case AV_ENC_GZIP:    inflateEnd(&d->u.z); break;
    case AV_DEC_DEFLATE: if (d->started) inflateEnd(&d->u.z); break;
    case AV_ENC_BR:      brotli_dec.destroy(d->u.brotli); break;
    case AV_ENC_ZSTD:    zstd_dec.free(d->u.zstd); break;
    }
    free(d);
}

static int inflate_step(struct av_dec *d, const uint8_t *in, size_t n,
                        uint8_t *out, size_t cap, size_t *consumed, size_t *produced) {
    z_stream *z = &d->u.z;
    static const uint8_t nothing = 0;
    if (n > UINT_MAX) n = UINT_MAX;
    if (cap > UINT_MAX) cap = UINT_MAX;
    z->next_in = (Bytef *)(n > 0 ? in : &nothing);
    z->avail_in = (uInt)n;
    z->next_out = out;
    z->avail_out = (uInt)cap;
    for (;;) {
        int rc = inflate(z, Z_NO_FLUSH);
        *consumed = n - z->avail_in;
        *produced = cap - z->avail_out;
        if (rc == Z_STREAM_END) {
            /* Another gzip member follows: carry on into it. */
            if (d->codec == AV_ENC_GZIP && z->avail_in >= 2
                && z->next_in[0] == 0x1f && z->next_in[1] == 0x8b) {
                /* With no room left, the next call finds the end again and
                 * starts the member then. */
                if (z->avail_out == 0) return 1;
                if (inflateReset(z) != Z_OK) return -1;
                continue;
            }
            d->ended = 1;
            return 2;
        }
        if (rc == Z_OK) return z->avail_out == 0 ? 1 : 0;
        if (rc == Z_BUF_ERROR) {
            if (z->avail_out == 0) return 1;
            return z->avail_in == 0 ? 0 : -1;
        }
        return -1;
    }
}

int av_dec_run(void *dec, const uint8_t *in, size_t n,
               uint8_t *out, size_t cap, size_t *consumed, size_t *produced) {
    struct av_dec *d = dec;
    *consumed = 0;
    *produced = 0;
    if (!d) return -1;
    if (d->ended) return 2;

    switch (d->codec) {
    case AV_DEC_DEFLATE:
        if (!d->started) {
            /* Both header bytes are needed to tell zlib from raw deflate. */
            if (n < 2) return 0;
            /* A zlib header: compression method 8, and the two bytes read as
             * a number a multiple of 31. Anything else is raw deflate. */
            int zlib = (in[0] & 0x0f) == 8 && ((in[0] << 8) | in[1]) % 31 == 0;
            if (inflateInit2(&d->u.z, zlib ? 15 : -15) != Z_OK) return -1;
            d->started = 1;
        }
        return inflate_step(d, in, n, out, cap, consumed, produced);
    case AV_ENC_GZIP:
        return inflate_step(d, in, n, out, cap, consumed, produced);
    case AV_ENC_BR: {
        size_t avail_in = n, avail_out = cap;
        const uint8_t *next_in = in;
        uint8_t *next_out = out;
        int rc = brotli_dec.decompress_stream(d->u.brotli, &avail_in, &next_in,
                                              &avail_out, &next_out, NULL);
        *consumed = n - avail_in;
        *produced = cap - avail_out;
        switch (rc) {
        case BROTLI_DECODER_RESULT_SUCCESS: d->ended = 1; return 2;
        case BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT: return 0;
        case BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT: return 1;
        default: return -1;
        }
    }
    case AV_ENC_ZSTD: {
        zstd_in zin = { in, n, 0 };
        zstd_out zout = { out, cap, 0 };
        size_t hint = zstd_dec.decompress_stream(d->u.zstd, &zout, &zin);
        *consumed = zin.pos;
        *produced = zout.pos;
        if (zstd_dec.is_error(hint)) return -1;
        if (hint == 0) { d->ended = 1; return 2; }
        if (zout.pos == zout.size) return 1;
        return zin.pos == zin.size ? 0 : 1;
    }
    }
    return -1;
}
