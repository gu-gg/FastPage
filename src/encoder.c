/* FastPage: independent 4 KiB page compression on little-endian AArch64.
 * Match search follows the LZ4 independent-block U16-table/catch-up scheme;
 * see ../licenses/LZ4_LICENSE.txt. This bitstream is not LZ4-compatible.
 * All state is local to one call; input and output buffers must not overlap.
 */
#include "codec.h"
#include <stdint.h>
#include <string.h>
#if !defined(__aarch64__) || !defined(__ARM_NEON) || \
    __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error FastPage requires little-endian AArch64 with NEON
#endif
#if !defined(__ARM_FEATURE_CRC32)
#error FastPage requires CRC32 support; compile with -march=armv8-a+crc
#endif
#include <arm_neon.h>
#include <arm_acle.h>

typedef unsigned char U8;
static uint32_t read32(const void *p) {uint32_t v;memcpy(&v,p,4);return v;}
static uint64_t read64(const void *p) {uint64_t v;memcpy(&v,p,8);return v;}
static unsigned get16(const U8 *p) {return p[0]|((unsigned)p[1]<<8);}
static void put16(U8 *p,unsigned v) {p[0]=(U8)v;p[1]=(U8)(v>>8);}
static unsigned hash_word(uint64_t v) {
    return __crc32cd(0,v);
}
static unsigned hash(const U8 *p) {return hash_word(read64(p));}
#define HASH_SLOT(h) ((h)>>20)

typedef uint16_t HashEntry;

static int equal_word(const U8 *a,uint64_t v) {
    return read64(a)==v;
}
static int equal_min(const U8 *a,const U8 *b) {
    return read64(a)==read64(b);
}
static unsigned count_match(const U8 *s,unsigned p,unsigned q,unsigned end) {
    unsigned begin=p;
    while(p+8<=end) {
        uint64_t x=read64(s+p)^read64(s+q);
        if(x)return p-begin+(unsigned)__builtin_ctzll(x)/8;
        p+=8;q+=8;
    }
    if(p+4<=end) {
        uint32_t x=read32(s+p)^read32(s+q);
        if(x)return p-begin+(unsigned)__builtin_ctz(x)/8;
        p+=4;q+=4;
    }
    while(p<end&&s[p]==s[q]){p++;q++;}
    return p-begin;
}
static unsigned extension(U8 *p,unsigned x) {
    unsigned n=0;while(x>=255){p[n++]=255;x-=255;}p[n++]=(U8)x;return n;
}
static unsigned extbytes(unsigned x) {return x/255+1;}
static unsigned raw(const U8 *src,unsigned n,U8 *dst,unsigned *lzsize,unsigned *mode) {
    memcpy(dst,src,n);if(lzsize)*lzsize=n;if(mode)*mode=255;return n;
}

#include "numeric_encode.inc"

extern int fastpage_decoder_init(void);
int codec_init(void) {return fastpage_decoder_init();}

/* Route numeric pages to C2; otherwise search the page-local hash table and
 * emit E5 metadata followed by its extension/literal payload. RAW is used
 * whenever the selected representation would not be smaller than the input. */
unsigned codec_encode(const U8 *src,unsigned n,U8 *dst,unsigned *lzsize,unsigned *mode) {
    if(!n||n>4096)return 0;
    if(n<16)return raw(src,n,dst,lzsize,mode);
    if(numeric_candidate(src,n)) {
        unsigned size=encode_words(src,n,dst);
        if(lzsize)*lzsize=size;
        if(mode)*mode=size==n?255:0xc2;
        return size;
    }
    /* 4096 page-local positions: high 12 bits of a seeded CRC32C hash. */
    HashEntry table[1u<<12];
    U8 payload[4096];
    U8 *records=dst+4;
    unsigned ip=1,anchor=0,nm=0,pp=0;
    const unsigned rbytes=3,lmask=63;
    memset(table,0,sizeof(table));

    uint64_t next_word=read64(src+ip);
    unsigned next_hash=hash_word(next_word);
    for(;;) {
        unsigned candidate,forward=ip,step=1;

        if(forward+16<=n-8) {
#pragma GCC unroll 16
            /* No adaptive skip arithmetic for these first 16 probes. */
            for(unsigned probe=0;probe<16;probe++) {
                unsigned h=next_hash;HashEntry entry=table[HASH_SLOT(h)];candidate=entry;ip=forward++;
                uint64_t word=next_word;next_word=read64(src+forward);next_hash=hash_word(next_word);
                table[HASH_SLOT(h)]=ip;
                if(equal_word(src+candidate,word))goto found_match;
            }
        }

        step=2;
        for(;;step++) {
#pragma GCC unroll 32
            for(unsigned probe=0;probe<28;probe++) {
                unsigned h=next_hash;
                HashEntry entry=table[HASH_SLOT(h)];candidate=entry;ip=forward;
                forward+=step;
                if(forward>n-8)goto finish;
                uint64_t word=next_word;next_word=read64(src+forward);next_hash=hash_word(next_word);
                table[HASH_SLOT(h)]=ip;
                if(equal_word(src+candidate,word))goto found_match;
            }
        }
found_match:;
        while(ip>anchor&&candidate>0&&src[ip-1]==src[candidate-1]){ip--;candidate--;}
next_match:;
        unsigned ll=ip-anchor,ml=8+count_match(src,ip+8,candidate+8,n-5),mv=ml-4;
        unsigned eb=(ll>=lmask?extbytes(ll-lmask):0)+(mv>=63?extbytes(mv-63):0);
        if(4+(nm+1)*rbytes+pp+eb+ll>=n)return raw(src,n,dst,lzsize,mode);
        uint32_t token=(ip-candidate)|((ll<lmask?ll:lmask)<<12)|((mv<63?mv:63)<<18);
        U8 *rec=records+nm*rbytes;
        rec[0]=(U8)token;rec[1]=(U8)(token>>8);rec[2]=(U8)(token>>16);
        if(ll>=lmask)pp+=extension(payload+pp,ll-lmask);
        if(mv>=63)pp+=extension(payload+pp,mv-63);
        if(ll) {
            if(ll<=16&&anchor+16<=n&&pp+16<=4096)
                memcpy(payload+pp,src+anchor,16);
            else memcpy(payload+pp,src+anchor,ll);
        }
        pp+=ll;nm++;
        ip+=ml;anchor=ip;
        if(ip>=n-8)break;
        unsigned insert_hash=hash(src+ip-2);
        table[HASH_SLOT(insert_hash)]=ip-2;
        unsigned h=hash(src+ip);HashEntry entry=table[HASH_SLOT(h)];candidate=entry;
        table[HASH_SLOT(h)]=ip;
        if(equal_min(src+candidate,src+ip))goto next_match;
        ip++;
        next_word=read64(src+ip);next_hash=hash_word(next_word);
    }
finish:;
    unsigned tail=n-anchor,size=4+nm*rbytes+pp+tail;
    if(size>=n)return raw(src,n,dst,lzsize,mode);
    dst[0]=0xe5;dst[1]=0;put16(dst+2,nm);
    memcpy(dst+4+nm*rbytes,payload,pp);
    memcpy(dst+4+nm*rbytes+pp,src+anchor,tail);
    if(lzsize)*lzsize=size;
    if(mode)*mode=0xe5;
    return size;
}
