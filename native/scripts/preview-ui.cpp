// Host preview of the exact framebuffer renderer used by the PS4 build.
// Build after generating ui_assets.h:
// g++ -std=c++11 -O2 native/scripts/preview-ui.cpp -o /tmp/peppy-preview
// /tmp/peppy-preview /tmp/peppy
#define PEPPY_UI_PREVIEW
#include "../downloads.h"
static DownloadSnapshot previewDownload = {};
DownloadSnapshot downloadSnapshot() { return previewDownload; }
const char* downloadStageName(int stage) {
    return stage == DOWNLOAD_STAGE_SEND ? "Enviar pedido" : "Diagnóstico de rede";
}
#include "../boot_test.cpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool save(const char* path, const uint32_t* frame) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; ++i) {
        uint8_t rgb[3] = {(uint8_t)(frame[i] >> 16), (uint8_t)(frame[i] >> 8), (uint8_t)frame[i]};
        if (fwrite(rgb, 1, 3, f) != 3) { fclose(f); return false; }
    }
    return fclose(f) == 0;
}

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "Usage: %s output-prefix\n", argv[0]); return 1; }
    uint32_t* frame = (uint32_t*)malloc((size_t)W * H * sizeof(uint32_t));
    if (!frame) return 1;
    int states = 0;
    for (activeCategory = 0; activeCategory < 5; ++activeCategory) {
      for (int selected = 0; selected < categoryCount(); ++selected) {
        char path[4096];
        drawStore(frame, selected, 2);
        snprintf(path, sizeof(path), "%s-home-%d-%d.ppm", argv[1], activeCategory, selected);
        if (!save(path, frame)) { free(frame); return 1; }
        drawDetails(frame, selected);
        snprintf(path, sizeof(path), "%s-details-%d-%d.ppm", argv[1], activeCategory, selected);
        if (!save(path, frame)) { free(frame); return 1; }
        states += 2;
      }
    }
    activeCategory = 0;
    downloadingApp = 0;
    for (int state = RUNNING; state <= CANCELLED; ++state) {
        char path[4096];
        previewDownload = {};
        previewDownload.state = state;
        previewDownload.received = UI_APPS[0].sizeBytes / 2;
        previewDownload.total = UI_APPS[0].sizeBytes;
        previewDownload.errorCode = DOWNLOAD_ERROR_NETWORK;
        previewDownload.stage = DOWNLOAD_STAGE_SEND;
        previewDownload.nativeCode = (int32_t)0x80431068;
        previewDownload.networkState = 3;
        drawDetails(frame, 0);
        snprintf(path, sizeof(path), "%s-download-%d.ppm", argv[1], state);
        if (!save(path, frame)) { free(frame); return 1; }
        ++states;
    }
    free(frame);
    printf("Rendered %d catalog, category and download states.\n", states);
    return 0;
}
