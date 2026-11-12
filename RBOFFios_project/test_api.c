/* Host-side check of the embedding API: run the game, count frames, pull audio, stop. */
#include "rbo_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static volatile int frames=0, fin=0; static int fw=0,fh=0; static uint32_t center=0, first_center=0;
static void on_present(void *c,const uint32_t *px,int w,int h){
    (void)c; frames++; fw=w; fh=h; center=px[(h/2)*w+w/2];
    if(frames==1) first_center=center;
    if(frames==1200){ FILE *f=fopen("/tmp/api_frame.ppm","wb"); fprintf(f,"P6\n%d %d\n255\n",w,h); for(int i=0;i<w*h;i++){ fputc((px[i]>>16)&255,f); fputc((px[i]>>8)&255,f); fputc(px[i]&255,f);} fclose(f); }
}
static void on_finished(void *c,int code){ (void)c; fin=1; printf("finished code=%d\n",code); }
int main(int argc,char **argv){
    const char *root=argc>1?argv[1]:"/home/claude/game";
    RboCallbacks cb={0,on_present,on_finished};
    if(rbo_start(root,"RBO.exe",&cb)){ puts("start failed"); return 1; }
    long audio_frames=0; int16_t buf[1024*2]; int nonzero=0;
    for(int i=0;i<2000 && frames<1300;i++){ usleep(20000); int g=rbo_audio_pull(buf,1024); audio_frames+=g; for(int k=0;k<g*2;k++) if(buf[k]) nonzero++; }
    rbo_set_key(0x1E,1); usleep(100000); rbo_set_key(0x1E,0);
    rbo_stop(); for(int i=0;i<100&&!fin;i++) usleep(50000);
    printf("presented=%d size=%dx%d first_center=%08x center=%08x audio_frames=%ld nonzero=%d finished=%d\n",frames,fw,fh,first_center,center,audio_frames,nonzero,fin);
    return 0;
}
