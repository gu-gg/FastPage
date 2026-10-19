/* Hand-built decoder vectors, independent of every encoder implementation.
 * Link with src/decoder.c; run under ASan/UBSan for exact-allocation
 * bounds checks. No corpus or performance measurement is needed.
 * C2 fields are packed one bit at a time, and E5 matches are expanded one byte
 * at a time. These scalar oracles intentionally share no SIMD/copy helpers.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fastpage_decoder_init(void);
int codec_decode(const unsigned char *, unsigned, unsigned char *, unsigned);

enum { PAGE_BYTES = 4096, SCRATCH_BYTES = 8192 };
static char current_case[192];
static unsigned long c2_cases, c2_raw_cases, c2_constant_cases, e5_cases;
static unsigned long skipped;

static void fail(const char *why) {
    fprintf(stderr, "FAIL: %s: %s\n", current_case, why);
    exit(1);
}

static void put16(unsigned char *dst, unsigned value) {
    dst[0] = (unsigned char)value;
    dst[1] = (unsigned char)(value >> 8);
}

static uint32_t random32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

static void check(const unsigned char *payload, unsigned size,
                  const unsigned char *expected, unsigned n) {
    if (!size || size >= n || n > PAGE_BYTES)
        fail("test attempted compressed input outside the codec ABI");
    unsigned char *src = malloc(size), *dst = malloc(n);
    if (!src || !dst) fail("allocation");
    memcpy(src, payload, size);
    for (unsigned i = 0; i < n; ++i) dst[i] = (unsigned char)(expected[i] ^ 255u);
    if (!codec_decode(src, size, dst, n)) fail("valid vector rejected");
    if (memcmp(src, payload, size)) fail("decoder modified its input");
    if (memcmp(dst, expected, n)) {
        for (unsigned i = 0; i < n; ++i) if (dst[i] != expected[i]) {
            fprintf(stderr, "First mismatch at byte %u: got %u, expected %u\n",
                    i, (unsigned)dst[i], (unsigned)expected[i]);
            break;
        }
        fail("decoded bytes differ from independent scalar oracle");
    }
    free(dst);
    free(src);
}

/* A compressed 64-word prefix supplies enough savings for every final-block
 * bit width/count, including bits=16 and count=1. No vector is silently tested
 * through size==n's RAW path. The target final block also exercises short
 * guarded loads at the exact end of the compressed allocation.
 */
static void numeric_case(unsigned bits, unsigned count, unsigned delta,
                         unsigned tail, unsigned pattern) {
    unsigned char packed[SCRATCH_BYTES] = {0}, expected[PAGE_BYTES];
    unsigned n = 128 + 2 * count + tail, pos = 2;
    unsigned base = pattern == 0 ? 65530u : pattern == 1 ? 32769u : 7u;
    unsigned mask = (1u << bits) - 1u, prev = base;
    uint32_t rng = 0x914bac37u ^ (bits << 20) ^ (count << 8) ^ pattern;
    snprintf(current_case, sizeof(current_case),
             "C2 bits=%u count=%u delta=%u tail=%u pattern=%u",
             bits, count, delta, tail, pattern);
    packed[0] = 0xc2;
    packed[1] = 0;
    packed[pos++] = 0;
    put16(packed + pos, 0xb7e1); pos += 2;
    for (unsigned i = 0; i < 64; ++i) put16(expected + 2 * i, 0xb7e1);
    packed[pos++] = (unsigned char)(bits | (delta ? 32u : 0u));
    put16(packed + pos, base); pos += 2;
    for (unsigned i = 0; i < count; ++i) {
        unsigned value;
        if (pattern == 0) {
            static const unsigned choices[] = {0, 65535, 1, 65534, 32768, 32767};
            value = choices[i % 6] & mask;
        } else if (pattern == 1) value = random32(&rng) & mask;
        else value = (i * 4051u + 123u) & mask;
        /* The encoder's canonical delta stream begins with zero delta. */
        if (delta && i == 0) value = 0;
        for (unsigned bit = 0; bit < bits; ++bit) {
            unsigned at = i * bits + bit;
            if ((value >> bit) & 1u)
                packed[pos + at / 8] |= (unsigned char)(1u << (at % 8));
        }
        unsigned decoded;
        if (delta) {
            /* Signed arithmetic remains in a safe int32 range, then wraps
             * explicitly to the format's 16-bit modulo arithmetic. */
            int32_t difference = (value & 1u) ? -(int32_t)((value + 1u) / 2u)
                                                 : (int32_t)(value / 2u);
            prev = (unsigned)((int32_t)prev + difference) & 65535u;
            decoded = prev;
        } else decoded = (base + value) & 65535u;
        put16(expected + 128 + 2 * i, decoded);
    }
    pos += (count * bits + 7u) / 8u;
    if (tail) {packed[pos++] = 0xd3; expected[n - 1] = 0xd3;}
    check(packed, pos, expected, n);
    ++c2_cases;
}

