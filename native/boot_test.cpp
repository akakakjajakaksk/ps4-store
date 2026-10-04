#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <stdint.h>

int main(void)
{
    // Probe the internal VideoOut module loader without calling sceVideoOutOpen.
    // If this stays alive, module loading works and the crash is in the API call path.
    volatile uint32_t rc = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);
    (void)rc;

    for (;;) {
        sceKernelUsleep(1000000);
    }
    return 0;
}
