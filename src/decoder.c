/* FastPage: independent 4 KiB page compression on little-endian AArch64.
 * Decode RAW, C2 numeric blocks and E5 LZ records using exact-size buffers.
 * This bitstream is not LZ4-compatible; see ../licenses/LZ4_LICENSE.txt.
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

typedef unsigned char U8;
static uint32_t read32(const void *p) {uint32_t v;memcpy(&v,p,4);return v;}
static unsigned get16(const U8 *p) {return p[0]|((unsigned)p[1]<<8);}
static void put16(U8 *p,unsigned v) {p[0]=(U8)v;p[1]=(U8)(v>>8);}

#include "numeric_decode.inc"
/* All numeric tables are constant, so initialization is also thread-safe. */
int fastpage_decoder_init(void) {return 1;}

static void literals(U8 *dst,const U8 *src,unsigned len,unsigned wr,unsigned rd) {
    if(len<=16&&wr>=16&&rd>=16) {memcpy(dst,src,16);return;}
    if(len<=32&&wr>=32&&rd>=32) {memcpy(dst,src,32);return;}
    memcpy(dst,src,len);
}

/* Index from the last sixteen valid output bytes into one complete cycle.
 * Only indices [16-distance,15] are selected, so neither uninitialized output
 * nor memory before the advertised output allocation is used. */
#define PERIOD_ROW(D) {16-(D)+0%(D),16-(D)+1%(D),16-(D)+2%(D),16-(D)+3%(D),16-(D)+4%(D),16-(D)+5%(D),16-(D)+6%(D),16-(D)+7%(D),16-(D)+8%(D),16-(D)+9%(D),16-(D)+10%(D),16-(D)+11%(D),16-(D)+12%(D),16-(D)+13%(D),16-(D)+14%(D),16-(D)+15%(D)}
static const U8 arm64_period_index[16][16] __attribute__((aligned(16)))={
    {0},PERIOD_ROW(1),PERIOD_ROW(2),PERIOD_ROW(3),
    PERIOD_ROW(4),PERIOD_ROW(5),PERIOD_ROW(6),PERIOD_ROW(7),
    PERIOD_ROW(8),PERIOD_ROW(9),PERIOD_ROW(10),PERIOD_ROW(11),
    PERIOD_ROW(12),PERIOD_ROW(13),PERIOD_ROW(14),PERIOD_ROW(15)
};
/* Replace a short distance with the smallest multiple at least sixteen. */
static const U8 arm64_long_period[16]={0,16,16,18,16,20,18,21,16,18,20,22,24,26,28,30};
#undef PERIOD_ROW

static void match_copy(U8 *dst,unsigned distance,unsigned len,unsigned wr,unsigned history) {
    if(distance>1&&distance<16&&history>=16&&wr>=16) {
        uint8x16_t pattern=vqtbl1q_u8(vld1q_u8(dst-16),vld1q_u8(arm64_period_index[distance]));
        vst1q_u8(dst,pattern);
        if(len<=16)return;
        /* The just-written cycle is valid LZ output. A multiple of the old
         * distance denotes the same byte sequence and permits disjoint
         * 16-byte memcpy chunks below, including at the exact page end. */
        dst+=16;len-=16;wr-=16;
        distance=arm64_long_period[distance];
    }

    const U8 *src=dst-distance;
    if(distance>=16&&len<=32&&wr>=32) {
        memcpy(dst,src,16);if(len>16)memcpy(dst+16,src+16,16);return;
    }
    if(distance>=8&&len<=16&&wr>=16) {
        memcpy(dst,src,8);if(len>8)memcpy(dst+8,src+8,8);return;
    }
    if(distance>=len){memcpy(dst,src,len);return;}
    if(distance==1){memset(dst,*src,len);return;}
    unsigned i=0;
    if(distance>=16) {
        for(;i+16<=len;i+=16)memcpy(dst+i,src+i,16);
    }
    else if(distance>=8)for(;i+8<=len;i+=8)memcpy(dst+i,src+i,8);
    else if(distance>=4)for(;i+4<=len;i+=4)memcpy(dst+i,src+i,4);
    for(;i<len;i++)dst[i]=src[i];
}

