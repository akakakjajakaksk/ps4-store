#include <orbis/libkernel.h>
#include "graphics.h"

#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1080
#define FRAME_DEPTH 4

int main()
{
    // Follow the official OpenOrbis graphics sample lifecycle:
    // heap-allocated Scene2D, frame id starting at 0, flip + wait + swap loop.
    Scene2D *scene = new Scene2D(FRAME_WIDTH, FRAME_HEIGHT, FRAME_DEPTH);

    if (!scene->Init(0xC000000, 2)) {
        for (;;) sceKernelUsleep(1000000);
    }

    Color background = { 18, 24, 38 };
    Color panel = { 35, 50, 75 };
    Color accent = { 0, 210, 255 };

    int frameID = 0;

    for (;;) {
        scene->FrameBufferFill(background);
        scene->DrawRectangle(260, 250, 1400, 580, panel);
        scene->DrawRectangle(260, 250, 1400, 20, accent);
        scene->DrawRectangle(510, 470, 900, 140, accent);
        scene->DrawRectangle(550, 510, 820, 60, background);

        scene->SubmitFlip(frameID);
        scene->FrameWait(frameID);
        scene->FrameBufferSwap();
        frameID++;
    }

    return 0;
}
