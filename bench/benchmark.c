/* Single-codec Silesia page benchmark. Corpus checks and round-trip comparisons
 * are outside the timed regions. All pages, including RAW pages, go through
 * the public codec function pointers. Linux/POSIX, no third-party libraries. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef unsigned char U8;
typedef unsigned (*encode_fn)(const U8 *, unsigned, U8 *, unsigned *, unsigned *);
typedef int (*decode_fn)(const U8 *, unsigned, U8 *, unsigned);
typedef struct { void *library; encode_fn encode; decode_fn decode; } Codec;
typedef struct { U8 *source; unsigned n, stored; } Block;
enum { PAGE_BYTES = 4096, SLOT_BYTES = 5120, FILE_COUNT = 12, MAX_BLOCKS = 52000 };
static const char *names[FILE_COUNT] = {
    "dickens", "mozilla", "mr", "nci", "ooffice", "osdb", "reymont", "samba",
    "sao", "webster", "xml", "x-ray"
};
static const uint64_t file_sizes[FILE_COUNT] = {
    10192446, 51220480, 9970564, 33553445, 6152192, 10085684,
    6627202, 21606400, 7251944, 41458703, 5345280, 8474240
};
static Block *blocks;
static U8 *files[FILE_COUNT], *compressed, *restored;
static size_t nblocks, file_starts[FILE_COUNT + 1];
static uint64_t input_bytes, initial_stored;
static volatile uint64_t observed;

static void fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(EXIT_FAILURE);
}
static unsigned parse_unsigned(const char *text, unsigned maximum) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !*text || *text == '-' || *end || value > maximum)
        fail("invalid integer argument");
    return (unsigned)value;
}
static double parse_seconds(const char *text) {
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (errno || !*text || *end || !isfinite(value) || value <= 0 || value > 60)
        fail("seconds must be finite and in (0,60]");
    return value;
}
static void *aligned_bytes(size_t bytes) {
    void *memory = NULL;
    if (posix_memalign(&memory, 64, bytes)) fail("allocation");
    return memory;
}
static void pin_cpu(unsigned cpu) {
    cpu_set_t allowed, chosen;
    CPU_ZERO(&allowed);
    CPU_ZERO(&chosen);
    if (sched_getaffinity(0, sizeof(allowed), &allowed)) fail("get CPU affinity");
    if (cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &allowed)) fail("CPU is not allowed");
    CPU_SET(cpu, &chosen);
    if (sched_setaffinity(0, sizeof(chosen), &chosen)) fail("set CPU affinity");
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) ||
        CPU_COUNT(&allowed) != 1 || !CPU_ISSET(cpu, &allowed)) fail("CPU affinity check");
}
static void *symbol(void *library, const char *name) {
    dlerror();
    void *address = dlsym(library, name);
    const char *error = dlerror();
    if (error || !address) fail(error ? error : name);
    return address;
}
#define LOAD_FUNCTION(target, library, name) do { \
    void *address = symbol((library), (name)); \
    _Static_assert(sizeof(target) == sizeof(address), "POSIX function pointer size"); \
    memcpy(&(target), &address, sizeof(target)); \
} while (0)
static Codec load_codec(const char *path) {
    Codec codec = {0};
    codec.library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!codec.library) fail(dlerror());
    int (*init)(void);
    LOAD_FUNCTION(init, codec.library, "codec_init");
    LOAD_FUNCTION(codec.encode, codec.library, "codec_encode");
    LOAD_FUNCTION(codec.decode, codec.library, "codec_decode");
    if (!init()) fail("codec initialization");
    return codec;
}
static void load_corpus(const char *directory) {
    blocks = calloc(MAX_BLOCKS, sizeof(*blocks));
    if (!blocks) fail("block directory allocation");
    for (unsigned f = 0; f < FILE_COUNT; ++f) {
        char path[4096];
        int count = snprintf(path, sizeof(path), "%s/%s", directory, names[f]);
        if (count < 0 || (size_t)count >= sizeof(path)) fail("corpus path too long");
        FILE *in = fopen(path, "rb");
        if (!in) fail(path);
        if (fseek(in, 0, SEEK_END) || ftell(in) != (long)file_sizes[f])
            fail("corpus size mismatch; run tools/fetch_corpus.py --verify-only");
        rewind(in);
        size_t size = (size_t)file_sizes[f];
        U8 *data = files[f] = aligned_bytes(size);
        if (fread(data, 1, size, in) != size) fail("corpus read");
        if (fclose(in)) fail("corpus close");
        file_starts[f] = nblocks;
        for (size_t offset = 0; offset < size; offset += PAGE_BYTES) {
            if (nblocks >= MAX_BLOCKS) fail("block directory capacity");
            unsigned n = (unsigned)(size - offset < PAGE_BYTES ? size - offset : PAGE_BYTES);
            blocks[nblocks++] = (Block){data + offset, n, 0};
        }
        input_bytes += size;
    }
    file_starts[FILE_COUNT] = nblocks;
    compressed = aligned_bytes(nblocks * SLOT_BYTES);
    restored = aligned_bytes(nblocks * PAGE_BYTES);
    memset(compressed, 0, nblocks * SLOT_BYTES);
    memset(restored, 0, nblocks * PAGE_BYTES);
}
__attribute__((noinline)) static uint64_t compress_all(const Codec *codec) {
    uint64_t total = 0;
    for (size_t i = 0; i < nblocks; ++i) {
        Block *block = blocks + i;
        unsigned lzsize, mode;
        block->stored = codec->encode(block->source, block->n,
                                     compressed + i * SLOT_BYTES, &lzsize, &mode);
        if (!block->stored || block->stored > block->n) fail("invalid encoded size");
        total += block->stored;
    }
    observed = total;
    __asm__ __volatile__("" : : "r"(compressed) : "memory");
    return total;
}
__attribute__((noinline)) static void decompress_all(const Codec *codec) {
    for (size_t i = 0; i < nblocks; ++i) {
        const Block *block = blocks + i;
        if (!codec->decode(compressed + i * SLOT_BYTES, block->stored,
                           restored + i * PAGE_BYTES, block->n)) fail("decode rejected a page");
    }
    __asm__ __volatile__("" : : "r"(restored) : "memory");
}
static void decompress_reverse(const Codec *codec) {
    for (size_t i = nblocks; i-- > 0;) {
        const Block *block = blocks + i;
        if (!codec->decode(compressed + i * SLOT_BYTES, block->stored,
                           restored + i * PAGE_BYTES, block->n)) fail("reverse decode rejected a page");
    }
}
static void verify_all(void) {
    for (size_t i = 0; i < nblocks; ++i)
        if (memcmp(blocks[i].source, restored + i * PAGE_BYTES, blocks[i].n))
            fail("round-trip mismatch");
}
static double now(clockid_t clock) {
    struct timespec time;
    if (clock_gettime(clock, &time)) fail("clock_gettime");
    return (double)time.tv_sec + (double)time.tv_nsec * 1e-9;
}
static void pass(const Codec *codec, unsigned operation) {
    if (operation == 0) compress_all(codec);
    else decompress_all(codec);
}
static void measure(const Codec *codec, unsigned operation, unsigned round, double target) {
    double start = now(CLOCK_MONOTONIC);
    pass(codec, operation);
    double warm = now(CLOCK_MONOTONIC) - start;
    if (operation) verify_all();
    double estimate = ceil(target / (warm > 1e-6 ? warm : 1e-6));
    unsigned loops = estimate > 64 ? 64 : estimate < 1 ? 1 : (unsigned)estimate;
    double cpu0 = now(CLOCK_THREAD_CPUTIME_ID), wall0 = now(CLOCK_MONOTONIC);
    for (unsigned repeat = 0; repeat < loops; ++repeat) pass(codec, operation);
    double wall = now(CLOCK_MONOTONIC) - wall0;
    double cpu = now(CLOCK_THREAD_CPUTIME_ID) - cpu0;
    if (wall <= 0 || cpu <= 0) fail("nonpositive measured duration");
    /* Validation and stdout are deliberately outside the measured interval. */
    if (observed != initial_stored) fail("encoded total changed during timing");
    if (!operation) decompress_all(codec);
    verify_all();
    printf("{\"type\":\"sample\",\"round\":%u,\"operation\":\"%s\",\"loops\":%u,"
           "\"input_bytes_per_loop\":%" PRIu64 ",\"warmup_wall_seconds\":%.9f,"
           "\"target_seconds\":%.9f,\"calibration_loop_limit\":64,"
           "\"wall_seconds\":%.9f,\"thread_cpu_seconds\":%.9f,"
           "\"wall_MBps\":%.6f,\"thread_cpu_MBps\":%.6f}\n",
           round, operation ? "decompress" : "compress", loops, input_bytes, warm,
           target, wall, cpu, (double)input_bytes * loops / wall / 1e6,
           (double)input_bytes * loops / cpu / 1e6);
    fflush(stdout);
}
int main(int argc, char **argv) {
    if (argc != 7)
        fail("usage: benchmark CORPUS LIBRARY CPU ROUND SECONDS {compress-first|decompress-first|verify}");
    unsigned cpu = parse_unsigned(argv[3], CPU_SETSIZE - 1);
    unsigned round = parse_unsigned(argv[4], 100);
    double seconds = parse_seconds(argv[5]);
    if (!round) fail("round must be positive");
    int verify_only = !strcmp(argv[6], "verify");
    int decode_first = !strcmp(argv[6], "decompress-first");
    if (!verify_only && !decode_first && strcmp(argv[6], "compress-first")) fail("invalid operation order");
    pin_cpu(cpu);
    Codec codec = load_codec(argv[2]);
    load_corpus(argv[1]);
    printf("{\"type\":\"environment\",\"round\":%u,\"cpu_affinity\":%u,"
           "\"input_bytes\":%" PRIu64 ",\"blocks\":%zu,\"block_bytes\":%u,"
           "\"compressed_slot_stride\":%u,\"decoded_slot_stride\":%u,"
           "\"order\":\"%s\",\"verify_only\":%s}\n",
           round, cpu, input_bytes, nblocks, PAGE_BYTES, SLOT_BYTES, PAGE_BYTES,
           argv[6], verify_only ? "true" : "false");
    fflush(stdout);
    initial_stored = compress_all(&codec);
    decompress_all(&codec);
    verify_all();
    memset(restored, 0xa5, nblocks * PAGE_BYTES);
    decompress_reverse(&codec);
    verify_all();
    uint64_t raw = 0;
    for (size_t i = 0; i < nblocks; ++i) raw += blocks[i].stored == blocks[i].n;
    printf("{\"type\":\"ratio\",\"round\":%u,\"input_bytes\":%" PRIu64 ","
           "\"stored_payload_bytes\":%" PRIu64 ",\"ratio\":%.9f,\"raw_blocks\":%" PRIu64 ","
           "\"roundtrip\":\"passed\"}\n", round, input_bytes, initial_stored,
           (double)input_bytes / initial_stored, raw);
    for (unsigned f = 0; f < FILE_COUNT; ++f) {
        uint64_t stored = 0, file_raw = 0;
        for (size_t i = file_starts[f]; i < file_starts[f + 1]; ++i) {
            stored += blocks[i].stored;
            file_raw += blocks[i].stored == blocks[i].n;
        }
        printf("{\"type\":\"file_ratio\",\"round\":%u,\"file\":\"%s\","
               "\"input_bytes\":%" PRIu64 ",\"stored_bytes\":%" PRIu64 ","
               "\"blocks\":%zu,\"raw_blocks\":%" PRIu64 "}\n",
               round, names[f], file_sizes[f], stored,
               file_starts[f + 1] - file_starts[f], file_raw);
    }
    fflush(stdout);
    if (!verify_only) {
        measure(&codec, decode_first ? 1 : 0, round, seconds);
        measure(&codec, decode_first ? 0 : 1, round, seconds);
    }
    printf("{\"type\":\"validation\",\"round\":%u,\"forward_roundtrip\":\"passed\","
           "\"reverse_roundtrip\":\"passed\",\"timing_samples\":%u,\"result\":\"passed\"}\n",
           round, verify_only ? 0 : 2);
    fflush(stdout);
    for (unsigned f = 0; f < FILE_COUNT; ++f) free(files[f]);
    free(blocks);
    free(compressed);
    free(restored);
    if (dlclose(codec.library)) fail("dlclose");
    return EXIT_SUCCESS;
}
