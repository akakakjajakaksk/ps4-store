#include <orbis/libkernel.h>

int main(void)
{
    for (;;) {
        sceKernelUsleep(1000000);
    }
    return 0;
}