static int read_extension(const U8 **ipp,const U8 *iend,unsigned *value) {
    unsigned b;
    do {
        if(*ipp==iend)return 0;
        b=*(*ipp)++;*value+=b;
        if(*value>4096)return 0;
    }while(b==255);
    return 1;
}
static __attribute__((always_inline)) inline int decode_general(const U8 **ipp,
        const U8 *iend,U8 **opp,U8 *dst,U8 *oend,unsigned rec32,
        unsigned ll,unsigned ml,unsigned distance) {
    const U8 *ip=*ipp;U8 *op=*opp;
    unsigned lmask=rec32?255:63;
    if(ll==lmask&&!read_extension(&ip,iend,&ll))return 0;
    if(!rec32&&ml==63&&!read_extension(&ip,iend,&ml))return 0;
    ml+=4;
    unsigned rd=(unsigned)(iend-ip),wr=(unsigned)(oend-op);
    if(ll>rd||ll>wr)return 0;
    if(ll)literals(op,ip,ll,wr,rd);
    ip+=ll;op+=ll;wr-=ll;
    unsigned history=(unsigned)(op-dst);
    if(!distance||distance>history||ml>wr)return 0;
    match_copy(op,distance,ml,wr,history);op+=ml;
    *ipp=ip;*opp=op;return 1;
}

/* Fast paths reserve the complete widened copy sizes. The general path
 * parses extensions and handles short distances and exact page boundaries. */
static __attribute__((always_inline)) inline int decode_record(
        const U8 **ipp,const U8 *iend,U8 **opp,U8 *dst,U8 *oend,
        unsigned rec32,uint32_t t) {
    const U8 *ip=*ipp;U8 *op=*opp;
    unsigned lmask=rec32?255:63;
    if(!rec32)t&=0xffffff;
    unsigned ll=(t>>12)&lmask,ml=rec32?t>>20:(t>>18)&63,distance=t&4095;
    unsigned history=(unsigned)(op-dst);

    unsigned rd=(unsigned)(iend-ip),wr=(unsigned)(oend-op);

    unsigned short_ok=ll<=32&&
        ml<=28&&distance>=16&&
        distance<=history+ll&&
        rd>=32&&wr>=ll+32;

    if(short_ok) {
        memcpy(op,ip,32);ip+=ll;op+=ll;

        /* Ordered disjoint chunks preserve distances16..31 repetition. */
        memcpy(op,op-distance,16);
        memcpy(op+16,op-distance+16,16);

        op+=ml+4;

        *ipp=ip;*opp=op;return 1;
    }

    if(ll<=62&&ml<=60&&
       distance>=64&&distance<=history+ll&&
       rd>=64&&wr>=ll+64) {
        memcpy(op,ip,64);ip+=ll;op+=ll;
        memcpy(op,op-distance,64);op+=ml+4;

        *ipp=ip;*opp=op;return 1;
    }

    if(!decode_general(&ip,iend,&op,dst,oend,rec32,ll,ml,distance))return 0;

    *ipp=ip;*opp=op;return 1;
}
static __attribute__((always_inline)) inline int decode_records(
        const U8 *src,unsigned size,U8 *dst,unsigned n,unsigned rec32) {
    unsigned rbytes=rec32?4:3,nm=get16(src+2);
    /* This same trailing byte also guards the final 24-bit read32 load. */
    if(nm>n/4||4+nm*rbytes>=size)return 0;
    const U8 *meta=src+4,*ip=meta+nm*rbytes,*iend=src+size;
    U8 *op=dst,*oend=dst+n;

    if(nm) {
        uint32_t t=read32(meta);meta+=rbytes;
        for(unsigned left=nm;left>1;--left) {
            uint32_t next=read32(meta);meta+=rbytes;
            if(!decode_record(&ip,iend,&op,dst,oend,rec32,t))return 0;
            t=next;
        }
        if(!decode_record(&ip,iend,&op,dst,oend,rec32,t))return 0;
    }

    if(iend-ip!=oend-op)return 0;
    memcpy(op,ip,(size_t)(oend-op));return 1;
}

int codec_decode(const U8 *src,unsigned size,U8 *dst,unsigned n) {
    if(!n||n>4096||size>n)return 0;
    if(size==n){memcpy(dst,src,n);return 1;}
    if(size>=2&&src[0]==0xc2)return decode_words(src,size,dst,n);
    if(size<4||src[0]!=0xe5||src[1]>1)return 0;

    return src[1]?decode_records(src,size,dst,n,1):decode_records(src,size,dst,n,0);
}
