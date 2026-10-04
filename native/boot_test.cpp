#include <orbis/libkernel.h>
#include "graphics.h"

#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1080
#define FRAME_DEPTH 4

int main(void)
{
    Scene2D scene(FRAME_WIDTH, FRAME_HEIGHT, FRAME_DEPTH);
    if (!scene.Init(0xC000000, 2)) {
        for (;;) sceKernelUsleep(1000000);
    }

    Color background = { 18, 24, 38 };
    Color panel = { 35, 50, 75 };
    Color accent = { 0, 210, 255 };

    scene.SetActiveFrameBuffer(0);
    scene.FrameBufferFill(background);
    scene.DrawRectangle(260, 250, 1400, 580, panel);
    scene.DrawRectangle(260, 250, 1400, 20, accent);
    scene.DrawRectangle(510, 470, 900, 140, accent);
    scene.DrawRectangle(550, 510, 820, 60, background);
    scene.SubmitFlip(1);
    scene.FrameWait(1);

    for (;;) sceKernelUsleep(1000000);
    return 0;
}
