// Host preview of the exact framebuffer renderer used by the PS4 build.
// Build after generating ui_assets.h and ui_catalog.h:
// g++ -std=c++11 -O2 native/scripts/preview-ui.cpp -o /tmp/peppy-preview
// /tmp/peppy-preview /tmp/peppy
#define PEPPY_UI_PREVIEW
#include "../downloads.h"
#include "../install.h"
#include "../music.h"
static DownloadSnapshot previewDownload = {};
static InstallSnapshot previewInstall = {};
static MusicSnapshot previewMusic = {MUSIC_PLAYING, 0, 30, false, 0};
static int downloadCalls = 0, installCalls = 0, cancelInstallCalls = 0;
static bool rejectDownload = false, rejectInstall = false, rejectInstallBusy = false;
static InstallSpec lastInstall = {};
static const char* lastDownloadContentId = 0;
DownloadSnapshot downloadSnapshot() { return previewDownload; }
bool startDownload(const DownloadSpec& spec, const char* expectedContentId) {
    ++downloadCalls;
    lastDownloadContentId = expectedContentId;
    previewDownload = {};
    previewDownload.state = rejectDownload ? FAILED : RUNNING;
    previewDownload.total = spec.expectedBytes;
    if (rejectDownload) previewDownload.errorCode = DOWNLOAD_ERROR_THREAD;
    return !rejectDownload;
}
void cancelDownload() { previewDownload.state = CANCELLED; }
const char* downloadStageName(int stage) {
    if (stage == DOWNLOAD_STAGE_SOURCE_READ) return "Página da fonte";
    if (stage == DOWNLOAD_STAGE_SOURCE_PARSE) return "Resolver link do PKG";
    return stage == DOWNLOAD_STAGE_SEND ? "Enviar pedido" : "Diagnóstico de rede";
}
InstallSnapshot installSnapshot() { return previewInstall; }
bool startInstall(const InstallSpec& spec) {
    ++installCalls;
    if (rejectInstallBusy) return false;
    lastInstall = spec;
    uint32_t generation = previewInstall.generation + 1;
    previewInstall = {};
    previewInstall.generation = generation;
    previewInstall.state = rejectInstall ? INSTALL_FAILED : INSTALL_RUNNING;
    previewInstall.total = spec.expectedBytes;
    previewInstall.stage = rejectInstall ? INSTALL_STAGE_THREAD : INSTALL_STAGE_REGISTER;
    previewInstall.taskId = 7;
    if (rejectInstall) previewInstall.errorCode = INSTALL_ERROR_THREAD;
    return !rejectInstall;
}
void cancelInstall() { ++cancelInstallCalls; }
const char* installStageName(int stage) {
    switch (stage) {
    case INSTALL_STAGE_REGISTER: return "Registrar instalação";
    case INSTALL_STAGE_PROGRESS: return "Instalar no PS4";
    case INSTALL_STAGE_THREAD: return "Iniciar instalação";
    case INSTALL_STAGE_RESTORE: return "Restaurar acesso";
    case INSTALL_STAGE_USER: return "Usuário do PS4";
    case INSTALL_STAGE_SERVER_START: return "Preparar entrega do PKG";
    case INSTALL_STAGE_SERVER_TRANSFER: return "Entregar PKG ao instalador";
    case INSTALL_STAGE_SERVER_STOP: return "Encerrar entrega do PKG";
    default: return "Preparar instalação";
    }
}
MusicSnapshot musicSnapshot() { return previewMusic; }
bool musicStart(int32_t) { previewMusic.state = MUSIC_PLAYING; return true; }
void musicSetVolume(int volume) { previewMusic.volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume); }
void musicToggleMute() { previewMusic.muted = !previewMusic.muted; }
void musicNextTrack() { previewMusic.track = (previewMusic.track + 1) % 2; }
void musicStop() { previewMusic.state = MUSIC_STOPPED; }
void musicShutdown() { musicStop(); }
const char* musicTrackName(int track) { return track == 1 ? "ACENDAOFAROL" : "FIGHT"; }
#include "../boot_test.cpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void resetController() {
    activeCategory = 0;
    downloadingApp = installingApp = -1;
    autoInstallPending = installCancelRequested = false;
    memset(downloadedBytes, 0, sizeof(downloadedBytes));
    memset(installedApps, 0, sizeof(installedApps));
    previewDownload = {};
    previewInstall = {};
    previewMusic = {MUSIC_PLAYING, 0, 30, false, 0};
    downloadCalls = installCalls = cancelInstallCalls = 0;
    rejectDownload = rejectInstall = rejectInstallBusy = false;
    lastDownloadContentId = 0;
}

