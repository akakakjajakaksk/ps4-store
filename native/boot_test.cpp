#include <stdint.h>
#include <stddef.h>
#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>

static const int W = 1920;
static const int H = 1080;
static const size_t FB_SIZE = (size_t)W * H * 4;

static void fill(uint32_t *fb, uint32_t color) {
    for (size_t i = 0; i < (size_t)W * H; ++i) fb[i] = color;
}

static void rect(uint32_t *fb, int x, int y, int w, int h, uint32_t color) {
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx)
            if (xx >= 0 && xx < W && yy >= 0 && yy < H)
                fb[(size_t)yy * W + xx] = color;
}

int main(void) {
    int video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    if (video < 0) for (;;) sceKernelUsleep(1000000);

    off_t offset = 0;
    size_t allocSize = (FB_SIZE + 0x1fffff) & ~((size_t)0x1fffff);
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), allocSize,
        0x200000, 3, &offset) < 0) for (;;) sceKernelUsleep(1000000);

    void *mem = 0;
    if (sceKernelMapDirectMemory(&mem, allocSize, 0x33, 0, offset, 0x200000) < 0)
        for (;;) sceKernelUsleep(1000000);

    void *buffers[1] = { mem };
    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, W, H, W);
    if (sceVideoOutRegisterBuffers(video, 0, buffers, 1, &attr) < 0)
        for (;;) sceKernelUsleep(1000000);

    uint32_t *fb = (uint32_t *)mem;
    fill(fb, 0x80101828);
    rect(fb, 260, 260, 1400, 560, 0x80202f46);
    rect(fb, 260, 260, 1400, 18, 0x8000d8ff);
    rect(fb, 520, 470, 880, 140, 0x8000d8ff);
    rect(fb, 560, 510, 800, 60, 0x80101828);

    sceVideoOutSetFlipRate(video, 0);
    sceVideoOutSubmitFlip(video, 0, ORBIS_VIDEO_OUT_FLIP_VSYNC, 1);

    for (;;) sceKernelUsleep(1000000);
    return 0;
}
