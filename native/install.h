#ifndef PEPPY_INSTALL_H
#define PEPPY_INSTALL_H

#include <stdint.h>

struct InstallSpec {
    const char* filename;
    const char* name;
    uint64_t expectedBytes;
};

enum InstallState {
    INSTALL_IDLE = 0, INSTALL_RUNNING = 1, INSTALL_DONE = 2,
    INSTALL_FAILED = 3, INSTALL_CANCELLED = 4
};

enum InstallError {
    INSTALL_ERROR_SPEC = -2000, INSTALL_ERROR_THREAD = -2001,
    INSTALL_ERROR_FILE = -2100, INSTALL_ERROR_PACKAGE = -2101,
    INSTALL_ERROR_MODULE = -2200, INSTALL_ERROR_ALREADY_INSTALLED = -2201,
    INSTALL_ERROR_BUSY = -2202, INSTALL_ERROR_SERVICE = -2203,
    INSTALL_ERROR_TASK = -2300, INSTALL_ERROR_PROGRESS = -2301,
    INSTALL_ERROR_TIMEOUT = -2302, INSTALL_ERROR_CLEANUP = -2303,
    INSTALL_ERROR_SELF = -2304, INSTALL_ERROR_SDK = -2400,
    INSTALL_ERROR_JAILBREAK = -2401, INSTALL_ERROR_GLOBAL_PATH = -2402,
    INSTALL_ERROR_COPY = -2403, INSTALL_ERROR_RESTORE = -2404
};

enum InstallStage {
    INSTALL_STAGE_NONE = 0, INSTALL_STAGE_SPEC, INSTALL_STAGE_THREAD,
    INSTALL_STAGE_FILE, INSTALL_STAGE_PACKAGE, INSTALL_STAGE_MODULE_APP,
    INSTALL_STAGE_MODULE_BGFT, INSTALL_STAGE_APP_INIT, INSTALL_STAGE_TITLE,
    INSTALL_STAGE_EXISTS, INSTALL_STAGE_HEAP, INSTALL_STAGE_BGFT_INIT,
    INSTALL_STAGE_REGISTER, INSTALL_STAGE_START, INSTALL_STAGE_PROGRESS,
    INSTALL_STAGE_CONFIRM, INSTALL_STAGE_STOP, INSTALL_STAGE_UNREGISTER,
    INSTALL_STAGE_BGFT_TERM, INSTALL_STAGE_APP_TERM, INSTALL_STAGE_FINISHED,
    INSTALL_STAGE_SDK, INSTALL_STAGE_JAILBREAK, INSTALL_STAGE_GLOBAL_PATH,
    INSTALL_STAGE_COPY, INSTALL_STAGE_RESTORE
};

struct InstallSnapshot {
    int state;
    uint64_t received;
    uint64_t total;
    int percent;
    int errorCode;
    int stage;
    int32_t nativeCode;
    int taskId;
    uint32_t progressBits;
    int preparingPercent;
    int localCopyPercent;
    int32_t cleanupCode;
    int cleanupStage;
    uint32_t generation;
};

// The controller serializes start/cancel. Only completed, validated downloads
// may be passed here; expectedBytes must be their exact final size. Installation
// keeps the PKG and never removes or overwrites an existing application.
bool startInstall(const InstallSpec& spec);
void cancelInstall();
InstallSnapshot installSnapshot();
const char* installStageName(int stage);

#endif
