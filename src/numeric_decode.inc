/* C2 numeric decoder. The tables map little-endian packed fields into
 * NEON lanes; TBL indices outside the 16-byte input vector produce zero.
 * A packed group of eight values occupies exactly `bits` bytes. */
static unsigned minimum(unsigned a,unsigned b) {return a<b?a:b;}
static unsigned packed_bytes(unsigned count,unsigned bits) {return (count*bits+7)/8;}

#define FIELD_INDEX32(B,J) ((B)*(J)/8),((B)*(J)/8+1),((B)*(J)/8+2),255
#define FIELD_ROW32(B) {FIELD_INDEX32(B,0),FIELD_INDEX32(B,1),FIELD_INDEX32(B,2),FIELD_INDEX32(B,3),FIELD_INDEX32(B,4),FIELD_INDEX32(B,5),FIELD_INDEX32(B,6),FIELD_INDEX32(B,7)}
#define FIELD_INDEX16(B,J) ((B)*(J)/8),((B)*(J)/8+1)
#define FIELD_ROW16(B) {FIELD_INDEX16(B,0),FIELD_INDEX16(B,1),FIELD_INDEX16(B,2),FIELD_INDEX16(B,3),FIELD_INDEX16(B,4),FIELD_INDEX16(B,5),FIELD_INDEX16(B,6),FIELD_INDEX16(B,7)}
#define FIELD_SHIFTS(B) {0,-(((B)*1)%8),-(((B)*2)%8),-(((B)*3)%8),-(((B)*4)%8),-(((B)*5)%8),-(((B)*6)%8),-(((B)*7)%8)}
#define ALL_WIDTHS(M) M(0),M(1),M(2),M(3),M(4),M(5),M(6),M(7),M(8),M(9),M(10),M(11),M(12),M(13),M(14),M(15),M(16)
static const U8 arm64_index32[17][32] __attribute__((aligned(16)))={ALL_WIDTHS(FIELD_ROW32)};
static const U8 arm64_index16[17][16] __attribute__((aligned(16)))={ALL_WIDTHS(FIELD_ROW16)};
static const int32_t arm64_shift32[17][8] __attribute__((aligned(16)))={ALL_WIDTHS(FIELD_SHIFTS)};
static const int16_t arm64_shift16[17][8] __attribute__((aligned(16)))={ALL_WIDTHS(FIELD_SHIFTS)};
#undef ALL_WIDTHS
#undef FIELD_SHIFTS
#undef FIELD_ROW16
#undef FIELD_INDEX16
#undef FIELD_ROW32
#undef FIELD_INDEX32

static inline uint16x8_t arm64_unpack8(uint8x16_t input,unsigned bits) {
    if(bits<=8) {
        uint16x8_t v=vreinterpretq_u16_u8(vqtbl1q_u8(input,vld1q_u8(arm64_index16[bits])));
        return vandq_u16(vshlq_u16(v,vld1q_s16(arm64_shift16[bits])),vdupq_n_u16((uint16_t)((1u<<bits)-1)));
    }
    uint32x4_t lo=vreinterpretq_u32_u8(vqtbl1q_u8(input,vld1q_u8(arm64_index32[bits])));
    uint32x4_t hi=vreinterpretq_u32_u8(vqtbl1q_u8(input,vld1q_u8(arm64_index32[bits]+16)));
    lo=vshlq_u32(lo,vld1q_s32(arm64_shift32[bits]));
    hi=vshlq_u32(hi,vld1q_s32(arm64_shift32[bits]+4));
    return vandq_u16(vcombine_u16(vmovn_u32(lo),vmovn_u32(hi)),vdupq_n_u16((uint16_t)((1u<<bits)-1)));
}
static inline uint16x8_t arm64_delta_scan8(uint16x8_t v) {
    const uint16x8_t zero=vdupq_n_u16(0);
    v=veorq_u16(vshrq_n_u16(v,1),vsubq_u16(zero,vandq_u16(v,vdupq_n_u16(1))));
    /* Inclusive scan in 16-bit arithmetic, as required by C2 wraparound. */
    v=vaddq_u16(v,vextq_u16(zero,v,7));
    v=vaddq_u16(v,vextq_u16(zero,v,6));
    v=vaddq_u16(v,vextq_u16(zero,v,4));
    return v;
}
static inline uint16x8_t arm64_delta8(uint16x8_t v,unsigned prev) {
    return vaddq_u16(arm64_delta_scan8(v),vdupq_n_u16((uint16_t)prev));
}