static void numeric_raw_block(unsigned count, unsigned tail) {
    unsigned char packed[SCRATCH_BYTES] = {0}, expected[PAGE_BYTES];
    unsigned n = 128 + 2 * count + tail, pos = 5;
    snprintf(current_case, sizeof(current_case), "C2 raw subblock count=%u tail=%u", count, tail);
    packed[0] = 0xc2; packed[1] = 0; packed[2] = 0;
    put16(packed + 3, 0x8197);
    for (unsigned i = 0; i < 64; ++i) put16(expected + 2 * i, 0x8197);
    packed[pos++] = 128;
    for (unsigned i = 0; i < 2 * count; ++i)
        packed[pos++] = expected[128 + i] = (unsigned char)(73u * i + 19u);
    if (tail) {packed[pos++] = 0x6b; expected[n - 1] = 0x6b;}
    check(packed, pos, expected, n);
    ++c2_raw_cases;
}

static void numeric_constant(unsigned words, unsigned tail) {
    unsigned char packed[5] = {0xc2, 128, 0xfe, 0xff, 0x27};
    unsigned char expected[PAGE_BYTES];
    unsigned n = 2 * words + tail, size = 4 + tail;
    snprintf(current_case, sizeof(current_case), "C2 global constant words=%u tail=%u", words, tail);
    if (size >= n) {++skipped; return;}
    for (unsigned i = 0; i < words; ++i) put16(expected + 2 * i, 65534);
    if (tail) expected[n - 1] = 0x27;
    check(packed, size, expected, n);
    ++c2_constant_cases;
}

static void extension(unsigned char *payload, unsigned *pos, unsigned value) {
    while (value >= 255) {payload[(*pos)++] = 255; value -= 255;}
    payload[(*pos)++] = (unsigned char)value;
}

/* Build two legal E5 records. The first writes nontrivial 64-byte-periodic
 * history and supplies compression savings, so short tested matches still
 * satisfy stored<n. The second is the distance/length/literal test under test.
 * rec32 exercises E5's four-byte record format, even though the current ARM
 * encoder normally emits three-byte records.
 */
static void lz_case(unsigned rec32, unsigned distance, unsigned ll,
                    unsigned ml, unsigned tail) {
    unsigned char packed[SCRATCH_BYTES] = {0}, expected[PAGE_BYTES];
    unsigned first_ll = distance > 64 ? distance : 64;
    unsigned n = first_ll + 256 + ll + ml + tail;
    unsigned rbytes = rec32 ? 4 : 3, lmask = rec32 ? 255 : 63;
    unsigned ip = 4 + 2 * rbytes, op = 0;
    snprintf(current_case, sizeof(current_case),
             "E5 record_bytes=%u distance=%u ll=%u ml=%u tail=%u n=%u",
             rbytes, distance, ll, ml, tail, n);
    if (n > PAGE_BYTES) {++skipped; return;}
    if (!distance || distance > 4095 || ml < 4) fail("invalid vector parameters");
    packed[0] = 0xe5; packed[1] = (unsigned char)rec32; put16(packed + 2, 2);
    for (unsigned record = 0; record < 2; ++record) {
        unsigned literals = record ? ll : first_ll;
        unsigned length = record ? ml : 256;
        unsigned offset = record ? distance : 64;
        unsigned mv = length - 4;
        unsigned lt = literals < lmask ? literals : lmask;
        unsigned mt = !rec32 && mv > 63 ? 63 : mv;
        uint32_t token = offset | (lt << 12) | (mt << (rec32 ? 20 : 18));
        for (unsigned byte = 0; byte < rbytes; ++byte)
            packed[4 + record * rbytes + byte] = (unsigned char)(token >> (8 * byte));
        if (literals >= lmask) extension(packed, &ip, literals - lmask);
        if (!rec32 && mv >= 63) extension(packed, &ip, mv - 63);
        uint32_t literal_rng = 0x51a397bdu ^ (record * 0x1a5387u);
        for (unsigned i = 0; i < literals; ++i) {
            unsigned char v = (unsigned char)(random32(&literal_rng) >> 9);
            packed[ip++] = expected[op++] = v;
        }
        if (offset > op) fail("match precedes available history");
        for (unsigned i = 0; i < length; ++i) {
            expected[op] = expected[op - offset];
            ++op;
        }
    }
    for (unsigned i = 0; i < tail; ++i)
        packed[ip++] = expected[op++] = (unsigned char)(0x93u + i * 37u);
    if (op != n) fail("scalar E5 output length mismatch");
    if (ip >= n) {++skipped; return;}
    check(packed, ip, expected, n);
    ++e5_cases;
}