static bool expect(bool condition, const char* message) {
    if (!condition) fprintf(stderr, "Controller check failed: %s\n", message);
    return condition;
}

static void completeDownload(int index) {
    previewDownload.state = DONE;
    previewDownload.received = previewDownload.total = UI_APPS[index].sizeBytes;
}

// Exercise the user-visible handoff: one validated download, one automatic
// installation, an explicit install-only retry, and cancellation/cleanup gates.
static bool verifyController() {
    if (UI_APP_COUNT < 3) return expect(false, "preview requires at least three catalog entries");
    resetController();
    rejectDownload = true;
    activateApp(0);
    if (!expect(previewDownload.state == FAILED && !autoInstallPending,
                "an immediate download failure cannot queue an installation")) return false;
    pollAutoInstall();
    if (!expect(installCalls == 0, "failed downloads never install")) return false;

    resetController();
    activateApp(0);
    completeDownload(0);
    rejectInstall = true;
    pollAutoInstall();
    for (int i = 0; i < 20; ++i) pollAutoInstall();
    if (!expect(downloadCalls == 1 && installCalls == 1 && !autoInstallPending,
                "automatic installation is attempted exactly once, including startup failure")) return false;
    if (!expect(lastInstall.expectedBytes == UI_APPS[0].sizeBytes &&
                strcmp(lastInstall.filename, UI_APPS[0].filename) == 0 &&
                strcmp(lastInstall.name, UI_APPS[0].name) == 0,
                "installation receives the completed download's validated bytes and metadata")) return false;

    rejectInstall = false;
    activateApp(1);
    previewDownload.state = FAILED;
    pollAutoInstall();
    activateApp(0);
    if (!expect(downloadCalls == 2 && installCalls == 2,
                "install retry reuses the saved PKG after navigating to another app")) return false;
    for (int i = 0; i < 20; ++i) pollAutoInstall();
    if (!expect(installCalls == 2 && !activateApp(2),
                "an active installation blocks another download and cannot auto-retry")) return false;
    if (!expect(!cancelOperation(1) && cancelOperation(0) && cancelInstallCalls == 1,
                "Triangle cancels only the selected app's own installation")) return false;
    if (!expect(!activateApp(1), "downloads remain blocked while cancellation is running")) return false;
    previewInstall.state = INSTALL_CANCELLED;
    pollAutoInstall();
    activateApp(0);
    if (!expect(installCalls == 3 && downloadCalls == 2,
                "cancelled installation can be explicitly retried without downloading again")) return false;
    previewInstall.state = INSTALL_DONE;
    previewInstall.percent = 100;
    pollAutoInstall();
    if (!expect(installedApps[0] && !activateApp(0),
                "only a confirmed completed installation displays and records success")) return false;

    activateApp(1);
    completeDownload(1);
    pollAutoInstall();
    previewInstall.state = INSTALL_FAILED;
    previewInstall.cleanupCode = (int32_t)0x80990001;
    previewInstall.cleanupStage = INSTALL_STAGE_RESTORE;
    if (!expect(!activateApp(1) && !activateApp(2) && downloadCalls == 3 && installCalls == 4,
                "unsafe cleanup blocks both new downloads and saved-PKG retries")) return false;

    resetController();
    installingApp = 0;
    previewInstall.state = INSTALL_DONE;
    previewInstall.taskId = 7;
    downloadedBytes[1] = UI_APPS[1].sizeBytes;
    rejectInstallBusy = true;
    bool accepted = activateApp(1);
    pollAutoInstall();
    if (!expect(!accepted && installingApp == 0 && installedApps[0] && !installedApps[1],
                "a rejected start cannot attribute an older DONE snapshot to another app")) return false;
    if (!expect(downloadedBytes[1] == UI_APPS[1].sizeBytes && downloadCalls == 0,
                "a busy rejection preserves the downloaded PKG for an explicit retry")) return false;
    resetController();
    rejectInstall = true;
    downloadedBytes[0] = UI_APPS[0].sizeBytes;
    downloadedBytes[1] = UI_APPS[1].sizeBytes;
    activateApp(0);
    uint32_t firstGeneration = previewInstall.generation;
    activateApp(1);
    if (!expect(previewInstall.state == INSTALL_FAILED && installingApp == 1 &&
                previewInstall.generation != firstGeneration && !installedApps[1],
                "a new failed generation is associated with its own app even when startup returns false")) return false;
    musicToggleMute();
    musicNextTrack();
    if (!expect(previewMusic.muted && previewMusic.track == 1 && previewMusic.volume == 30,
                "sound and playlist controls preserve the default volume")) return false;
    char size[48];
    sizeLabel(size, sizeof(size), 22675456ULL);
    if (!expect(strcmp(size, "21.6 MB") == 0, "small package labels retain MB precision")) return false;
    sizeLabel(size, sizeof(size), 1024ULL * 1024 * 1024);
    if (!expect(strcmp(size, "1.0 GB") == 0, "one GiB switches the displayed unit")) return false;
    sizeLabel(size, sizeof(size), 5ULL * 1024 * 1024 * 1024);
    if (!expect(strcmp(size, "5.0 GB") == 0, "large package labels show GB")) return false;
    sizeLabel(size, sizeof(size), UINT64_MAX);
    if (!expect(strcmp(size, "17179869183.9 GB") == 0, "size labels do not overflow uint64")) return false;
    for (int i = 0; i < UI_APP_COUNT; ++i) {
        if (!UI_APPS[i].contentId[0]) continue;
        resetController();
        activateApp(i);
        if (!expect(lastDownloadContentId && strcmp(lastDownloadContentId, UI_APPS[i].contentId) == 0,
                    "a checked download receives its catalog Content ID")) return false;
        completeDownload(i);
        pollAutoInstall();
        if (!expect(installCalls == 1 && lastInstall.expectedBytes == UI_APPS[i].sizeBytes,
                    "a checked download keeps the exact size during install handoff")) return false;
        bool official = strncmp(UI_APPS[i].url, "https://github.com/", 19) == 0;
        if (!expect((strcmp(UI_APPS[i].sourceBadge, "PKG / FONTE OFICIAL") == 0) == official,
                    "checked packages display the badge for their actual source")) return false;
    }
    resetController();
    puts("Checked automatic install handoff, retry, cancellation, cleanup, music and package size labels.");
    return true;
}

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

