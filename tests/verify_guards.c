/* Exact-capacity guard pages, deterministic page-local encoding, and threads.
 * Usage: verify_guards build/libfastpage.so
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { PAGE_BYTES = 4096, ENCODE_BYTES = 4352, PATTERNS = 10, THREADS = 4 };
typedef unsigned char U8;
typedef unsigned (*Encode)(const U8 *, unsigned, U8 *, unsigned *, unsigned *);
typedef int (*Decode)(const U8 *, unsigned, U8 *, unsigned);
typedef struct { void *handle; Encode encode; Decode decode; } Codec;
typedef struct { U8 *mapping, *body, *data; size_t total, span, capacity; } Guard;
typedef struct { unsigned size, baseline, mode; U8 data[ENCODE_BYTES]; } Encoded;
static size_t system_page;
static char current_case[192];
static unsigned guarded_cases, history_cases, raw_cases, e5_cases, c2_cases;

static void fail(const char *why) {
    fprintf(stderr, "FAIL: %s: %s\n", current_case, why);
    exit(1);
}

static Codec load_codec(const char *path) {
    Codec c;
    c.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!c.handle) fail(dlerror());
    int (*init)(void) = (int (*)(void))dlsym(c.handle, "codec_init");
    c.encode = (Encode)dlsym(c.handle, "codec_encode");
    c.decode = (Decode)dlsym(c.handle, "codec_decode");
    if (!init || !c.encode || !c.decode || !init()) fail("library API/init");
    return c;
}

static uint32_t next_random(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *state = x;
}

static void pattern(U8 *dst, unsigned n, unsigned kind, unsigned salt) {
    uint32_t rng = 0x53db8a91u ^ salt ^ (kind * 0x9e3779b9u);
    for (unsigned i = 0; i < n; ++i) {
        U8 r = (U8)next_random(&rng);
        switch (kind) {
        case 0: dst[i] = 0; break;
        case 1: dst[i] = 255; break;
        case 2: dst[i] = r; break;
        case 3: dst[i] = (U8)((i % 7u) * 37u + 129u); break;
        case 4: dst[i] = i < 32 ? r : dst[i - 32]; break;
        case 5: dst[i] = i % 193u < 16u ? r : (U8)0xb7; break;
        case 6: dst[i] = (U8)(r & 15u); break;
        case 7: dst[i] = (U8)((i & 1u) ? (i / 2u) >> 8 : i / 2u); break;
        case 8: dst[i] = i < (n + 1u) / 2u ? r : dst[i - (n + 1u) / 2u]; break;
        default: dst[i] = i < 64 || i % 127u == 0 ? r : dst[i - 61]; break;
        }
    }
}

static Encoded encode_page(const Codec *c, const U8 *src, unsigned n) {
    Encoded e;
    memset(&e, 0xa5, sizeof(e));
    e.size = c->encode(src, n, e.data, &e.baseline, &e.mode);
    if (!e.size || e.size > n || e.baseline != e.size ||
        ((e.size == n) != (e.mode == 255))) fail("invalid encoded size/metadata");
    return e;
}

static int same(const Encoded *a, const Encoded *b) {
    return a->size == b->size && a->baseline == b->baseline &&
           a->mode == b->mode && !memcmp(a->data, b->data, a->size);
}

static Guard guard_new(size_t capacity, int trailing) {
    Guard g;
    g.capacity = capacity;
    g.span = ((capacity + system_page - 1) / system_page) * system_page;
    g.total = g.span + 2 * system_page;
    g.mapping = mmap(NULL, g.total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g.mapping == MAP_FAILED) fail("guard mmap");
    g.body = g.mapping + system_page;
    if (mprotect(g.body, g.span, PROT_READ | PROT_WRITE)) fail("guard mprotect");
    memset(g.body, 0xa5, g.span);
    g.data = trailing ? g.body + g.span - capacity : g.body;
    return g;
}

static void guard_readonly(Guard *g) {
    if (mprotect(g->body, g->span, PROT_READ)) fail("read-only input mprotect");
}

static void guard_check(const Guard *g) {
    for (const U8 *p = g->body; p < g->data; ++p)
        if (*p != 0xa5) fail("write before advertised capacity");
    for (const U8 *p = g->data + g->capacity; p < g->body + g->span; ++p)
        if (*p != 0xa5) fail("write after advertised capacity");
}

static void guard_free(Guard *g) {
    if (munmap(g->mapping, g->total)) fail("guard munmap");
}

static void guarded_roundtrip(const Codec *c, const U8 *plain,
                              unsigned n, const Encoded *expected, int trailing) {
    Guard src = guard_new(n, trailing), dst = guard_new(ENCODE_BYTES, trailing);
    memcpy(src.data, plain, n);
    guard_readonly(&src);
    unsigned baseline = 0, mode = 0;
    unsigned size = c->encode(src.data, n, dst.data, &baseline, &mode);
    if (size != expected->size || baseline != expected->baseline || mode != expected->mode ||
        memcmp(dst.data, expected->data, size)) fail("guarded encoding differs");
    guard_check(&dst);
    Guard stored = guard_new(size, trailing), out = guard_new(n, trailing);
    memcpy(stored.data, dst.data, size);
    guard_readonly(&stored);
    if (!c->decode(stored.data, size, out.data, n) || memcmp(out.data, plain, n))
        fail("exact-capacity roundtrip");
    guard_check(&out);
    guard_free(&out); guard_free(&stored); guard_free(&dst); guard_free(&src);
    ++guarded_cases;
}

typedef struct { const Codec *codec; unsigned id; Encoded expected; U8 plain[PAGE_BYTES]; } Worker;
static void *worker(void *opaque) {
    Worker *w = opaque;
    U8 disturbance[PAGE_BYTES], scratch[ENCODE_BYTES], decoded[PAGE_BYTES];
    for (unsigned round = 0; round < 128; ++round) {
        unsigned baseline = 0, mode = 0;
        pattern(disturbance, PAGE_BYTES, (round + w->id) % PATTERNS, round + 1);
        (void)w->codec->encode(disturbance, PAGE_BYTES, scratch, &baseline, &mode);
        unsigned size = w->codec->encode(w->plain, PAGE_BYTES, scratch, &baseline, &mode);
        if (size != w->expected.size || baseline != w->expected.baseline ||
            mode != w->expected.mode || memcmp(scratch, w->expected.data, size) ||
            !w->codec->decode(scratch, size, decoded, PAGE_BYTES) ||
            memcmp(decoded, w->plain, PAGE_BYTES)) return (void *)(uintptr_t)1;
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) fail("usage: verify_guards LIBRARY.so");
    long pagesize = sysconf(_SC_PAGESIZE);
    if (pagesize <= 0) fail("system page size");
    system_page = (size_t)pagesize;
    Codec primary = load_codec(argv[1]);
    /* Invalid sizes must return before dereferencing even inaccessible pointers. */
    U8 *inaccessible = mmap(NULL, system_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (inaccessible == MAP_FAILED) fail("invalid-size mmap");
    snprintf(current_case, sizeof(current_case), "invalid encoder lengths with PROT_NONE buffers");
    if (primary.encode(inaccessible, 0, inaccessible, NULL, NULL) ||
        primary.encode(inaccessible, 4097, inaccessible, NULL, NULL)) fail("invalid length accepted");
    if (munmap(inaccessible, system_page)) fail("invalid-size munmap");
    static const unsigned lengths[] = {
        1, 7, 8, 9, 15, 16, 17, 23, 24, 25, 31, 32, 33, 39, 40, 41,
        63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 513,
        1023, 1024, 1025, 2047, 2048, 2049, 4063, 4064, 4065,
        4071, 4072, 4073, 4080, 4081, 4087, 4088, 4089, 4094, 4095, 4096
    };
    U8 plain[PAGE_BYTES], disturbance[PAGE_BYTES];
    for (unsigned kind = 0; kind < PATTERNS; ++kind)
        for (unsigned i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
            unsigned n = lengths[i];
            snprintf(current_case, sizeof(current_case), "pattern=%u n=%u", kind, n);
            pattern(plain, n, kind, 0);
            Encoded expected = encode_page(&primary, plain, n);
            if (expected.mode == 255) ++raw_cases;
            else if (expected.mode == 0xe5) ++e5_cases;
            else if (expected.mode == 0xc2) ++c2_cases;
            else fail("unknown mode");
            for (int edge = 0; edge < 2; ++edge) {
                snprintf(current_case, sizeof(current_case), "pattern=%u n=%u %s guard", kind, n,
                         edge ? "trailing" : "leading");
                guarded_roundtrip(&primary, plain, n, &expected, edge);
            }
        }
    if (!e5_cases || !raw_cases) fail("patterns failed to cover E5 and RAW");
    for (unsigned kind = 0; kind < PATTERNS; ++kind) {
        pattern(plain, PAGE_BYTES, kind, 0);
        Encoded expected = encode_page(&primary, plain, PAGE_BYTES);
        /* Interleave unrelated pages and lengths before encoding the same page. */
        for (unsigned round = 0; round < 41; ++round) {
            snprintf(current_case, sizeof(current_case), "history pattern=%u round=%u", kind, round);
            unsigned n = round & 1u ? PAGE_BYTES : 16u + (round * 97u) % 4081u;
            pattern(disturbance, n, round % PATTERNS, round + 1);
            (void)encode_page(&primary, disturbance, n);
            Encoded repeated = encode_page(&primary, plain, PAGE_BYTES);
            if (!same(&expected, &repeated)) fail("encoding depends on call history");
            ++history_cases;
        }
    }
    snprintf(current_case, sizeof(current_case), "concurrent same-library encoding");
    pthread_t threads[THREADS];
    Worker workers[THREADS];
    for (unsigned i = 0; i < THREADS; ++i) {
        workers[i].codec = &primary; workers[i].id = i;
        pattern(workers[i].plain, PAGE_BYTES, i + 2, 0);
        workers[i].expected = encode_page(&primary, workers[i].plain, PAGE_BYTES);
    }
    for (unsigned i = 0; i < THREADS; ++i)
        if (pthread_create(&threads[i], NULL, worker, &workers[i])) fail("pthread_create");
    for (unsigned i = 0; i < THREADS; ++i) {
        void *result;
        if (pthread_join(threads[i], &result) || result) fail("concurrent bytes/roundtrip differ");
    }
    printf("{\"status\":\"passed\",\"guarded_roundtrips\":%u,\"invalid_sizes_rejected\":2,"
           "\"history_checks\":%u,\"concurrent_checks\":512,"
           "\"raw_pages\":%u,\"e5_pages\":%u,\"c2_pages\":%u,\"system_page_bytes\":%zu}\n",
           guarded_cases, history_cases, raw_cases, e5_cases, c2_cases, system_page);
    dlclose(primary.handle);
    return 0;
}