/* The caller validates count packed bytes and 2*count output bytes. Every
 * load below is exact (16, 8, or 1 byte); no adjacent block or padding is
 * read. Two widened groups share a single 16-byte load. */
static inline void arm64_decode_byte_fields(const U8 *src,U8 *dst,
                                            unsigned count,unsigned base,
                                            unsigned is_delta) {
    unsigned j=0;
    if(is_delta) {
        uint16x8_t carry=vdupq_n_u16((uint16_t)base);

        for(;j+32<=count;j+=32) {
            uint8x16_t p0=vld1q_u8(src+j),p1=vld1q_u8(src+j+16);
            uint16x8_t a=arm64_delta_scan8(vmovl_u8(vget_low_u8(p0)));
            uint16x8_t b=arm64_delta_scan8(vmovl_u8(vget_high_u8(p0)));
            uint16x8_t c=arm64_delta_scan8(vmovl_u8(vget_low_u8(p1)));
            uint16x8_t d=arm64_delta_scan8(vmovl_u8(vget_high_u8(p1)));
            a=vaddq_u16(a,carry);b=vaddq_u16(b,vdupq_laneq_u16(a,7));
            c=vaddq_u16(c,vdupq_laneq_u16(b,7));d=vaddq_u16(d,vdupq_laneq_u16(c,7));
            carry=vdupq_laneq_u16(d,7);
            vst1q_u8(dst+2*j,vreinterpretq_u8_u16(a));
            vst1q_u8(dst+2*j+16,vreinterpretq_u8_u16(b));
            vst1q_u8(dst+2*j+32,vreinterpretq_u8_u16(c));
            vst1q_u8(dst+2*j+48,vreinterpretq_u8_u16(d));
        }

        for(;j+16<=count;j+=16) {
            uint8x16_t packed=vld1q_u8(src+j);
            uint16x8_t a=arm64_delta_scan8(vmovl_u8(vget_low_u8(packed)));
            uint16x8_t b=arm64_delta_scan8(vmovl_u8(vget_high_u8(packed)));
            a=vaddq_u16(a,carry);
            b=vaddq_u16(b,vdupq_laneq_u16(a,7));
            carry=vdupq_laneq_u16(b,7);
            vst1q_u8(dst+2*j,vreinterpretq_u8_u16(a));
            vst1q_u8(dst+2*j+16,vreinterpretq_u8_u16(b));
        }
        if(j+8<=count) {
            uint16x8_t a=arm64_delta_scan8(vmovl_u8(vld1_u8(src+j)));
            a=vaddq_u16(a,carry);
            carry=vdupq_laneq_u16(a,7);
            vst1q_u8(dst+2*j,vreinterpretq_u8_u16(a));j+=8;
        }
        unsigned prev=vgetq_lane_u16(carry,0);
        for(;j<count;j++) {
            unsigned v=src[j];
            prev=(prev+((v>>1)^(0u-(v&1))))&65535u;
            put16(dst+2*j,prev);
        }
    } else {
        uint16x8_t basev=vdupq_n_u16((uint16_t)base);

        for(;j+32<=count;j+=32) {
            uint8x16_t p0=vld1q_u8(src+j),p1=vld1q_u8(src+j+16);
            vst1q_u8(dst+2*j,vreinterpretq_u8_u16(vaddq_u16(vmovl_u8(vget_low_u8(p0)),basev)));
            vst1q_u8(dst+2*j+16,vreinterpretq_u8_u16(vaddq_u16(vmovl_u8(vget_high_u8(p0)),basev)));
            vst1q_u8(dst+2*j+32,vreinterpretq_u8_u16(vaddq_u16(vmovl_u8(vget_low_u8(p1)),basev)));
            vst1q_u8(dst+2*j+48,vreinterpretq_u8_u16(vaddq_u16(vmovl_u8(vget_high_u8(p1)),basev)));
        }

        for(;j+16<=count;j+=16) {
            uint8x16_t packed=vld1q_u8(src+j);
            uint16x8_t a=vaddq_u16(vmovl_u8(vget_low_u8(packed)),basev);
            uint16x8_t b=vaddq_u16(vmovl_u8(vget_high_u8(packed)),basev);
            vst1q_u8(dst+2*j,vreinterpretq_u8_u16(a));
            vst1q_u8(dst+2*j+16,vreinterpretq_u8_u16(b));
        }
        if(j+8<=count) {
            uint16x8_t a=vaddq_u16(vmovl_u8(vld1_u8(src+j)),basev);
            vst1q_u8(dst+2*j,vreinterpretq_u8_u16(a));j+=8;
        }
        for(;j<count;j++)put16(dst+2*j,base+src[j]);
    }
}

