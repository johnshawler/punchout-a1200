#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "fceumm/src/drivers/libretro/libretro-common/include/libretro.h"
static unsigned fmt=RETRO_PIXEL_FORMAT_RGB565; static const void*vid; static unsigned vw,vh; static size_t vp;
static int frame; static unsigned *script; static int shotevery; static unsigned padmask; static long audio_total;
static bool env(unsigned c,void*d){
 switch(c){case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: fmt=*(unsigned*)d;return true;
 case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**)d=".";return true;
 case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: return false;
 default:return false;}}
static void vr(const void*d,unsigned w,unsigned h,size_t p){if(d){vid=d;vw=w;vh=h;vp=p;}}
static void ar(int16_t l,int16_t r){audio_total++;}
static size_t arb(const int16_t*d,size_t n){audio_total+=n;return n;}
static void ip(void){}
static int16_t is(unsigned p,unsigned d,unsigned i,unsigned id){ if(p||d!=RETRO_DEVICE_JOYPAD)return 0; return (padmask>>id)&1;}
int main(int argc,char**argv){
 void*h=dlopen(argv[1],RTLD_NOW); if(!h){puts(dlerror());return 1;}
 #define F(n) void(*n)()=dlsym(h,#n)
 void(*set_env)(void*)=dlsym(h,"retro_set_environment");
 void(*set_vr)(void*)=dlsym(h,"retro_set_video_refresh");
 void(*set_ar)(void*)=dlsym(h,"retro_set_audio_sample");
 void(*set_arb)(void*)=dlsym(h,"retro_set_audio_sample_batch");
 void(*set_ip)(void*)=dlsym(h,"retro_set_input_poll");
 void(*set_is)(void*)=dlsym(h,"retro_set_input_state");
 void(*init)()=dlsym(h,"retro_init");
 bool(*load)(const struct retro_game_info*)=dlsym(h,"retro_load_game");
 void(*run)()=dlsym(h,"retro_run");
 void*(*getmem)(unsigned)=dlsym(h,"retro_get_memory_data");
 void(*getav)(struct retro_system_av_info*)=dlsym(h,"retro_get_system_av_info");
 set_env(env);set_vr(vr);set_ar(ar);set_arb(arb);set_ip(ip);set_is(is);init();
 FILE*f=fopen(argv[2],"rb");fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char*buf=malloc(n);fread(buf,1,n,f);
 struct retro_game_info gi={argv[2],buf,n,NULL};
 if(!load(&gi)){puts("load fail");return 1;}
 struct retro_system_av_info av;getav(&av);printf("fps %.4f %ux%u rate %.1f\n",av.timing.fps,av.geometry.base_width,av.geometry.base_height,av.timing.sample_rate);
 int N=atoi(argv[3]);script=calloc(N+1,4);shotevery=atoi(argv[5]);{FILE*sf=fopen(argv[6],"r");int a,b,m;while(sf&&fscanf(sf,"%d %d %x",&a,&b,&m)==3)for(int i=a;i<b&&i<N;i++)script[i]|=m;}FILE*ram=fopen(argv[4],"wb");
 unsigned char*r=getmem(RETRO_MEMORY_SYSTEM_RAM); uint16_t*(*tp)(void)=dlsym(h,"retro_trace_ptr");void(*tc)(void)=dlsym(h,"retro_trace_clear");FILE*tf=fopen(argv[7],"wb");uint16_t*T=tp();
 for(frame=0;frame<N;frame++){
  padmask=script[frame];
  tc();run();fwrite(r,1,2048,ram);{uint32_t n=0;for(int b=0;b<16;b++)for(int a=0;a<65536;a++)if(T[b*65536+a])n++;fwrite(&n,4,1,tf);for(int b=0;b<16;b++)for(int a=0;a<65536;a++)if(T[b*65536+a]){uint8_t bb=b;uint16_t aa=a,cc=T[b*65536+a];fwrite(&bb,1,1,tf);fwrite(&aa,2,1,tf);fwrite(&cc,2,1,tf);}}
  if(vid&&(frame%shotevery==0)){char nm[96];snprintf(nm,96,"%s.%05d.ppm",argv[4],frame);FILE*p=fopen(nm,"wb");fprintf(p,"P6 %u %u 255\n",vw,vh);
   for(unsigned y=0;y<vh;y++)for(unsigned x=0;x<vw;x++){unsigned c;if(fmt==RETRO_PIXEL_FORMAT_RGB565){c=((uint16_t*)((char*)vid+y*vp))[x];fputc((c>>11&31)*255/31,p);fputc((c>>5&63)*255/63,p);fputc((c&31)*255/31,p);}else{c=((uint32_t*)((char*)vid+y*vp))[x];fputc(c>>16&255,p);fputc(c>>8&255,p);fputc(c&255,p);}}fclose(p);}
 }
 printf("frames %d audio samples %ld\n",N,audio_total);return 0;}
