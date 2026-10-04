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

static void drawStore(uint32_t*p,int selected,int padState){
 const uint32_t bg=0x80080D18,nav=0x80101728,panel=0x80151E30,card=0x801B263B;
 const uint32_t accent=0x800078FF,glow=0x8030B8FF,white=0x80F5F8FF,muted=0x80879AB8,shadow=0x8003070D;
 fill(p,bg);
 rect(p,0,0,W,108,nav);rect(p,0,104,W,4,accent);
 rect(p,58,25,62,62,accent);rect(p,72,39,34,34,bg);
 text(p,150,28,"ORBIS STORE",7,white);text(p,151,82,"PS4 HOME",2,muted);
 if(padState==2){rect(p,1180,36,190,38,0x8014352B);text(p,1205,46,"PAD READ",3,0x8060FF90);}

 rect(p,1430,28,420,56,panel);border(p,1430,28,420,56,2,0x80364A68);text(p,1470,45,"SEARCH",3,muted);

 rect(p,42,145,310,875,shadow);rect(p,50,137,310,875,panel);
 const char*menu[4]={"HOME","GAMES","APPS","INSTALL"};
 for(int i=0;i<4;i++){int yy=190+i*92;if(i==0){rect(p,50,yy-18,310,66,0x80203855);rect(p,50,yy-18,7,66,accent);}text(p,92,yy,menu[i],4,i==0?white:muted);}
 text(p,92,930,"ORBIS",3,muted);text(p,92,970,"STORE",3,muted);

 rect(p,405,145,1460,292,shadow);rect(p,413,137,1460,292,0x8014243C);
 rect(p,413,137,14,292,accent);
 text(p,475,185,"ORBIS STORE",8,white);text(p,478,260,"HOME FOR PS4 APPS",4,muted);
 rect(p,478,337,250,58,accent);text(p,530,352,"OPEN",4,white);
 rect(p,1510,185,250,150,0x801C3456);border(p,1510,185,250,150,3,glow);
 rect(p,1580,215,110,90,accent);rect(p,1603,238,64,44,0x8014243C);

 const int y=500,cw=330,ch=390,gap=27,start=413;
 const char*names[4]={"GAME 1","APP 2","GAME 3","APP 4"};
 const uint32_t covers[4]={0x80304D82,0x80513B68,0x802E6255,0x80604D32};
 for(int i=0;i<4;i++){
  int x=start+i*(cw+gap);
  rect(p,x+8,y+10,cw,ch,shadow);rect(p,x,y,cw,ch,card);
  rect(p,x,y,cw,225,covers[i]);
  rect(p,x+22,y+20,286,8,i==selected?accent:0x80445A78);
  rect(p,x+118,y+68,94,94,0x80212E47);border(p,x+118,y+68,94,94,3,i==selected?glow:muted);
  text(p,x+25,y+254,names[i],4,white);text(p,x+25,y+300,"INSTALL",3,muted);
  rect(p,x+25,y+342,132,34,i==selected?accent:0x802B3A54);text(p,x+42,y+350,"OPEN",3,white);
 }
 int sx=start+selected*(cw+gap);
 border(p,sx-7,y-7,cw+14,ch+14,5,glow);
 rect(p,413,930,1460,72,nav);text(p,455,952,"SELECT",3,white);text(p,720,952,"BACK",3,muted);
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

 int32_t userModule=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE);
 int32_t userId=-1,userRc=userModule;
 if(userModule>=0){
  OrbisUserServiceInitializeParams usp;
  usp.priority=ORBIS_KERNEL_PRIO_FIFO_LOWEST;
  userRc=sceUserServiceInitialize(&usp);
  if(userRc==0) userRc=sceUserServiceGetInitialUser(&userId);
 }
 int32_t padModule=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_PAD);
 int32_t padInit=(padModule>=0)?scePadInit():padModule;
 int32_t pad=(padInit==0 && userRc==0)?scePadOpen(userId,0,0,0):-1;

 int selected=0,front=0;uint32_t prev=0;int64_t frame=1;
 drawStore(fb[front],selected,2);
 sceVideoOutSubmitFlip(video,front,ORBIS_VIDEO_OUT_FLIP_VSYNC,frame++);

 for(;;){
  OrbisPadData pd;
  if(pad>=0 && scePadReadState(pad,&pd)>=0){
   static bool readShown=false;
   if(!readShown){front=1-front;drawStore(fb[front],selected,2);sceVideoOutSubmitFlip(video,front,ORBIS_VIDEO_OUT_FLIP_VSYNC,frame++);readShown=true;}
   uint32_t now=pd.buttons;
   bool changed=false;
   if((now&ORBIS_PAD_BUTTON_RIGHT)&&!(prev&ORBIS_PAD_BUTTON_RIGHT)){selected=(selected+1)%4;changed=true;}
   if((now&ORBIS_PAD_BUTTON_LEFT)&&!(prev&ORBIS_PAD_BUTTON_LEFT)){selected=(selected+3)%4;changed=true;}
   prev=now;
   if(changed){
    front=1-front;
    drawStore(fb[front],selected,(pad>=0)?1:0);
    sceVideoOutSubmitFlip(video,front,ORBIS_VIDEO_OUT_FLIP_VSYNC,frame++);
   }
  }
  sceKernelUsleep(16000);
 }
 return 0;
}
