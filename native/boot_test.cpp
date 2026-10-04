#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/VideoOut.h>
#include <stdint.h>

int main(void)
{
    // Stage 2 VideoOut probe:
    // 1) load the internal VideoOut module (stage 1 stayed alive on hardware)
    // 2) open the main VideoOut bus
    // Keep the process alive without registering buffers yet.
    volatile uint32_t module_rc =
        sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);
    (void)module_rc;

    volatile int32_t video =
        sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    (void)video;

    for (;;) {
        sceKernelUsleep(1000000);
    }
    return 0;
}
