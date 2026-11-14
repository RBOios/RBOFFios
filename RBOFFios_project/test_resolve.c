#define _XOPEN_SOURCE 700
#define _GNU_SOURCE
/* test_resolve.c - the cached resolve_path must return exactly what the original (scan) version returns.
 *   make test_resolve && ./test_resolve /path/to/game/folder                                                       */
#include "hle.c"
#include <time.h>
#include <ftw.h>
uint8_t *gmem; 
static char **list; static int nl, cap;
static int walk(const char *fpath,const struct stat *sb,int type,struct FTW *ftw){ (void)sb;(void)ftw; if(type==FTW_F){ if(nl==cap){ cap=cap?cap*2:1024; list=realloc(list,cap*sizeof *list); } list[nl++]=strdup(fpath); } return 0; }
static uint64_t rs=88172645463325252ull; static uint32_t rnd(void){ rs^=rs<<13; rs^=rs>>7; rs^=rs<<17; return (uint32_t)(rs>>11); }
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e3+t.tv_nsec/1e6; }
int main(int argc,char **argv){
    if(argc<2) return 1;
    g_root=argv[1];
    nftw(argv[1],walk,16,FTW_PHYS);
    size_t rl=strlen(argv[1]); long bad=0,total=0,miss=0;
    char (*paths)[1024]=malloc(sizeof(char[1024])*4000); int np=0;
    for(int k=0;k<4000&&nl;k++){
        const char *rel=list[rnd()%nl]+rl+1; char g[1024]; size_t n=0;
        int style=rnd()%4; if(style==1){ g[n++]='.'; g[n++]='\\'; } else if(style==2){ g[n++]='.'; g[n++]='\\'; g[n++]='.'; g[n++]='\\'; } else if(style==3){ g[n++]='C'; g[n++]=':'; g[n++]='\\'; }
        for(const char *p=rel;*p&&n<1000;p++){ char c=*p=='/'?'\\':*p; if(rnd()%3==0){ if(c>='a'&&c<='z') c-=32; else if(c>='A'&&c<='Z') c+=32; } g[n++]=c; }
        if(rnd()%12==0){ g[n++]='x'; }                       /* a missing file */
        g[n]=0; snprintf(paths[np++],1024,"%s",g);
    }
    for(int pass=0;pass<2;pass++) for(int i=0;i<np;i++){
        char a[1200],b[1200]; int ra=resolve_path(paths[i],a,sizeof a), rb=resolve_path_slow(paths[i],b,sizeof b);
        total++; if(!ra) miss++; if(ra!=rb||strcmp(a,b)){ bad++; if(bad<6) printf("MISMATCH %s -> [%d] %s  vs [%d] %s\n",paths[i],ra,a,rb,b); }
    }
    printf("%ld lookups over %d files (%ld missing paths), %ld mismatches -> %s\n",total,nl,miss,bad,bad?"FAILED":"IDENTICAL");
    /* timing */
    char o[1200]; double t0=now(); for(int r=0;r<3;r++) for(int i=0;i<np;i++) resolve_path_slow(paths[i],o,sizeof o); double t1=now();
    for(int r=0;r<3;r++) for(int i=0;i<np;i++) resolve_path(paths[i],o,sizeof o); double t2=now();
    printf("per lookup: scan %.1f us, cached %.1f us\n",(t1-t0)*1000/(3.0*np),(t2-t1)*1000/(3.0*np));
    return bad?1:0;
}
