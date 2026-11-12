#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t pe_load(const char *path){
    FILE *f=fopen(path,"rb"); if(!f){perror(path);exit(1);}
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *b=malloc(n); fread(b,1,n,f); fclose(f);
    uint32_t pe=*(uint32_t*)(b+0x3C);
    if(memcmp(b+pe,"PE\0\0",4)){fprintf(stderr,"not PE\n");exit(1);}
    uint16_t nsec=*(uint16_t*)(b+pe+6);
    uint16_t optsz=*(uint16_t*)(b+pe+20);
    uint8_t *opt=b+pe+24;
    uint32_t entry=*(uint32_t*)(opt+16), base=*(uint32_t*)(opt+28), size=*(uint32_t*)(opt+56), hsz=*(uint32_t*)(opt+60);
    if(base+size>=MEM_SIZE/4){fprintf(stderr,"image too big\n");exit(1);}
    memcpy(gmem+base,b,hsz<n?hsz:n);
    uint8_t *sec=opt+optsz; g_code_lo=0xFFFFFFFFu; g_code_hi=0;
    for(int i=0;i<nsec;i++){
        uint8_t *s=sec+i*40;
        uint32_t vsz=*(uint32_t*)(s+8), va=*(uint32_t*)(s+12), rsz=*(uint32_t*)(s+16), roff=*(uint32_t*)(s+20);
        { uint32_t ch=*(uint32_t*)(s+36); if(ch&0x20000000u){ if(base+va<g_code_lo) g_code_lo=base+va; uint32_t e=base+va+(vsz>*(uint32_t*)(s+16)?vsz:*(uint32_t*)(s+16)); if(e>g_code_hi) g_code_hi=e; } }
        if(rsz>vsz && vsz) rsz=vsz;
        if(roff+rsz>n) rsz=n-roff;
        memcpy(gmem+base+va,b+roff,rsz);
        fprintf(stderr,"  section %.8s va=%08x vsz=%08x raw=%08x\n",s,base+va,vsz,rsz);
    }
    fprintf(stderr,"  code range %08x-%08x entry %08x\n",g_code_lo,g_code_hi,base+entry);
    /* imports */
    uint32_t impRva=*(uint32_t*)(opt+96+8), impSz=*(uint32_t*)(opt+96+12);
    (void)impSz;
    int total=0, missing=0;
    for(uint32_t d=base+impRva; rd32(d+12); d+=20){
        const char *dll=(const char*)(gmem+base+rd32(d+12));
        uint32_t oft=rd32(d), ft=rd32(d+16);
        uint32_t lookup = oft? base+oft : base+ft;
        for(int k=0;;k++){
            uint32_t ent=rd32(lookup+4*k); if(!ent) break;
            char name[96];
            if(ent&0x80000000u) snprintf(name,sizeof name,"Ordinal%u",ent&0xFFFF);
            else snprintf(name,sizeof name,"%s",(const char*)(gmem+base+ent+2));
            int idx=hle_lookup(dll,name);
            if(idx<0){ idx=hle_register(dll,name,-1,NULL); missing++; if(g_trace)fprintf(stderr,"  [unimplemented import] %s!%s\n",dll,name); }
            wr32(base+ft+4*k, hle_thunk(idx));
            total++;
        }
    }
    fprintf(stderr,"  imports resolved: %d (unimplemented: %d)\n",total,missing);
    free(b);
    return base+entry;
}