/* Isolate numeric tables, local tail storage and their register pressure from
 * the much more frequent E5/RAW dispatcher. The wire format is unchanged. */

__attribute__((noinline))

static int decode_words(const U8 *src,unsigned size,U8 *dst,unsigned n) {
    if(size<2||size>=n||src[0]!=0xc2)return 0;
    unsigned nw=n/2,pos=2,tail=n%2,limit=size-tail;
    if(src[1]==128) {
        if(size!=4+tail)return 0;
        unsigned value=get16(src+2),i=0;

        uint16x8_t v=vdupq_n_u16((uint16_t)value);
        for(;i+8<=nw;i+=8)vst1q_u8(dst+2*i,vreinterpretq_u8_u16(v));

        for(;i<nw;i++)put16(dst+2*i,value);
        if(tail)dst[n-1]=src[4];
        return 1;
    }
    if(src[1]!=0)return 0;
    for(unsigned i=0;i<nw;i+=64) {
        if(pos>=limit)return 0;
        unsigned count=minimum(64,nw-i),tag=src[pos++];
        if(tag==128) {
            if(2*count>limit-pos)return 0;
            memcpy(dst+2*i,src+pos,2*count);pos+=2*count;continue;
        }
        unsigned bits=tag&31,is_delta=tag&32;
        if(tag&192||bits>16||limit-pos<2)return 0;
        unsigned base=get16(src+pos);pos+=2;
        unsigned bytes=packed_bytes(count,bits),j=0,used=0,prev=base;
        if(bytes>limit-pos)return 0;

        if(bits==8) {
            arm64_decode_byte_fields(src+pos,dst+2*i,count,base,is_delta);
            pos+=bytes;continue;
        }

        /* Validated packed data occupies at most 128 bytes. A local tail
         * copy provides the sixteen readable bytes needed by every vector
         * load, without imposing external input-padding requirements.
         * Most blocks read directly from the input; only a page-end block
         * whose last vector could cross size uses this small scratch area. */
        U8 padded[144];
        const U8 *packed=src+pos;
        if(bits&&count>=8&&size-pos<bytes+16) {
            memcpy(padded,packed,bytes);
            memset(padded+bytes,0,16);
            packed=padded;
        }
#define LOAD_PACKED(USED) vld1q_u8(packed+(USED))

#define UNPACK_GROUP(INPUT) arm64_unpack8((INPUT),bits)

        if(!bits) {
            uint16x8_t v=vdupq_n_u16((uint16_t)base);
            for(;j+8<=count;j+=8)vst1q_u8(dst+2*(i+j),vreinterpretq_u8_u16(v));
        } else if(!is_delta) {
            const uint16x8_t basev=vdupq_n_u16((uint16_t)base);

            for(;j+32<=count;j+=32,used+=4*bits) {
                uint16x8_t a=UNPACK_GROUP(LOAD_PACKED(used));
                uint16x8_t b=UNPACK_GROUP(LOAD_PACKED(used+bits));
                uint16x8_t c=UNPACK_GROUP(LOAD_PACKED(used+2*bits));
                uint16x8_t d=UNPACK_GROUP(LOAD_PACKED(used+3*bits));
                vst1q_u8(dst+2*(i+j),vreinterpretq_u8_u16(vaddq_u16(a,basev)));
                vst1q_u8(dst+2*(i+j+8),vreinterpretq_u8_u16(vaddq_u16(b,basev)));
                vst1q_u8(dst+2*(i+j+16),vreinterpretq_u8_u16(vaddq_u16(c,basev)));
                vst1q_u8(dst+2*(i+j+24),vreinterpretq_u8_u16(vaddq_u16(d,basev)));
            }

            for(;j+16<=count;j+=16,used+=2*bits) {
                uint16x8_t a=UNPACK_GROUP(LOAD_PACKED(used));
                uint16x8_t b=UNPACK_GROUP(LOAD_PACKED(used+bits));
                vst1q_u8(dst+2*(i+j),vreinterpretq_u8_u16(vaddq_u16(a,basev)));
                vst1q_u8(dst+2*(i+j+8),vreinterpretq_u8_u16(vaddq_u16(b,basev)));
            }

            for(;j+8<=count;j+=8,used+=bits) {
                uint16x8_t v=UNPACK_GROUP(LOAD_PACKED(used));
                vst1q_u8(dst+2*(i+j),vreinterpretq_u8_u16(vaddq_u16(v,basev)));
            }
        } else {
            for(;j+32<=count;j+=32,used+=4*bits) {
                uint16x8_t a=UNPACK_GROUP(LOAD_PACKED(used));
                uint16x8_t b=UNPACK_GROUP(LOAD_PACKED(used+bits));
                uint16x8_t c=UNPACK_GROUP(LOAD_PACKED(used+2*bits));
                uint16x8_t d=UNPACK_GROUP(LOAD_PACKED(used+3*bits));
                a=arm64_delta_scan8(a);b=arm64_delta_scan8(b);
                c=arm64_delta_scan8(c);d=arm64_delta_scan8(d);

                a=vaddq_u16(a,vdupq_n_u16((uint16_t)prev));

                b=vaddq_u16(b,vdupq_laneq_u16(a,7));
                c=vaddq_u16(c,vdupq_laneq_u16(b,7));
                d=vaddq_u16(d,vdupq_laneq_u16(c,7));

                prev=vgetq_lane_u16(d,7);

                vst1q_u8(dst+2*(i+j),vreinterpretq_u8_u16(a));
                vst1q_u8(dst+2*(i+j+8),vreinterpretq_u8_u16(b));
                vst1q_u8(dst+2*(i+j+16),vreinterpretq_u8_u16(c));
                vst1q_u8(dst+2*(i+j+24),vreinterpretq_u8_u16(d));
            }

            /* Decode both groups before introducing their carry dependence.
             * Each scan is independent; all arithmetic wraps in 16 bits. */
            for(;j+16<=count;j+=16,used+=2*bits) {
                uint16x8_t a=UNPACK_GROUP(LOAD_PACKED(used));
                uint16x8_t b=UNPACK_GROUP(LOAD_PACKED(used+bits));
                a=arm64_delta_scan8(a);
                b=arm64_delta_scan8(b);

                a=vaddq_u16(a,vdupq_n_u16((uint16_t)prev));

                b=vaddq_u16(b,vdupq_laneq_u16(a,7));

                prev=vgetq_lane_u16(b,7);

                vst1q_u8(dst+2*(i+j),vreinterpretq_u8_u16(a));
                vst1q_u8(dst+2*(i+j+8),vreinterpretq_u8_u16(b));
            }

            for(;j+8<=count;j+=8,used+=bits) {
                uint16x8_t v=UNPACK_GROUP(LOAD_PACKED(used));

                v=arm64_delta8(v,prev);prev=vgetq_lane_u16(v,7);

                vst1q_u8(dst+2*(i+j),vreinterpretq_u8_u16(v));
            }
        }
#undef UNPACK_GROUP
#undef LOAD_PACKED

        uint32_t acc=0;unsigned filled=0;
        for(;j<count;j++) {
            while(filled<bits) {acc|=(uint32_t)src[pos+used++]<<filled;filled+=8;}
            unsigned value=acc&((1u<<bits)-1);acc>>=bits;filled-=bits;
            if(is_delta) {unsigned d=(value>>1)^(0u-(value&1));prev=(prev+d)&65535;value=prev;}
            else value=(value+base)&65535;
            put16(dst+2*(i+j),value);
        }
        if(used!=bytes)return 0;
        pos+=bytes;
    }
    if(pos+tail!=size)return 0;
    if(tail)dst[n-1]=src[pos];
    return 1;
}
