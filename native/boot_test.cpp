#include <orbis/libkernel.h>

int main(void)
{
    // Control probe: no VideoOut headers, library, or calls.
    // Expected result on PS4: app stays alive on a black screen.
    for (;;) {
        sceKernelUsleep(1000000);
    }
    return 0;
}
