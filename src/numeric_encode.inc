/* C2 numeric encoding: 64-word frames or ZigZag deltas, packed with NEON.
 * Wide stores stay within the documented 4352-byte encode capacity; the
 * returned length is exact and partial groups use bounded scalar stores. */
static unsigned minimum(unsigned a,unsigned b) {return a<b?a:b;}
static unsigned packed_bytes(unsigned count,unsigned bits) {return (count*bits+7)/8;}
/* Lossless 16-bit frame / delta mode selected from the page contents. */
static unsigned word_bits(unsigned x) {return x?32u-(unsigned)__builtin_clz(x):0;}
static unsigned pack_words(const uint16_t *v,unsigned count,unsigned bits,U8 *dst) {
    if(!bits)return 0;
    unsigned pos=0,i=0;

    if(bits==16){memcpy(dst,v,2*count);return 2*count;}

    /* Pairwise addition joins disjoint bit fields. Widen before each join:
     * 2 values fit in 32 bits, 4 in 64 bits for every bits in 1..15. */
    const int32x4_t shift32={0,(int32_t)bits,0,(int32_t)bits};
    const int64x2_t shift64={0,(int64_t)(2*bits)};

    for(;i+8<=count;i+=8,pos+=bits) {
        uint64_t a,b;

        uint16x8_t cur=vld1q_u16(v+i);
        uint32x4_t lo=vshlq_u32(vmovl_u16(vget_low_u16(cur)),shift32);
        uint32x4_t hi=vshlq_u32(vmovl_u16(vget_high_u16(cur)),shift32);
        uint32x4_t pairs=vpaddq_u32(lo,hi);
        uint64x2_t low=vshlq_u64(vmovl_u32(vget_low_u32(pairs)),shift64);
        uint64x2_t high=vshlq_u64(vmovl_u32(vget_high_u32(pairs)),shift64);
        uint64x2_t groups=vpaddq_u64(low,high);
        a=vgetq_lane_u64(groups,0);b=vgetq_lane_u64(groups,1);

        uint64_t packed_low=a|(b<<(4*bits));memcpy(dst+pos,&packed_low,8);
        if(bits>8){uint64_t high=b>>(64-4*bits);memcpy(dst+pos+8,&high,8);}
    }
    /* At most 7 bytes of overstore, within the encoder scratch contract. */

    uint64_t acc=0;unsigned filled=0;
    for(;i<count;i++) {
        acc|=(uint64_t)v[i]<<filled;filled+=bits;
        if(filled>=32){uint32_t w=(uint32_t)acc;memcpy(dst+pos,&w,4);pos+=4;acc>>=32;filled-=32;}
    }
    unsigned tail=(filled+7)/8;memcpy(dst+pos,&acc,tail);return pos+tail;
}

static unsigned route_pair_delta(uint32_t pair) {
    return ((pair>>16)-(pair&65535)+(1u<<(10-1)))&65535;
}

static int route_small_or_delta(const U8 *src,unsigned n) {
    if(n<64)return 0;
    unsigned small_mask=0,delta_mask=0;
    /* Reuse each bounded pair load for both predicates. Failures accumulate:
     * once each predicate has failed somewhere, neither can recover later.
     * The last small-value sample is the LAST word of its pair. */

    if(n==4096) {
#define SAMPLE_AT(j) do { uint32_t pair=read32(src+2*((j)==15?2046:(j)*2047/15)); small_mask|=(j)==15?pair>>24:(pair>>8)&255; delta_mask|=route_pair_delta(pair); } while(0)

#define SAMPLE_GROUP(a,b,c,d) do { SAMPLE_AT(a);SAMPLE_AT(b);SAMPLE_AT(c);SAMPLE_AT(d); if(small_mask>=32&&delta_mask>=(1u<<10))return 0; } while(0)

        /* Reject immediately when both tests fail at the first sample;
         * otherwise finish samples 0..3, then process groups of four. */

        SAMPLE_AT(0);
        if(small_mask>=32&&delta_mask>=(1u<<10))return 0;
        SAMPLE_AT(1);SAMPLE_AT(2);SAMPLE_AT(3);
        if(small_mask>=32&&delta_mask>=(1u<<10))return 0;
        SAMPLE_GROUP(4,5,6,7);
        SAMPLE_GROUP(8,9,10,11);
        SAMPLE_GROUP(12,13,14,15);
#undef SAMPLE_GROUP
#undef SAMPLE_AT
        return 1;
    }

    unsigned nw=n/2;
    for(unsigned j=0;j<16;j+=4) {
        for(unsigned k=0;k<4;k++) {
            unsigned p=minimum((j+k)*(nw-1)/15,nw-2);
            uint32_t pair=read32(src+2*p);
            small_mask|=j+k==15?pair>>24:(pair>>8)&255;
            delta_mask|=route_pair_delta(pair);
        }
        if(small_mask>=32&&delta_mask>=(1u<<10))return 0;
    }
    return 1;
}

