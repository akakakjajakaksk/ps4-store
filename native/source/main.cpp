#include <stdio.h>
#include <unistd.h>
#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>

static void print_screen(int selected) {
    printf("\033[2J\033[H");
    printf("========================================\n");
    printf("          ORBIS STORE NATIVE 0.1        \n");
    printf("========================================\n\n");
    printf("%s DESTAQUES\n", selected == 0 ? "> " : "  ");
    printf("%s CATALOGO\n", selected == 1 ? "> " : "  ");
    printf("%s DOWNLOADS\n", selected == 2 ? "> " : "  ");
    printf("%s CONFIGURACOES\n", selected == 3 ? "> " : "  ");
    printf("\nDualShock 4: cima/baixo para navegar\n");
    printf("X = selecionar   O = voltar\n");
    printf("\nNative 0.1: interface base. Catalogo online vem na proxima etapa.\n");
}

int main() {
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_PAD);
    int user = 0;
    int pad = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
    int selected = 0;
    unsigned int oldButtons = 0;

    print_screen(selected);

    while (1) {
        OrbisPadData data;
        if (pad >= 0 && scePadReadState(pad, &data) == 0) {
            unsigned int now = data.buttons;
            unsigned int pressed = now & ~oldButtons;

            if (pressed & ORBIS_PAD_BUTTON_UP) {
                selected = (selected + 3) % 4;
                print_screen(selected);
            }
            if (pressed & ORBIS_PAD_BUTTON_DOWN) {
                selected = (selected + 1) % 4;
                print_screen(selected);
            }
            if (pressed & ORBIS_PAD_BUTTON_CROSS) {
                printf("\nSelecionado: %d\n", selected + 1);
            }
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) {
                printf("\nORBIS STORE - voltar\n");
            }
            oldButtons = now;
        }
        usleep(16000);
    }
    return 0;
}