int main(void) {
    strcpy(current_case, "initialization");
    if (!fastpage_decoder_init()) fail("decoder init");
    for (unsigned bits = 0; bits <= 16; ++bits)
        for (unsigned count = 1; count <= 64; ++count)
            for (unsigned delta = 0; delta <= 1; ++delta)
                for (unsigned tail = 0; tail <= 1; ++tail)
                    for (unsigned pattern = 0; pattern < 3; ++pattern)
                        numeric_case(bits, count, delta, tail, pattern);
    for (unsigned count = 1; count <= 64; ++count)
        for (unsigned tail = 0; tail <= 1; ++tail) {
            numeric_raw_block(count, tail);
            numeric_constant(count, tail);
        }
    static const unsigned constant_words[] = {127, 128, 129, 255, 256, 257, 1023, 1024, 2047, 2048};
    for (unsigned i = 0; i < sizeof(constant_words) / sizeof(*constant_words); ++i)
        for (unsigned tail = 0; tail <= 1; ++tail)
            if (2 * constant_words[i] + tail <= PAGE_BYTES) numeric_constant(constant_words[i], tail);

    static const unsigned lengths[] = {4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33,
        63, 64, 65, 66, 67, 68, 127, 128, 129, 255, 256, 257, 511, 512, 513, 1023};
    static const unsigned tails[] = {0, 1, 3, 16, 32, 64};
    for (unsigned rec32 = 0; rec32 <= 1; ++rec32)
        for (unsigned distance = 1; distance <= 80; ++distance)
            for (unsigned i = 0; i < sizeof(lengths) / sizeof(*lengths); ++i)
                for (unsigned t = 0; t < sizeof(tails) / sizeof(*tails); ++t)
                    lz_case(rec32, distance, 0, lengths[i], tails[t]);

    /* Literal sentinels and neighboring copy widths; tails make the remaining
     * readable/writable capacity straddle the guarded fast-path thresholds. */
    static const unsigned literals[] = {0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 62, 63, 64, 254, 255, 256, 318, 510, 511};
    static const unsigned distances[] = {7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 511, 512, 1024, 2048};
    static const unsigned short_lengths[] = {4, 8, 15, 16, 17, 31, 32, 33, 64, 67};
    for (unsigned rec32 = 0; rec32 <= 1; ++rec32) {
        for (unsigned d = 0; d < 11; ++d)
            for (unsigned l = 0; l < sizeof(literals) / sizeof(*literals); ++l)
                for (unsigned m = 0; m < sizeof(short_lengths) / sizeof(*short_lengths); ++m)
                    for (unsigned t = 0; t < sizeof(tails) / sizeof(*tails); ++t)
                        lz_case(rec32, distances[d], literals[l], short_lengths[m], tails[t]);
        for (unsigned d = 11; d < sizeof(distances) / sizeof(*distances); ++d)
            for (unsigned m = 0; m < sizeof(lengths) / sizeof(*lengths); ++m)
                for (unsigned t = 0; t < sizeof(tails) / sizeof(*tails); ++t)
                    lz_case(rec32, distances[d], 0, lengths[m], tails[t]);
        for (unsigned d = 0; d < sizeof(distances) / sizeof(*distances); ++d)
            for (unsigned t = 0; t < sizeof(tails) / sizeof(*tails); ++t) {
                unsigned seed_length = distances[d] > 64 ? distances[d] : 64;
                unsigned ml = PAGE_BYTES - seed_length - 256 - tails[t];
                lz_case(rec32, distances[d], 0, ml, tails[t]);
            }
    }
    printf("{\"status\":\"passed\",\"c2_packed_vectors\":%lu,\"c2_raw_subblocks\":%lu,"
           "\"c2_constant_vectors\":%lu,\"e5_vectors\":%lu,\"skipped_noncompressible_or_oversize\":%lu,"
           "\"exact_allocations\":true,\"encoder_called\":false}\n",
           c2_cases, c2_raw_cases, c2_constant_cases, e5_cases, skipped);
    return 0;
}
