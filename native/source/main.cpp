#include <SDL2/SDL.h>
#include <stdio.h>

struct App { const char* name; const char* category; const char* version; };
static App apps[]={
 {"Apollo Save Tool","UTILITARIOS","2.3.2"},
 {"ezRemote Client","UTILITARIOS","2.00"},
 {"ItemzFlow","UTILITARIOS","1.08"},
 {"Homebrew Store","UTILITARIOS","4.4"}
};
static const int APP_COUNT=sizeof(apps)/sizeof(apps[0]);
static int selected=0;

static void rect(SDL_Renderer* r,int x,int y,int w,int h,Uint8 rr,Uint8 g,Uint8 b,Uint8 a){
 SDL_Rect q={x,y,w,h}; SDL_SetRenderDrawColor(r,rr,g,b,a); SDL_RenderFillRect(r,&q);
}
static void draw(SDL_Renderer* r){
 SDL_SetRenderDrawColor(r,3,18,48,255); SDL_RenderClear(r);
 rect(r,0,0,1920,92,4,31,72,255);
 rect(r,0,92,1920,300,5,67,135,255);
 rect(r,85,132,8,210,88,195,255,255);
 for(int i=0;i<APP_COUNT;i++){
   int x=90+i*330,y=455,w=285,h=330;
   if(i==selected){rect(r,x-8,y-8,w+16,h+16,120,210,255,255);}
   rect(r,x,y,w,h,9,43,86,255);
   rect(r,x,y,w,190,(Uint8)(20+i*16),(Uint8)(95+i*10),(Uint8)(165+i*8),255);
   rect(r,x+20,y+225,w-40,14,220,235,248,255);
   rect(r,x+20,y+258,(w-40)*2/3,9,110,145,180,255);
 }
 rect(r,90,860,1740,2,52,102,153,255);
 SDL_RenderPresent(r);
}
int main(){
 if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER)!=0) return 1;
 SDL_Window* win=SDL_CreateWindow("Orbis Store Native 0.3",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,1920,1080,SDL_WINDOW_FULLSCREEN);
 if(!win) return 2;
 SDL_Renderer* r=SDL_CreateRenderer(win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
 if(!r) return 3;
 SDL_GameController* pad=NULL;
 for(int i=0;i<SDL_NumJoysticks();i++) if(SDL_IsGameController(i)){pad=SDL_GameControllerOpen(i);break;}
 int run=1; draw(r);
 while(run){
   SDL_Event e;
   while(SDL_PollEvent(&e)){
     if(e.type==SDL_QUIT) run=0;
     if(e.type==SDL_CONTROLLERBUTTONDOWN){
       if(e.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT){selected=(selected+APP_COUNT-1)%APP_COUNT;draw(r);}
       if(e.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT){selected=(selected+1)%APP_COUNT;draw(r);}
       if(e.cbutton.button==SDL_CONTROLLER_BUTTON_B) run=0;
     }
     if(e.type==SDL_KEYDOWN){
       if(e.key.keysym.sym==SDLK_LEFT){selected=(selected+APP_COUNT-1)%APP_COUNT;draw(r);}
       if(e.key.keysym.sym==SDLK_RIGHT){selected=(selected+1)%APP_COUNT;draw(r);}
     }
   }
   SDL_Delay(8);
 }
 if(pad) SDL_GameControllerClose(pad);
 SDL_DestroyRenderer(r); SDL_DestroyWindow(win); SDL_Quit(); return 0;
}