static int numeric_candidate(const U8 *src,unsigned n) {
    return route_small_or_delta(src,n);
}
__attribute__((noinline))

static unsigned encode_words(const U8 *src,unsigned n,U8 *dst) {
    if(n<16) {memcpy(dst,src,n);return n;}
    unsigned nw=n/2,pos=2,global_min=65535,global_max=0;

    /* A constant C2 page has a special four-byte representation. Reject
     * nonconstant input with the first 32-byte comparison, then scan with
     * independent 64-bit words. Read only complete 16-bit input values. */
    unsigned constant=get16(src),end=n&~1u,j=0;
    uint64_t repeated=(uint64_t)constant*UINT64_C(0x0001000100010001);
    for(;j+32<=end;j+=32) {
        uint64_t different=(read64(src+j)^repeated)|(read64(src+j+8)^repeated)|
                           (read64(src+j+16)^repeated)|(read64(src+j+24)^repeated);
        if(different)goto nonconstant;
    }
    for(;j+8<=end;j+=8)if(read64(src+j)!=repeated)goto nonconstant;
    for(;j<end;j+=2)if(get16(src+j)!=constant)goto nonconstant;
    dst[0]=0xc2;dst[1]=128;put16(dst+2,constant);
    if(n&1u)dst[4]=src[n-1];
    return 4+(n&1u);
nonconstant:;

    dst[0]=0xc2;dst[1]=0;
    for(unsigned i=0;i<nw;i+=64) {
        unsigned count=minimum(64,nw-i),lo=65535,hi=0,all_delta=0,prev=get16(src+2*i);
        uint16_t values[64],delta[64];

        if(count==64) {
            uint16x8_t vmin=vdupq_n_u16(65535),vmax=vdupq_n_u16(0),dor=vdupq_n_u16(0);
            uint16x8_t last=vdupq_n_u16((uint16_t)prev);
            for(unsigned j=0;j<64;j+=8) {
                uint16x8_t cur=vld1q_u16((const uint16_t *)(src+2*(i+j)));
                uint16x8_t pred=vextq_u16(last,cur,7);
                uint16x8_t d=vsubq_u16(cur,pred);
                d=veorq_u16(vshlq_n_u16(d,1),vreinterpretq_u16_s16(vshrq_n_s16(vreinterpretq_s16_u16(d),15)));

                vst1q_u16(values+j,cur);

                vst1q_u16(delta+j,d);
                vmin=vminq_u16(vmin,cur);vmax=vmaxq_u16(vmax,cur);dor=vorrq_u16(dor,d);
                last=cur;
            }
            lo=vminvq_u16(vmin);hi=vmaxvq_u16(vmax);
            dor=vorrq_u16(dor,vextq_u16(dor,dor,4));
            dor=vorrq_u16(dor,vextq_u16(dor,dor,2));
            dor=vorrq_u16(dor,vextq_u16(dor,dor,1));
            all_delta=vgetq_lane_u16(dor,0);
        } else

        for(unsigned j=0;j<count;j++) {
            unsigned value=get16(src+2*(i+j));

            values[j]=(uint16_t)value;

            if(value<lo)lo=value;
            if(value>hi)hi=value;
            unsigned d=(value-prev)&65535;
            delta[j]=((d<<1)^(0u-(d>>15)))&65535;all_delta|=delta[j];prev=value;
        }
        if(lo<global_min)global_min=lo;
        if(hi>global_max)global_max=hi;
        unsigned fb=word_bits(hi-lo),db=word_bits(all_delta),is_delta=db<fb,bits=is_delta?db:fb;
        unsigned packed=packed_bytes(count,bits);
        if(3+packed>=1+2*count) {
            dst[pos++]=128;memcpy(dst+pos,src+2*i,2*count);pos+=2*count;
        } else {
            dst[pos++]=(U8)(bits|(is_delta?32:0));
            put16(dst+pos,is_delta?values[0]:lo);pos+=2;
            if(!is_delta)for(unsigned j=0;j<count;j++)values[j]-=lo;

            pos+=pack_words(is_delta?delta:values,count,bits,dst+pos);
        }
        if(pos+n%2>=n) {memcpy(dst,src,n);return n;}
    }
    if(global_min==global_max) {dst[1]=128;put16(dst+2,global_min);pos=4;}
    if(n%2)dst[pos++]=src[n-1];
    if(pos>=n) {memcpy(dst,src,n);return n;}
    return pos;
}
