#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/VideoOut.h>
#include <stdint.h>
#include <stddef.h>

static const int W = 1920;
static const int H = 1080;

static void fill(uint32_t *p, uint32_t c)
{
    for (int i = 0; i < W * H; ++i) p[i] = c;
}

static void rect(uint32_t *p, int x, int y, int w, int h, uint32_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx)
            p[yy * W + xx] = c;
}

static void border(uint32_t *p, int x, int y, int w, int h, int t, uint32_t c)
{
    rect(p, x, y, w, t, c);
    rect(p, x, y + h - t, w, t, c);
    rect(p, x, y, t, h, c);
    rect(p, x + w - t, y, t, h, c);
}

static void drawStore(uint32_t *p)
{
    const uint32_t bg     = 0x80101420;
    const uint32_t top    = 0x80192334;
    const uint32_t panel  = 0x80212D42;
    const uint32_t card   = 0x802B3A54;
    const uint32_t accent = 0x8000A8FF;
    const uint32_t white  = 0x80F4F7FF;
    const uint32_t muted  = 0x80788AA8;

    fill(p, bg);

    // Top navigation bar.
    rect(p, 0, 0, W, 120, top);
    rect(p, 0, 116, W, 4, accent);

    // Simple Orbis Store logo mark.
    rect(p, 70, 34, 52, 52, accent);
    rect(p, 82, 46, 28, 28, top);

    // Decorative title bars: visual placeholder until font rendering is added.
    rect(p, 150, 43, 250, 18, white);
    rect(p, 150, 69, 145, 10, muted);

    // Search / status area.
    rect(p, 1450, 36, 380, 50, panel);
    border(p, 1450, 36, 380, 50, 2, muted);
    rect(p, 1480, 55, 210, 10, muted);

    // Sidebar.
    rect(p, 55, 170, 330, 830, panel);
    rect(p, 55, 190, 8, 86, accent);
    rect(p, 95, 215, 190, 16, white);
    rect(p, 95, 330, 145, 14, muted);
    rect(p, 95, 410, 170, 14, muted);
    rect(p, 95, 490, 125, 14, muted);
    rect(p, 95, 570, 155, 14, muted);

    // Featured banner.
    rect(p, 430, 170, 1435, 300, panel);
    border(p, 430, 170, 1435, 300, 3, accent);
    rect(p, 485, 225, 510, 28, white);
    rect(p, 485, 275, 700, 14, muted);
    rect(p, 485, 310, 590, 14, muted);
    rect(p, 485, 370, 210, 54, accent);
    rect(p, 525, 391, 130, 12, white);

    // Game/app cards.
    const int y = 525;
    const int cw = 325;
    const int ch = 385;
    const int gap = 30;
    const int start = 430;
    for (int i = 0; i < 4; ++i) {
        int x = start + i * (cw + gap);
        rect(p, x, y, cw, ch, card);
        rect(p, x, y, cw, 230, (i == 0) ? 0x804B65A0 :
                              (i == 1) ? 0x80633E72 :
                              (i == 2) ? 0x803A6B58 : 0x80715A35);
        rect(p, x + 25, y + 260, 210, 17, white);
        rect(p, x + 25, y + 295, 155, 10, muted);
        rect(p, x + 25, y + 335, 92, 28, accent);
    }

    // Selected first card.
    border(p, start - 5, y - 5, cw + 10, ch + 10, 5, accent);

    // Bottom hint strip.
    rect(p, 430, 950, 1435, 50, top);
    rect(p, 470, 970, 115, 10, white);
    rect(p, 650, 970, 150, 10, muted);
}

int main(void)
{
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);

    int32_t video = sceVideoOutOpen(
        ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    if (video < 0) for (;;) sceKernelUsleep(1000000);

    const size_t bufferSize = (size_t)W * H * 4;
    const size_t alignment = 0x200000;
    const size_t allocSize =
        ((bufferSize + alignment - 1) / alignment) * alignment;

    off_t directOff = 0;
    int rc = sceKernelAllocateDirectMemory(
        0, sceKernelGetDirectMemorySize(), allocSize,
        alignment, 3, &directOff);
    if (rc < 0) for (;;) sceKernelUsleep(1000000);

    void *framebuffer = 0;
    rc = sceKernelMapDirectMemory(
        &framebuffer, allocSize, 0x33, 0, directOff, alignment);
    if (rc < 0 || !framebuffer) for (;;) sceKernelUsleep(1000000);

    drawStore((uint32_t *)framebuffer);

    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(
        &attr, 0x80000000, 1, 0, W, H, W);

    void *buffers[1] = { framebuffer };
    rc = sceVideoOutRegisterBuffers(video, 0, buffers, 1, &attr);
    if (rc == 0) {
        sceVideoOutSetFlipRate(video, 0);
        sceVideoOutSubmitFlip(video, 0, ORBIS_VIDEO_OUT_FLIP_VSYNC, 1);
    }

    for (;;) sceKernelUsleep(1000000);
    return 0;
}
