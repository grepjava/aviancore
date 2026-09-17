#ifndef AVIAN_COMPRESS_H
#define AVIAN_COMPRESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Response body compression.
 *
 * gzip comes from zlib, which is linked: it is on every system this builds
 * on. brotli and zstd are loaded with dlopen the first time they are asked
 * for, so a machine without them still runs -- it just offers gzip -- and a
 * wheel does not have to carry either library.
 *
 * One streaming interface over all three. Swift drives it in a loop: feed
 * input, give it output space, call again while it says there is more.
 * ------------------------------------------------------------------------- */

#define AV_ENC_IDENTITY 0
#define AV_ENC_GZIP     1
#define AV_ENC_BR       2
#define AV_ENC_ZSTD     3

/* What to do with the input once it has been taken. */
#define AV_ENC_CONTINUE 0   /* buffer it; output whenever the codec likes */
#define AV_ENC_FLUSH    1   /* everything so far must be decodable by the peer */
#define AV_ENC_FINISH   2   /* end the stream */

/* 1 when the codec can be used in this process. */
int av_enc_available(int codec);

/* A new stream, or NULL when the codec is unavailable or out of memory. */
void *av_enc_new(int codec);
void av_enc_free(void *enc);

/* Takes up to `n` bytes of `in` and writes up to `cap` bytes to `out`.
 *
 * Returns 0 when all of the input has been taken and whatever `mode` asked for
 * is complete, 1 when it must be called again with fresh output space (and
 * the input that was not yet consumed), and -1 when the codec failed.
 *
 * `*consumed` and `*produced` say how far it got either way. */
int av_enc_run(void *enc, const uint8_t *in, size_t n, int mode,
               uint8_t *out, size_t cap, size_t *consumed, size_t *produced);

/* ---------------------------------------------------------------------------
 * Response body decompression, for a client reading what a server sent.
 *
 * The same codecs the other way, plus "deflate", which RFC 9110 says is the
 * zlib format and which some servers send as raw deflate anyway: the first
 * two bytes decide. A gzip body of several members is read as one.
 * ------------------------------------------------------------------------- */

#define AV_DEC_DEFLATE  4

/* 1 when the codec can be decoded in this process. */
int av_dec_available(int codec);

/* A new stream, or NULL when the codec is unavailable or out of memory. */
void *av_dec_new(int codec);
void av_dec_free(void *dec);

/* Takes up to `n` bytes of `in` and writes up to `cap` bytes to `out`.
 *
 * Returns 0 when all of the input has been taken and more is expected, 1 when
 * it must be called again with fresh output space (and the input not yet
 * consumed), 2 when the encoded stream has ended, and -1 when the input is not
 * valid for the codec. Input left over after the end is not consumed.
 *
 * `*consumed` and `*produced` say how far it got either way. */
int av_dec_run(void *dec, const uint8_t *in, size_t n,
               uint8_t *out, size_t cap, size_t *consumed, size_t *produced);

#ifdef __cplusplus
}
#endif

#endif
