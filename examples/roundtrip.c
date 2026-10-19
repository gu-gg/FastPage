#include <stdio.h>
#include <string.h>
#include "codec.h"

int main(void) {
    unsigned char input[4096], stored[4352], output[4096];
    const char text[] = "Independent 4 KiB page compression demonstration. ";
    for (unsigned i = 0; i < sizeof(input); ++i)
        input[i] = (unsigned char)text[i % (sizeof(text) - 1)];
    if (!codec_init()) { fputs("codec_init failed\n", stderr); return 1; }
    unsigned lzsize = 0, mode = 0;
    unsigned size = codec_encode(input, sizeof(input), stored, &lzsize, &mode);
    if (!size || size > sizeof(input)) { fputs("invalid encoded length\n", stderr); return 1; }
    if (!codec_decode(stored, size, output, sizeof(output)) || memcmp(input, output, sizeof(input))) {
        fputs("round trip failed\n", stderr); return 1;
    }
    printf("round trip passed: input=%zu stored=%u mode=%u ratio=%.3f\n",
           sizeof(input), size, mode, (double)sizeof(input)/size);
    return 0;
}
