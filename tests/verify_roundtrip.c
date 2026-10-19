/* Full-length synthetic cases, damaged-input safety, optional Silesia corpus. */
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec.h"
static const char *names[]={"dickens","mozilla","mr","nci","ooffice","osdb","reymont","samba","sao","webster","xml","x-ray"};
static const uint64_t sizes[]={10192446,51220480,9970564,33553445,6152192,10085684,6627202,21606400,7251944,41458703,5345280,8474240};
typedef struct {uint64_t bytes,stored,pages,raw,modes[2],matches,word_blocks,delta_blocks,widths[17];} Stats;
static uint32_t seed=123456789;
static uint32_t rnd(void) {uint32_t x=seed;x^=x<<13;x^=x>>17;x^=x<<5;return seed=x;}
static void fail(const char *s) {fprintf(stderr,"FAIL: %s\n",s);exit(1);}
static void *alloc(size_t n) {void *p=malloc(n?n:1);if(!p)fail("allocation");return p;}

static void inspect(const unsigned char *src,unsigned n,unsigned size,Stats *s) {
    s->bytes+=n;s->stored+=size;s->pages++;
    if(size==n) {s->raw++;return;}
    if(src[0]==0xe5) {
        if(size<4||src[1]!=0)fail("invalid encoder E5 header");
        s->modes[0]++;s->matches+=src[2]|((unsigned)src[3]<<8);return;
    }
    if(src[0]!=0xc2)fail("unexpected format");
    s->modes[1]++;
    if(src[1]==128)return;
    unsigned nw=n/2,p=2;
    for(unsigned i=0;i<nw;i+=64) {
        unsigned count=nw-i<64?nw-i:64,tag=src[p++];s->word_blocks++;
        if(tag==128) {p+=2*count;continue;}
        unsigned bits=tag&31;s->widths[bits]++;s->delta_blocks+=(tag&32)!=0;
        p+=2+(count*bits+7)/8;
    }
    if(p+n%2!=size)fail("word format accounting");
}

static void roundtrip(const unsigned char *input,unsigned n,Stats *stats,int mutate) {
    unsigned char *source=alloc(n),*encoded=alloc(4352),*out=alloc(n);
    memcpy(source,input,n);
    unsigned unused,mode,size=codec_encode(source,n,encoded,&unused,&mode);
    if(!size||size>n)fail("no-expansion invariant");
    unsigned char *exact=alloc(size);memcpy(exact,encoded,size);
    if(!codec_decode(exact,size,out,n)||memcmp(source,out,n))fail("roundtrip");
    if(stats)inspect(exact,n,size,stats);
    if(mutate&&size<n) {
        for(unsigned j=0;j<4;j++) {
            unsigned truncated=j==0?0:j==1?size-1:rnd()%size;
            unsigned char *part=alloc(truncated);memcpy(part,exact,truncated);
            (void)codec_decode(part,truncated,out,n);free(part);
        }
        for(unsigned j=0;j<4;j++) {
            unsigned at=rnd()%size;exact[at]^=(unsigned char)(1+(rnd()%255));
            (void)codec_decode(exact,size,out,n);
        }
    }
    free(exact);free(out);free(encoded);free(source);
}

static void print_stats(const char *name,const Stats *s) {
    printf("{\"file\":\"%s\",\"input_bytes\":%"PRIu64",\"stored_bytes\":%"PRIu64","
           "\"pages\":%"PRIu64",\"raw_pages\":%"PRIu64",\"ratio\":%.9f,"
           "\"e5_pages\":%"PRIu64",\"c2_pages\":%"PRIu64",\"matches\":%"PRIu64","
           "\"word_blocks\":%"PRIu64",\"delta_blocks\":%"PRIu64",\"roundtrip\":\"passed\"}\n",
           name,s->bytes,s->stored,s->pages,s->raw,(double)s->bytes/s->stored,
           s->modes[0],s->modes[1],s->matches,s->word_blocks,s->delta_blocks);
    fflush(stdout);
}

int main(int argc,char **argv) {
    if(argc>2)fail("usage: verify_roundtrip [CORPUS]");
    if(!codec_init())fail("init");
    unsigned char input[4096];unsigned cases=0;
    for(unsigned kind=0;kind<13;kind++)for(unsigned n=1;n<=4096;n++) {
        unsigned word=1024;
        for(unsigned i=0;i<n;i++) {
            unsigned r=rnd();
            input[i]=(unsigned char)(kind==0?0:kind==1?255:kind==2?i%7:kind==3?r:
                      kind==4?r%16:kind==5?(r%17?32+r%31:r):kind==6?i%256:
                      kind==7?(i%16==0?r:i%16):(r%16?0:r));
            if(kind>=9) {
                if(i%2==0)word=kind==9?(3*(i/2))&65535:kind==10?(word+(r%17)-8)&4095:kind==11?(i%4?8191:0):(i%128==0?4095:1024+(r%7));
                input[i]=(unsigned char)(word>>(8*(i%2)));
            }
        }
        roundtrip(input,n,NULL,n%127==0);cases++;
    }
    fprintf(stderr,"%u synthetic round trips passed (every length 1..4096).\n",cases);
    /* Arbitrary and header-shaped damaged inputs, with exact-size allocations,
     * exercise bounded decoding. Valid mutations need not be rejected. */
    for(unsigned j=0;j<10000;j++) {
        unsigned n=1+rnd()%4096,size=rnd()%(n+1);
        unsigned char *src=alloc(size),*dst=alloc(n);
        for(unsigned i=0;i<size;i++)src[i]=(unsigned char)rnd();
        if(size>=2) {
            src[0]=(unsigned char)(j%4?0xe5:0xc2);
            src[1]=src[0]==0xc2?(j%8?0:128):(j%2);
            if(size>=4&&src[0]==0xe5) {
                unsigned records=rnd()%(n/4+1);
                src[2]=(unsigned char)records;src[3]=(unsigned char)(records>>8);
            }
        }
        (void)codec_decode(src,size,dst,n);free(src);free(dst);
    }
    fprintf(stderr,"10000 damaged/arbitrary decoder inputs completed.\n");
    if(argc==1) { puts("All synthetic and damaged-input checks passed."); return 0; }
    Stats total={0};
    for(unsigned f=0;f<12;f++) {
        char path[4096];snprintf(path,sizeof(path),"%s/%s",argv[1],names[f]);
        FILE *in=fopen(path,"rb");if(!in)fail(path);Stats s={0};size_t n;
        while((n=fread(input,1,sizeof(input),in)))roundtrip(input,(unsigned)n,&s,0);
        if(ferror(in))fail("corpus read");
        fclose(in);if(s.bytes!=sizes[f])fail("corpus size");print_stats(names[f],&s);
        total.bytes+=s.bytes;total.stored+=s.stored;total.pages+=s.pages;total.raw+=s.raw;
        for(unsigned k=0;k<2;k++)total.modes[k]+=s.modes[k];
        total.matches+=s.matches;total.word_blocks+=s.word_blocks;total.delta_blocks+=s.delta_blocks;
        for(unsigned k=0;k<17;k++)total.widths[k]+=s.widths[k];
    }
    print_stats("TOTAL",&total);fprintf(stderr,"All checks passed.\n");return 0;
}
