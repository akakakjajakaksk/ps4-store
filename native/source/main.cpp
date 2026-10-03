#include <stdio.h>
#include <orbis/libkernel.h>

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("ORBIS STORE NATIVE 0.4\n");
    printf("BOOT OK - first package validation build\n");

    for (;;) {
        sceKernelUsleep(1000000);
    }
    return 0;
}
