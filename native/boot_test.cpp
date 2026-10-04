#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <stdint.h>

int main(void)
{
    // Minimal VideoOut probe: no Scene2D, no framebuffer allocation.
    // If this stays alive, the crash is later in graphics initialization.
    int video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);

    // Keep the process alive whether open succeeds or fails.
    // This isolates sceVideoOutOpen itself from the rest of the renderer.
    volatile int result = video;
    (void)result;

    for (;;) {
        sceKernelUsleep(1000000);
    }

    return 0;
}
