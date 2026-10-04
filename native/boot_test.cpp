#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/VideoOut.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <stdint.h>
#include <stddef.h>

static const int W=1920,H=1080;

static void fill(uint32_t*p,uint32_t c){for(int i=0;i<W*H;i++)p[i]=c;}
static void rect(uint32_t*p,int x,int y,int w,int h,uint32_t c){
 if(x<0){w+=x;x=0;} if(y<0){h+=y;y=0;} if(x+w>W)w=W-x;if(y+h>H)h=H-y;if(w<=0||h<=0)return;
 for(int yy=y;yy<y+h;yy++)for(int xx=x;xx<x+w;xx++)p[yy*W+xx]=c;
}
static void border(uint32_t*p,int x,int y,int w,int h,int t,uint32_t c){
 rect(p,x,y,w,t,c);rect(p,x,y+h-t,w,t,c);rect(p,x,y,t,h,c);rect(p,x+w-t,y,t,h,c);
}

static const char* glyph(char c){
 switch(c){
 case 'A':return "01110100011000111111100011000110001";
 case 'B':return "11110100011000111110100011000111110";
 case 'C':return "01111100001000010000100001000001111";
 case 'D':return "11110100011000110001100011000111110";
 case 'E':return "11111100001000011110100001000011111";
 case 'F':return "11111100001000011110100001000010000";
 case 'G':return "01111100001000010111100011000101111";
 case 'H':return "10001100011000111111100011000110001";
 case 'I':return "11111001000010000100001000010011111";
 case 'L':return "10000100001000010000100001000011111";
 case 'M':return "10001110111010110101100011000110001";
 case 'N':return "10001110011010110011100011000110001";
 case 'O':return "01110100011000110001100011000101110";
 case 'P':return "11110100011000111110100001000010000";
 case 'R':return "11110100011000111110101001001010001";
 case 'S':return "01111100001000001110000010000111110";
 case 'T':return "11111001000010000100001000010000100";
 case 'U':return "10001100011000110001100011000101110";
 case 'V':return "10001100011000110001100010101000100";
 case 'Y':return "10001100010101000100001000010000100";
 case '1':return "00100011000010000100001000010001110";
 case '2':return "01110100010000100010001000100011111";
 case '3':return "11110000010000101110000010000111110";
 case '4':return "00010001100101010010111110001000010";
 default:return 0;
 }
}
static void text(uint32_t*p,int x,int y,const char*s,int scale,uint32_t c){
 for(;*s;s++){
  if(*s==' '){x+=4*scale;continue;}
  const char*g=glyph(*s); if(!g){x+=6*scale;continue;}
  for(int yy=0;yy<7;yy++)for(int xx=0;xx<5;xx++)if(g[yy*5+xx]=='1')rect(p,x+xx*scale,y+yy*scale,scale,scale,c);
  x+=6*scale;
 }
}

static void drawStore(uint32_t*p,int selected){
 const uint32_t bg=0x80101420,top=0x80192334,panel=0x80212D42,card=0x802B3A54;
 const uint32_t accent=0x8000A8FF,white=0x80F4F7FF,muted=0x80788AA8;
 fill(p,bg); rect(p,0,0,W,120,top);rect(p,0,116,W,4,accent);
 rect(p,70,34,52,52,accent);rect(p,82,46,28,28,top);
 text(p,150,38,"ORBIS STORE",7,white); text(p,150,91,"HOME",2,muted);

 rect(p,1450,36,380,50,panel);border(p,1450,36,380,50,2,muted);text(p,1490,52,"SEARCH",3,muted);

 rect(p,55,170,330,830,panel);rect(p,55,190,8,86,accent);
 text(p,95,210,"HOME",5,white);text(p,95,330,"GAMES",4,muted);
 text(p,95,410,"APPS",4,muted);text(p,95,490,"INSTALL",4,muted);

 rect(p,430,170,1435,300,panel);border(p,430,170,1435,300,3,accent);
 text(p,485,215,"ORBIS STORE",7,white);
 text(p,485,285,"HOME FOR PS4 APPS",4,muted);
 rect(p,485,370,260,54,accent);text(p,525,383,"OPEN",4,white);

 const int y=525,cw=325,ch=385,gap=30,start=430;
 const char*names[4]={"GAME 1","APP 2","GAME 3","APP 4"};
 for(int i=0;i<4;i++){
  int x=start+i*(cw+gap);rect(p,x,y,cw,ch,card);
  rect(p,x,y,cw,230,(i==0)?0x804B65A0:(i==1)?0x80633E72:(i==2)?0x803A6B58:0x80715A35);
  text(p,x+25,y+260,names[i],4,white);text(p,x+25,y+305,"INSTALL",3,muted);
  rect(p,x+25,y+345,125,35,accent);text(p,x+39,y+353,"OPEN",3,white);
 }
 border(p,start+selected*(cw+gap)-5,y-5,cw+10,ch+10,5,accent);
 rect(p,430,950,1435,50,top);text(p,470,965,"SELECT",3,white);text(p,720,965,"BACK",3,muted);
}

int main(void){
 sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);
 int32_t video=sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN,ORBIS_VIDEO_OUT_BUS_MAIN,0,0);
 if(video<0)for(;;)sceKernelUsleep(1000000);

 const size_t one=(size_t)W*H*4,align=0x200000;
 const size_t sz=one*2,alloc=((sz+align-1)/align)*align;
 off_t off=0;int rc=sceKernelAllocateDirectMemory(0,sceKernelGetDirectMemorySize(),alloc,align,3,&off);
 if(rc<0)for(;;)sceKernelUsleep(1000000);
 void*mem=0;rc=sceKernelMapDirectMemory(&mem,alloc,0x33,0,off,align);
 if(rc<0||!mem)for(;;)sceKernelUsleep(1000000);

 uint32_t*fb[2]={(uint32_t*)mem,(uint32_t*)((char*)mem+one)};
 OrbisVideoOutBufferAttribute attr;sceVideoOutSetBufferAttribute(&attr,0x80000000,1,0,W,H,W);
 void*bufs[2]={fb[0],fb[1]};rc=sceVideoOutRegisterBuffers(video,0,bufs,2,&attr);
 if(rc<0)for(;;)sceKernelUsleep(1000000);
 sceVideoOutSetFlipRate(video,0);

 // Diagnostic: skip UserService completely and use the main user ID.
 int32_t padInit=scePadInit();
 int32_t pad=(padInit==0)?scePadOpen(ORBIS_VIDEO_USER_MAIN,0,0,0):-1;

 int selected=0,front=0;int64_t frame=1;
 drawStore(fb[front],selected);
 sceVideoOutSubmitFlip(video,front,ORBIS_VIDEO_OUT_FLIP_VSYNC,frame++);

 // Do not read buttons yet. This stage only proves Pad init/open can stay alive.
 volatile int32_t padHandle=pad;
 (void)padHandle;
 for(;;){
  sceKernelUsleep(1000000);
 }
 return 0;
}
