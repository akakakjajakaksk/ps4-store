#include <stdio.h>
#include <unistd.h>
#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>

struct App {
    const char* name;
    const char* category;
    const char* version;
    const char* developer;
};

static App catalog[] = {
    {"Apollo Save Tool", "Utilitarios", "2.3.2", "bucanero"},
    {"ezRemote Client", "Utilitarios", "2.00", "cy33hc"},
    {"ItemzFlow", "Utilitarios", "1.08", "LightningMods"},
    {"Homebrew Store", "Utilitarios", "4.4", "LightningMods"}
};
static const int APP_COUNT = sizeof(catalog) / sizeof(catalog[0]);

enum Screen { HOME, CATALOG, DETAILS, DOWNLOADS, SETTINGS };
static Screen screen = HOME;
static int selected = 0;

static void clear_screen() { printf("\033[2J\033[H"); }

static void draw_header(const char* title) {
    clear_screen();
    printf("=============================================\n");
    printf("       ORBIS STORE NATIVE 0.2 - PS4          \n");
    printf("=============================================\n");
    printf("%s\n\n", title);
}

static void draw_home() {
    draw_header("HOME");
    const char* items[]={"DESTAQUES","CATALOGO","DOWNLOADS","CONFIGURACOES"};
    for(int i=0;i<4;i++) printf("%s %s\n", selected==i?">":" ", items[i]);
    printf("\nX selecionar | setas navegar\n");
}

static void draw_catalog() {
    draw_header("CATALOGO HOMEBREW");
    for(int i=0;i<APP_COUNT;i++)
        printf("%s %-22s  v%s\n", selected==i?">":" ", catalog[i].name, catalog[i].version);
    printf("\nX detalhes | O voltar\n");
}

static void draw_details() {
    App &a=catalog[selected];
    draw_header("DETALHES");
    printf("%s\n\nCategoria: %s\nVersao: %s\nDev: %s\n",a.name,a.category,a.version,a.developer);
    printf("\n[ INSTALAR - EM DESENVOLVIMENTO ]\n");
    printf("\nO voltar\n");
}

static void draw_simple(const char* title,const char* text) {
    draw_header(title); printf("%s\n\nO voltar\n",text);
}

static void redraw() {
    if(screen==HOME) draw_home();
    else if(screen==CATALOG) draw_catalog();
    else if(screen==DETAILS) draw_details();
    else if(screen==DOWNLOADS) draw_simple("DOWNLOADS","Nenhum download nativo iniciado.");
    else draw_simple("CONFIGURACOES","Orbis Store Native 0.2");
}

int main() {
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_PAD);
    int pad=scePadOpen(0,ORBIS_PAD_PORT_TYPE_STANDARD,0,NULL);
    unsigned int oldButtons=0;
    redraw();

    while(1) {
        OrbisPadData data;
        if(pad>=0 && scePadReadState(pad,&data)==0) {
            unsigned int now=data.buttons, pressed=now & ~oldButtons;
            int max=(screen==HOME?4:(screen==CATALOG?APP_COUNT:1));

            if((pressed & ORBIS_PAD_BUTTON_UP) && (screen==HOME || screen==CATALOG)) {
                selected=(selected+max-1)%max; redraw();
            }
            if((pressed & ORBIS_PAD_BUTTON_DOWN) && (screen==HOME || screen==CATALOG)) {
                selected=(selected+1)%max; redraw();
            }
            if(pressed & ORBIS_PAD_BUTTON_CROSS) {
                if(screen==HOME) {
                    if(selected==1){screen=CATALOG;selected=0;}
                    else if(selected==2){screen=DOWNLOADS;selected=0;}
                    else if(selected==3){screen=SETTINGS;selected=0;}
                    else {screen=CATALOG;selected=0;}
                } else if(screen==CATALOG) screen=DETAILS;
                redraw();
            }
            if(pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                if(screen==DETAILS){screen=CATALOG;}
                else if(screen!=HOME){screen=HOME;selected=0;}
                redraw();
            }
            oldButtons=now;
        }
        usleep(16000);
    }
    return 0;
}
