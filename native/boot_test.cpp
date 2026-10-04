#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/VideoOut.h>
#include <stdint.h>
#include <stddef.h>

int main(void)
{
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);

    int32_t video = sceVideoOutOpen(
        ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    if (video < 0) {
        for (;;) sceKernelUsleep(1000000);
    }

    const size_t width = 1920;
    const size_t height = 1080;
    const size_t bufferSize = width * height * 4;
    const size_t alignment = 0x200000;
    const size_t allocSize =
        ((bufferSize + alignment - 1) / alignment) * alignment;

    off_t directOff = 0;
    int rc = sceKernelAllocateDirectMemory(
        0, sceKernelGetDirectMemorySize(), allocSize,
        alignment, 3, &directOff);
    if (rc < 0) {
        for (;;) sceKernelUsleep(1000000);
    }

    void *framebuffer = 0;
    rc = sceKernelMapDirectMemory(
        &framebuffer, allocSize, 0x33, 0, directOff, alignment);
    if (rc < 0 || !framebuffer) {
        for (;;) sceKernelUsleep(1000000);
    }

    // Bright solid diagnostic color: ARGB-ish encoding used by OpenOrbis samples.
    uint32_t *pixels = (uint32_t *)framebuffer;
    const uint32_t solid = 0x80FF2000;
    for (size_t i = 0; i < width * height; ++i) {
        pixels[i] = solid;
    }

    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(
        &attr, 0x80000000, 1, 0,
        (uint32_t)width, (uint32_t)height, (uint32_t)width);

    void *buffers[1] = { framebuffer };
    rc = sceVideoOutRegisterBuffers(video, 0, buffers, 1, &attr);
    if (rc == 0) {
        sceVideoOutSetFlipRate(video, 0);
        sceVideoOutSubmitFlip(video, 0, ORBIS_VIDEO_OUT_FLIP_VSYNC, 1);
    }

    for (;;) {
        sceKernelUsleep(1000000);
    }
    return 0;
}