static bool saveState(const char* prefix, const char* state, const uint32_t* frame) {
    char path[4096];
    int size = snprintf(path, sizeof(path), "%s-%s.ppm", prefix, state);
    return size > 0 && (size_t)size < sizeof(path) && save(path, frame);
}

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--check-controller") == 0) return verifyController() ? 0 : 1;
    if (argc != 2) { fprintf(stderr, "Usage: %s output-prefix | --check-controller\n", argv[0]); return 1; }
    if (!verifyController()) return 1;
    uint32_t* allocation = (uint32_t*)malloc(((size_t)W * H + 2) * sizeof(uint32_t));
    if (!allocation) return 1;
    allocation[0] = allocation[(size_t)W * H + 1] = 0xBAADF00D;
    uint32_t* frame = allocation + 1;
    int states = 0;
    for (activeCategory = 0; activeCategory < 5; ++activeCategory) {
      for (int selected = 0; selected < categoryCount(); ++selected) {
        char state[80];
        drawStore(frame, selected, 2);
        snprintf(state, sizeof(state), "home-%d-%d", activeCategory, selected);
        if (!saveState(argv[1], state, frame)) { free(allocation); return 1; }
        drawDetails(frame, selected);
        snprintf(state, sizeof(state), "details-%d-%d", activeCategory, selected);
        if (!saveState(argv[1], state, frame)) { free(allocation); return 1; }
        states += 2;
      }
    }
    resetController();
    downloadingApp = 0;
    for (int state = RUNNING; state <= CANCELLED; ++state) {
        char name[80];
        previewDownload = {};
        previewDownload.state = state;
        autoInstallPending = state == DONE;
        previewDownload.received = UI_APPS[0].sizeBytes / 2;
        previewDownload.total = UI_APPS[0].sizeBytes;
        previewDownload.errorCode = DOWNLOAD_ERROR_NETWORK;
        previewDownload.stage = DOWNLOAD_STAGE_SEND;
        previewDownload.nativeCode = (int32_t)0x80431068;
        previewDownload.networkState = 3;
        drawDetails(frame, 0);
        snprintf(name, sizeof(name), "download-%d", state);
        if (!saveState(argv[1], name, frame)) { free(allocation); return 1; }
        ++states;
    }
    for (int state = INSTALL_RUNNING; state <= INSTALL_CANCELLED; ++state) {
        resetController();
        downloadingApp = installingApp = 0;
        downloadedBytes[0] = UI_APPS[0].sizeBytes;
        completeDownload(0);
        previewInstall.state = state;
        previewInstall.total = UI_APPS[0].sizeBytes;
        previewInstall.received = previewInstall.total / 2;
        previewInstall.percent = 42;
        previewInstall.taskId = 7;
        previewInstall.stage = state == INSTALL_FAILED ? INSTALL_STAGE_REGISTER : INSTALL_STAGE_PROGRESS;
        previewInstall.errorCode = state == INSTALL_FAILED ? INSTALL_ERROR_TASK : 0;
        previewInstall.nativeCode = state == INSTALL_FAILED ? (int32_t)0x80990015 : 0;
        if (state == INSTALL_DONE) previewInstall.percent = 100;
        char name[80];
        drawDetails(frame, 0);
        snprintf(name, sizeof(name), "install-%d", state);
        if (!saveState(argv[1], name, frame)) { free(allocation); return 1; }
        drawStore(frame, 0, 2);
        snprintf(name, sizeof(name), "install-home-%d", state);
        if (!saveState(argv[1], name, frame)) { free(allocation); return 1; }
        states += 2;
    }
    resetController();
    installingApp = downloadingApp = 0;
    downloadedBytes[0] = UI_APPS[0].sizeBytes;
    completeDownload(0);
    previewInstall.state = INSTALL_RUNNING;
    previewInstall.mode = INSTALL_MODE_HTTP_LOCAL;
    previewInstall.sdkVersion = 0xFFFFFFFFU;
    previewInstall.sdkErrno = 256;
    previewInstall.stage = INSTALL_STAGE_PROGRESS;
    previewInstall.taskId = 7;
    previewInstall.percent = 42;
    previewInstall.httpRequests = 3;
    previewInstall.httpStatus = 206;
    previewInstall.httpBytes = UI_APPS[0].sizeBytes / 2;
    drawDetails(frame, 0);
    if (!saveState(argv[1], "install-http-local", frame)) { free(allocation); return 1; }
    previewInstall.state = INSTALL_FAILED;
    previewInstall.errorCode = INSTALL_ERROR_PROGRESS;
    previewInstall.stage = INSTALL_STAGE_PROGRESS;
    previewInstall.nativeCode = (int32_t)0x80991404;
    previewInstall.httpStatus = 404;
    drawDetails(frame, 0);
    if (!saveState(argv[1], "install-http-native-failure", frame)) { free(allocation); return 1; }
    states += 2;
    previewInstall.state = INSTALL_RUNNING;
    drawDetails(frame, 1);
    if (!saveState(argv[1], "install-other-app", frame)) { free(allocation); return 1; }
    installCancelRequested = true;
    drawDetails(frame, 0);
    if (!saveState(argv[1], "install-cancelling", frame)) { free(allocation); return 1; }
    states += 2;
    installCancelRequested = false;
    previewInstall.state = INSTALL_FAILED;
    previewInstall.errorCode = INSTALL_ERROR_RESTORE;
    previewInstall.stage = previewInstall.cleanupStage = INSTALL_STAGE_RESTORE;
    previewInstall.nativeCode = previewInstall.cleanupCode = (int32_t)0x80990001;
    drawDetails(frame, 0);
    if (!saveState(argv[1], "install-reopen", frame)) { free(allocation); return 1; }
    drawDetails(frame, 1);
    if (!saveState(argv[1], "install-reopen-other-app", frame)) { free(allocation); return 1; }
    drawStore(frame, 0, 2);
    if (!saveState(argv[1], "install-reopen-home", frame)) { free(allocation); return 1; }
    states += 3;

    resetController();
    for (int state = MUSIC_STOPPED; state <= MUSIC_FAILED; ++state) {
        previewMusic.state = state;
        previewMusic.track = state == MUSIC_PLAYING ? 1 : 0;
        previewMusic.errorCode = state == MUSIC_FAILED ? (int32_t)0x80260001 : 0;
        char name[80];
        drawStore(frame, 0, 2);
        snprintf(name, sizeof(name), "music-%d", state);
        if (!saveState(argv[1], name, frame)) { free(allocation); return 1; }
        ++states;
    }
    previewMusic = {MUSIC_PLAYING, 0, 30, true, 0};
    drawStore(frame, 0, 2);
    if (!saveState(argv[1], "music-muted", frame)) { free(allocation); return 1; }
    ++states;
    bool guards = allocation[0] == 0xBAADF00D && allocation[(size_t)W * H + 1] == 0xBAADF00D;
    free(allocation);
    if (!expect(guards, "rendering preserves both framebuffer guards")) return 1;
    printf("Rendered %d catalog, download, install and music states.\n", states);
    return 0;
}
