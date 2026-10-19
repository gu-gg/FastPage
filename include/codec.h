#ifndef FASTPAGE_CODEC_H
#define FASTPAGE_CODEC_H

#ifdef __cplusplus
extern "C" {
#endif

/* The caller owns all buffers. No state or allocation survives a call. */
enum {
    FASTPAGE_PAGE_BYTES = 4096,
    FASTPAGE_ENCODE_CAPACITY = 4352
};

/* Call once before encoding/decoding; returns nonzero on success. */
int codec_init(void);

/* Compress n bytes, where 1 <= n <= FASTPAGE_PAGE_BYTES.
 * dst must have FASTPAGE_ENCODE_CAPACITY writable bytes; src/dst cannot overlap.
 * Returns stored length (1..n), or zero for an invalid n. A stored length equal
 * to n denotes a verbatim RAW page. Both lengths must be stored by the caller.
 * Optional lzsize receives the stored length; optional mode receives 0xc2 for
 * numeric data, 0xe5 for LZ records, or 255 for RAW. Either pointer may be NULL.
 */
unsigned codec_encode(const unsigned char *src, unsigned n, unsigned char *dst,
                      unsigned *lzsize, unsigned *mode);

/* Decode exactly n bytes into dst from exactly stored readable bytes in src.
 * dst must have n writable bytes; buffers cannot overlap and need no padding.
 * Returns nonzero on success, zero for invalid sizes or a malformed bitstream.
 * Output contents are unspecified after failure.
 */
int codec_decode(const unsigned char *src, unsigned stored,
                 unsigned char *dst, unsigned n);

#ifdef __cplusplus
}
#endif
#endif
