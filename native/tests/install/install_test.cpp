#include <assert.h>
#include <atomic>
#include <stdarg.h>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

static std::string testRoot, localDirectory, systemDirectory, copyDirectory, systemRoot;
#define PEPPY_DOWNLOAD_DIRECTORY localDirectory.c_str()
#define PEPPY_INSTALL_SYSTEM_DIRECTORY systemDirectory.c_str()
#define PEPPY_INSTALL_COPY_DIRECTORY copyDirectory.c_str()
#define PEPPY_INSTALL_ROOT_DIRECTORY systemRoot.c_str()
#include "../../install.cpp"

enum Mode {
    NORMAL, NO_SDK, JAILBREAK_FAIL, RESTORE_FAIL, MODULE_FAIL, MODULE_LOADED,
    LOAD_FAIL_BUT_PRESENT, APP_INIT_FAIL, TITLE_FAIL, BAD_TITLE, NON_APP, SELF_APP,
    EXISTS_FAIL, APP_EXISTS, BGFT_INIT_FAIL, REGISTER_FAIL, REGISTER_EXISTS,
    REGISTER_BUSY, START_FAIL, PROGRESS_API_FAIL, PROGRESS_RESULT_FAIL,
    OVERFLOW_PROGRESS, ZERO_PROGRESS, BLOCK_PROGRESS, STOP_FAIL, UNREGISTER_FAIL,
    BGFT_TERM_FAIL, APP_TERM_FAIL, ATTR_FAIL, DETACH_FAIL, CREATE_FAIL,
    DELAY_CONFIRM, CONFIRM_FAIL, ALIAS_COPY, COPY_ENOSPC, COPY_FSYNC_FAIL,
    COPY_CLOSE_FAIL, COPY_RENAME_FAIL, COPY_CANCEL, BAD_GLOBAL_ROOT,
    SOURCE_STAT_FAIL, GLOBAL_STAT_FAIL, COPY_STAT_FAIL, COPY_REPLACED_PATH,
    ALIAS_DIFFERENT_INODE, ALIAS_SYMLINK, COPY_DIRECTORY_SYMLINK
};
static Mode mode;
static int modules, moduleProbes, appInitCalls, appTermCalls, bgftInitCalls, bgftTermCalls;
static int registerCalls, startCalls, progressCalls, existsCalls, stopCalls, unregisterCalls;
static int jailbreakCalls, restoreCalls, titleCalls;
static bool jailbroken, nativeStarted;
static std::atomic<bool> block(false), entered(false);
static std::string registeredPath;
static std::map<int, std::string> descriptorPaths;
static int unavailableLstatCalls, noFollowOpens;
static const int32_t RAW = (int32_t)0x8099ee01U;
static const uint64_t NATIVE_LENGTH = 5ULL * 1024 * 1024 * 1024;
static bool needsCopy(Mode value) {
    return (value >= ALIAS_COPY && value <= BAD_GLOBAL_ROOT) ||
           value == COPY_STAT_FAIL || value == COPY_REPLACED_PATH ||
           value == ALIAS_DIFFERENT_INODE || value == ALIAS_SYMLINK || value == COPY_DIRECTORY_SYMLINK;
}

extern "C" int32_t sceKernelUsleep(uint32_t) { usleep(20); return 0; }
extern "C" int32_t scePthreadAttrInit(OrbisPthreadAttr* value) {
    *value = 1; return mode == ATTR_FAIL ? RAW : 0;
}
extern "C" int32_t scePthreadAttrDestroy(OrbisPthreadAttr*) { return 0; }
extern "C" int32_t scePthreadAttrSetdetachstate(OrbisPthreadAttr*, int value) {
    assert(value == 1); return mode == DETACH_FAIL ? RAW : 0;
}
extern "C" int32_t scePthreadCreate(OrbisPthread* thread, const OrbisPthreadAttr*,
                                    void*(*entry)(void*), void* argument, const char* name) {
    assert(!strcmp(name, "peppy-install"));
    if (mode == CREATE_FAIL) return RAW;
    *thread = 1;
    std::thread(entry, argument).detach();
    return 0;
}
extern "C" int32_t peppyInstallModuleLoaded(OrbisSysModuleInternal) {
    ++moduleProbes;
    if (mode == MODULE_LOADED || (mode == LOAD_FAIL_BUT_PRESENT && moduleProbes % 2 == 0)) return 0;
    return -1;
}
extern "C" uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal id) {
    assert(jailbroken);
    assert(id == ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL || id == ORBIS_SYSMODULE_INTERNAL_BGFT);
    ++modules;
    return mode == MODULE_FAIL || mode == LOAD_FAIL_BUT_PRESENT ? (uint32_t)RAW : 0;
}
extern "C" int32_t peppyInstallSdkVersion() { return mode == NO_SDK ? -1 : 0x100; }
extern "C" int32_t peppyInstallJailbreak(PeppyJailbreakBackup* backup) {
    assert(!jailbroken);
    ++jailbreakCalls;
    if (mode == JAILBREAK_FAIL) return RAW;
    backup->paid = 0x123456789abc;
    backup->rdir = (void*)0x1234;
    jailbroken = true;
    if (needsCopy(mode)) {
        // The namespace-visible original path changes after jailbreak. Copy
        // must still read the validated old inode, never reopen this path.
        std::string original = localDirectory + "/apollo.pkg";
        std::string moved = localDirectory + "/old-inode.pkg";
        assert(rename(original.c_str(), moved.c_str()) == 0);
        FILE* file = fopen(original.c_str(), "wb");
        assert(file);
        unsigned char invalid[8192] = {};
        assert(fwrite(invalid, 1, sizeof(invalid), file) == sizeof(invalid));
        assert(fclose(file) == 0);
    }
    return 0;
}
extern "C" int32_t peppyInstallRestore(PeppyJailbreakBackup* backup) {
    assert(jailbroken && backup->paid == 0x123456789abc && backup->rdir == (void*)0x1234);
    ++restoreCalls;
    if (mode == RESTORE_FAIL) return RAW;
    jailbroken = false;
    return 0;
}
extern "C" int32_t sceAppInstUtilInitialize() {
    assert(jailbroken); ++appInitCalls; return mode == APP_INIT_FAIL ? RAW : 0;
}
extern "C" int32_t sceAppInstUtilTerminate() {
    assert(jailbroken); ++appTermCalls; return mode == APP_TERM_FAIL ? RAW : 0;
}
extern "C" int32_t sceAppInstUtilGetTitleIdFromPkg(const char* path, char* title, int32_t* isApp) {
    assert(jailbroken); ++titleCalls;
    assert(std::string(path).find(systemRoot) == 0);
    FILE* file = fopen(path, "rb");
    assert(file); unsigned char magic[4]; assert(fread(magic, 1, 4, file) == 4);
    assert(fclose(file) == 0 && magic[0] == 0x7f && magic[1] == 'C');
    if (mode == TITLE_FAIL) return RAW;
    strcpy(title, mode == BAD_TITLE ? "invalid" : mode == SELF_APP ? "BREW00001" : "APOL00004");
    *isApp = mode == NON_APP ? 0 : 1;
    return 0;
}
extern "C" int32_t sceAppInstUtilAppExists(const char* title, int32_t* exists) {
    assert(jailbroken && !strcmp(title, "APOL00004"));
    ++existsCalls;
    if (mode == EXISTS_FAIL || (mode == CONFIRM_FAIL && existsCalls > 1)) return RAW;
    *exists = mode == APP_EXISTS || (nativeStarted && progressCalls >= 3) ? 1 : 0;
    return 0;
}
extern "C" bool sceAppInstUtilAppIsInInstalling(const char* id) {
    assert(jailbroken && !strcmp(id, "UP0001-APOL00004_00-0000000000000000"));
    return mode == DELAY_CONFIRM && progressCalls < 5;
}
extern "C" int32_t peppyBgftInit(PeppyBgftInit* init) {
    assert(jailbroken && init->heap && init->heapSize == 1024 * 1024);
    ++bgftInitCalls;
    return mode == BGFT_INIT_FAIL ? RAW : 0;
}
extern "C" int32_t peppyBgftRegister(PeppyBgftParamEx* params, int32_t* task) {
    assert(jailbroken && params->slot == 0 && params->params.entitlementType == 5);
    assert(params->params.packageSize == 8192 && params->params.option == 2);
    assert(!strcmp(params->params.id, "UP0001-APOL00004_00-0000000000000000"));
    assert(!strcmp(params->params.playgoScenarioId, "0"));
    assert(!strstr(params->params.contentUrl, "://"));
    registeredPath = params->params.contentUrl;
    ++registerCalls;
    if (mode == REGISTER_FAIL) return RAW;
    if (mode == REGISTER_EXISTS) { *task = 42; return (int32_t)0x80990088U; }
    if (mode == REGISTER_BUSY) return (int32_t)0x80990086U;
    assert(registeredPath == (needsCopy(mode) ? copyDirectory : systemDirectory) + "/apollo.pkg");
    *task = 17;
    return 0;
}
extern "C" int32_t sceBgftServiceDownloadStartTask(int32_t task) {
    assert(jailbroken && task == 17); ++startCalls;
    if (mode == START_FAIL) return RAW;
    nativeStarted = true;
    return 0;
}
extern "C" int32_t peppyBgftProgress(int32_t task, PeppyBgftProgress* progress) {
    assert(jailbroken && task == 17); ++progressCalls;
    if (mode == BLOCK_PROGRESS || mode == STOP_FAIL || mode == UNREGISTER_FAIL) {
        entered.store(true);
        while (block.load()) usleep(20);
    }
    if (mode == PROGRESS_API_FAIL) return RAW;
    progress->errorResult = mode == PROGRESS_RESULT_FAIL ? RAW : 0;
    if (mode == ZERO_PROGRESS) return 0;
    progress->length = NATIVE_LENGTH;
    progress->lengthTotal = NATIVE_LENGTH;
    progress->transferred = progress->transferredTotal =
        progressCalls == 1 ? 0 : progressCalls == 2 ? NATIVE_LENGTH / 2 : NATIVE_LENGTH;
    if (mode == OVERFLOW_PROGRESS) ++progress->transferredTotal;
    progress->bits = 0x10203040;
    progress->preparingPercent = 100;
    progress->localCopyPercent = progressCalls >= 3 ? 100 : 20;
    return 0;
}
extern "C" int32_t sceBgftServiceDownloadStopTask(int32_t task) {
    assert(jailbroken && task == 17); ++stopCalls; return mode == STOP_FAIL ? RAW : 0;
}
extern "C" int32_t sceBgftServiceIntDownloadUnregisterTask(int32_t task) {
    assert(jailbroken && task == 17); ++unregisterCalls; return mode == UNREGISTER_FAIL ? RAW : 0;
}
extern "C" int32_t sceBgftServiceIntTerm() {
    assert(jailbroken); ++bgftTermCalls; return mode == BGFT_TERM_FAIL ? RAW : 0;
}
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" int __real_fsync(int);
extern "C" int __real_close(int);
extern "C" int __real_rename(const char*, const char*);
extern "C" int __real_open(const char*, int, ...);
extern "C" int __real_fstat(int, struct stat*);
extern "C" int __wrap_lstat(const char*, struct stat*) {
    // Match OpenOrbis' PS4 implementation for every test, not just one mode.
    ++unavailableLstatCalls;
    errno = ENOSYS;
    return -1;
}
extern "C" int __wrap_open(const char* path, int flags, ...) {
    assert(flags & O_NOFOLLOW);
    ++noFollowOpens;
    mode_t permissions = 0;
    if (flags & O_CREAT) {
        va_list arguments; va_start(arguments, flags);
        permissions = (mode_t)va_arg(arguments, int); va_end(arguments);
    }
    int descriptor = __real_open(path, flags, permissions);
    if (descriptor >= 0) descriptorPaths[descriptor] = path;
    return descriptor;
}
// glibc Fortify redirects nonconstant two-argument open calls to __open_2.
// Exercise the same no-follow guard/injection even under _FORTIFY_SOURCE=3.
extern "C" int __wrap___open_2(const char* path, int flags) {
    assert(!(flags & O_CREAT));
    return __wrap_open(path, flags);
}
extern "C" int __wrap_fstat(int descriptor, struct stat* info) {
    const std::string& path = descriptorPaths[descriptor];
    if ((mode == SOURCE_STAT_FAIL && path == localDirectory + "/apollo.pkg") ||
        (mode == GLOBAL_STAT_FAIL && path == systemDirectory + "/apollo.pkg") ||
        (mode == COPY_STAT_FAIL && path == copyDirectory + "/apollo.pkg.part")) {
        errno = ENOSYS;
        return -1;
    }
    return __real_fstat(descriptor, info);
}
extern "C" ssize_t __wrap_write(int fd, const void* bytes, size_t size) {
    if (mode == COPY_ENOSPC && jailbroken) { errno = ENOSPC; return -1; }
    if (mode == COPY_CANCEL && jailbroken) {
        entered.store(true); while (block.load()) usleep(20);
    }
    // Partial writes are legal; the installer must loop until complete.
    return __real_write(fd, bytes, size > 123 ? 123 : size);
}
extern "C" int __wrap_fsync(int fd) {
    if (mode == COPY_FSYNC_FAIL && jailbroken) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
extern "C" int __wrap_close(int fd) {
    int flags = fcntl(fd, F_GETFL);
    std::string path = descriptorPaths[fd];
    int rc = __real_close(fd);
    descriptorPaths.erase(fd);
    if (mode == COPY_CLOSE_FAIL && jailbroken && (flags & O_ACCMODE) == O_WRONLY) {
        errno = EIO; return -1;
    }
    if (mode == COPY_REPLACED_PATH && jailbroken && (flags & O_ACCMODE) == O_WRONLY) {
        assert(path == copyDirectory + "/apollo.pkg.part");
        assert(__real_rename(path.c_str(), (path + ".old").c_str()) == 0);
        FILE* replacement = fopen(path.c_str(), "wb"); assert(replacement);
        unsigned char invalid[8192] = {};
        assert(fwrite(invalid, 1, sizeof(invalid), replacement) == sizeof(invalid));
        assert(fclose(replacement) == 0);
    }
    return rc;
}
extern "C" int __wrap_rename(const char* from, const char* to) {
    if (mode == COPY_RENAME_FAIL && jailbroken && strstr(from, ".part")) { errno = EIO; return -1; }
    return __real_rename(from, to);
}

static void reset(Mode next, bool alias = true) {
    assert(__atomic_load_n(&g_installBusy, __ATOMIC_ACQUIRE) == 0);
    mode = next;
    modules = moduleProbes = appInitCalls = appTermCalls = bgftInitCalls = bgftTermCalls = 0;
    registerCalls = startCalls = progressCalls = existsCalls = stopCalls = unregisterCalls = 0;
    jailbreakCalls = restoreCalls = titleCalls = 0;
    jailbroken = nativeStarted = false;
    registeredPath.clear();
    descriptorPaths.clear(); noFollowOpens = 0;
    block.store(next == BLOCK_PROGRESS || next == STOP_FAIL || next == UNREGISTER_FAIL || next == COPY_CANCEL);
    entered.store(false);
    // Each fault scenario emulates a fresh application process.
    __atomic_store_n(&g_installUnsafe, 0, __ATOMIC_RELEASE);
    char rootTemplate[] = "/tmp/peppy-install-XXXXXX";
    char* directory = mkdtemp(rootTemplate); assert(directory);
    testRoot = directory;
    localDirectory = testRoot + "/local";
    systemRoot = testRoot + "/system";
    systemDirectory = systemRoot + "/downloads";
    copyDirectory = systemRoot + "/install";
    assert(mkdir(localDirectory.c_str(), 0755) == 0);
    assert(mkdir(systemRoot.c_str(), 0755) == 0);
    assert(mkdir(systemDirectory.c_str(), 0755) == 0);
    unsigned char header[8192] = {};
    header[0] = 0x7f; header[1] = 'C'; header[2] = 'N'; header[3] = 'T';
    const char* id = "UP0001-APOL00004_00-0000000000000000";
    assert(strlen(id) == 36); memcpy(header + 0x40, id, 36);
    FILE* file = fopen((localDirectory + "/apollo.pkg").c_str(), "wb"); assert(file);
    assert(fwrite(header, 1, sizeof(header), file) == sizeof(header)); assert(fclose(file) == 0);
    if (alias) assert(link((localDirectory + "/apollo.pkg").c_str(),
                          (systemDirectory + "/apollo.pkg").c_str()) == 0);
    if (next == ALIAS_DIFFERENT_INODE) {
        file = fopen((systemDirectory + "/apollo.pkg").c_str(), "wb"); assert(file);
        assert(fwrite(header, 1, sizeof(header), file) == sizeof(header)); assert(fclose(file) == 0);
    }
    if (next == ALIAS_SYMLINK)
        assert(symlink((localDirectory + "/apollo.pkg").c_str(), (systemDirectory + "/apollo.pkg").c_str()) == 0);
    if (next == COPY_DIRECTORY_SYMLINK)
        assert(symlink(localDirectory.c_str(), copyDirectory.c_str()) == 0);
    if (next == BAD_GLOBAL_ROOT) {
        assert(rmdir(systemDirectory.c_str()) == 0 && rmdir(systemRoot.c_str()) == 0);
        file = fopen(systemRoot.c_str(), "wb"); assert(file); assert(fclose(file) == 0);
    }
}
static InstallSpec spec() { InstallSpec value = { "apollo.pkg", "Apollo Save Tool", 8192 }; return value; }
static InstallSnapshot waitDone() {
    for (int i = 0; i < 200000; ++i) {
        if (!__atomic_load_n(&g_installBusy, __ATOMIC_ACQUIRE)) return installSnapshot();
        usleep(20);
    }
    assert(!"installer did not complete"); return installSnapshot();
}
static void waitEntered() {
    for (int i = 0; i < 100000; ++i) { if (entered.load()) return; usleep(20); }
    assert(!"mock did not enter blocking stage");
}
static InstallSnapshot run(Mode next) {
    reset(next, !needsCopy(next));
    assert(startInstall(spec())); return waitDone();
}
static void failed(Mode next, int category, int stageExpected, int32_t native) {
    InstallSnapshot value = run(next);
    if (value.state != INSTALL_FAILED || value.errorCode != category ||
        value.stage != stageExpected || value.nativeCode != native)
        fprintf(stderr, "mode=%d state=%d category=%d stage=%d native=%d (expected %d/%d/%d)\n",
                next, value.state, value.errorCode, value.stage, value.nativeCode,
                category, stageExpected, native);
    assert(value.state == INSTALL_FAILED && value.errorCode == category);
    assert(value.stage == stageExpected && value.nativeCode == native);
    assert(restoreCalls == (jailbreakCalls && next != JAILBREAK_FAIL ? 1 : 0));
    if (next != RESTORE_FAIL) assert(!jailbroken);
    assert(access((localDirectory + "/apollo.pkg").c_str(), F_OK) == 0);
}
int main() {
    struct stat unavailable;
    assert(lstat("/tmp", &unavailable) == -1 && errno == ENOSYS);
    assert(unavailableLstatCalls == 1); unavailableLstatCalls = 0;
    InstallSnapshot value = run(NORMAL);
    assert(value.state == INSTALL_DONE && value.percent == 100 && value.errorCode == 0);
    assert(value.received == NATIVE_LENGTH && value.total == NATIVE_LENGTH);
    assert(value.progressBits == 0x10203040 && value.localCopyPercent == 100);
    assert(jailbreakCalls == 1 && restoreCalls == 1 && !jailbroken);
    assert(registerCalls == 1 && progressCalls == 3 && stopCalls == 0 && unregisterCalls == 0);
    assert(appInitCalls == 1 && appTermCalls == 1 && bgftInitCalls == 1 && bgftTermCalls == 1);
    // The mock system makes another app available to install. The same Peppy
    // process must reinitialize services only after clean prior shutdown.
    nativeStarted = false; progressCalls = existsCalls = 0;
    assert(startInstall(spec())); value = waitDone(); assert(value.state == INSTALL_DONE);
    value = run(DELAY_CONFIRM); assert(value.state == INSTALL_DONE && progressCalls == 5);
    failed(NO_SDK, INSTALL_ERROR_SDK, INSTALL_STAGE_SDK, -1);
    failed(JAILBREAK_FAIL, INSTALL_ERROR_JAILBREAK, INSTALL_STAGE_JAILBREAK, RAW);
    failed(RESTORE_FAIL, INSTALL_ERROR_RESTORE, INSTALL_STAGE_RESTORE, RAW);
    uint32_t previousGeneration = installSnapshot().generation;
    assert(!startInstall(spec()) && installSnapshot().generation == previousGeneration);
    failed(MODULE_FAIL, INSTALL_ERROR_MODULE, INSTALL_STAGE_MODULE_APP, RAW);
    value = run(MODULE_LOADED); assert(value.state == INSTALL_DONE && modules == 0);
    value = run(LOAD_FAIL_BUT_PRESENT); assert(value.state == INSTALL_DONE && modules == 2);
    failed(APP_INIT_FAIL, INSTALL_ERROR_SERVICE, INSTALL_STAGE_APP_INIT, RAW);
    failed(TITLE_FAIL, INSTALL_ERROR_PACKAGE, INSTALL_STAGE_TITLE, RAW);
    failed(BAD_TITLE, INSTALL_ERROR_PACKAGE, INSTALL_STAGE_TITLE, EINVAL);
    failed(NON_APP, INSTALL_ERROR_PACKAGE, INSTALL_STAGE_TITLE, EINVAL);
    failed(SELF_APP, INSTALL_ERROR_SELF, INSTALL_STAGE_TITLE, 0);
    failed(EXISTS_FAIL, INSTALL_ERROR_SERVICE, INSTALL_STAGE_EXISTS, RAW);
    failed(APP_EXISTS, INSTALL_ERROR_ALREADY_INSTALLED, INSTALL_STAGE_EXISTS, 0);
    assert(!registerCalls && !bgftInitCalls && !stopCalls);
    failed(BGFT_INIT_FAIL, INSTALL_ERROR_SERVICE, INSTALL_STAGE_BGFT_INIT, RAW);
    failed(REGISTER_FAIL, INSTALL_ERROR_TASK, INSTALL_STAGE_REGISTER, RAW);
    failed(REGISTER_EXISTS, INSTALL_ERROR_ALREADY_INSTALLED, INSTALL_STAGE_REGISTER, (int32_t)0x80990088U);
    assert(stopCalls == 0 && unregisterCalls == 0 && installSnapshot().taskId == -1);
    failed(REGISTER_BUSY, INSTALL_ERROR_BUSY, INSTALL_STAGE_REGISTER, (int32_t)0x80990086U);
    failed(START_FAIL, INSTALL_ERROR_TASK, INSTALL_STAGE_START, RAW);
    assert(stopCalls == 1 && unregisterCalls == 1);
    failed(PROGRESS_API_FAIL, INSTALL_ERROR_PROGRESS, INSTALL_STAGE_PROGRESS, RAW);
    failed(PROGRESS_RESULT_FAIL, INSTALL_ERROR_PROGRESS, INSTALL_STAGE_PROGRESS, RAW);
    failed(OVERFLOW_PROGRESS, INSTALL_ERROR_PROGRESS, INSTALL_STAGE_PROGRESS, EINVAL);
    failed(ZERO_PROGRESS, INSTALL_ERROR_TIMEOUT, INSTALL_STAGE_PROGRESS, 0);
    failed(CONFIRM_FAIL, INSTALL_ERROR_SERVICE, INSTALL_STAGE_CONFIRM, RAW);
    failed(BGFT_TERM_FAIL, INSTALL_ERROR_CLEANUP, INSTALL_STAGE_BGFT_TERM, RAW);
    assert(!startInstall(spec()));
    failed(APP_TERM_FAIL, INSTALL_ERROR_CLEANUP, INSTALL_STAGE_APP_TERM, RAW);
    assert(!startInstall(spec()));
    for (Mode next : { ATTR_FAIL, DETACH_FAIL, CREATE_FAIL }) {
        reset(next); previousGeneration = installSnapshot().generation;
        assert(!startInstall(spec())); value = installSnapshot();
        assert(value.generation == previousGeneration + 1);
        assert(value.state == INSTALL_FAILED && value.errorCode == INSTALL_ERROR_THREAD && value.nativeCode == RAW);
        assert(!jailbreakCalls && !modules);
    }
    for (Mode next : { BLOCK_PROGRESS, STOP_FAIL, UNREGISTER_FAIL }) {
        reset(next); assert(startInstall(spec())); waitEntered();
        previousGeneration = installSnapshot().generation;
        assert(!startInstall(spec()) && installSnapshot().generation == previousGeneration);
        cancelInstall();
        assert(installSnapshot().state == INSTALL_RUNNING);
        block.store(false); value = waitDone();
        assert(stopCalls == 1 && unregisterCalls == 1 && restoreCalls == 1);
        if (next == BLOCK_PROGRESS) assert(value.state == INSTALL_CANCELLED);
        else {
            assert(value.state == INSTALL_FAILED && value.errorCode == INSTALL_ERROR_CLEANUP && value.cleanupCode == RAW);
            assert(!startInstall(spec()));
        }
    }
    value = run(ALIAS_COPY); assert(value.state == INSTALL_DONE);
    assert(registeredPath == copyDirectory + "/apollo.pkg");
    assert(access((copyDirectory + "/apollo.pkg.part").c_str(), F_OK) != 0);
    value = run(ALIAS_DIFFERENT_INODE); assert(value.state == INSTALL_DONE);
    assert(registeredPath == copyDirectory + "/apollo.pkg");
    value = run(ALIAS_SYMLINK); assert(value.state == INSTALL_DONE);
    assert(registeredPath == copyDirectory + "/apollo.pkg");
    failed(SOURCE_STAT_FAIL, INSTALL_ERROR_FILE, INSTALL_STAGE_FILE, ENOSYS);
    assert(!jailbreakCalls && !registerCalls);
    failed(GLOBAL_STAT_FAIL, INSTALL_ERROR_GLOBAL_PATH, INSTALL_STAGE_GLOBAL_PATH, ENOSYS);
    assert(!registerCalls && access(copyDirectory.c_str(), F_OK) != 0);
    failed(COPY_STAT_FAIL, INSTALL_ERROR_COPY, INSTALL_STAGE_COPY, ENOSYS);
    assert(!registerCalls && access((copyDirectory + "/apollo.pkg.part").c_str(), F_OK) != 0);
    failed(COPY_REPLACED_PATH, INSTALL_ERROR_COPY, INSTALL_STAGE_COPY, EINVAL);
    assert(!registerCalls);
    value = run(COPY_DIRECTORY_SYMLINK);
    assert(value.state == INSTALL_FAILED && value.errorCode == INSTALL_ERROR_GLOBAL_PATH &&
           (value.nativeCode == ELOOP || value.nativeCode == ENOTDIR));
    assert(!registerCalls && access((localDirectory + "/apollo.pkg.part").c_str(), F_OK) != 0);
    failed(COPY_ENOSPC, INSTALL_ERROR_COPY, INSTALL_STAGE_COPY, ENOSPC);
    failed(COPY_FSYNC_FAIL, INSTALL_ERROR_COPY, INSTALL_STAGE_COPY, EIO);
    failed(COPY_CLOSE_FAIL, INSTALL_ERROR_COPY, INSTALL_STAGE_COPY, EIO);
    failed(COPY_RENAME_FAIL, INSTALL_ERROR_COPY, INSTALL_STAGE_COPY, EIO);
    assert(access((copyDirectory + "/apollo.pkg.part").c_str(), F_OK) != 0 && !registerCalls);
    failed(BAD_GLOBAL_ROOT, INSTALL_ERROR_GLOBAL_PATH, INSTALL_STAGE_GLOBAL_PATH, ENOTDIR);
    reset(COPY_CANCEL, false); assert(startInstall(spec())); waitEntered();
    cancelInstall(); block.store(false); value = waitDone();
    assert(value.state == INSTALL_CANCELLED && !registerCalls && restoreCalls == 1);
    assert(access((copyDirectory + "/apollo.pkg.part").c_str(), F_OK) != 0);
    reset(NORMAL);
    for (const char* bad : { "../evil.pkg", "apollo.pkg.part", "/apollo.pkg", "a..pkg", "x;evil.pkg" }) {
        InstallSpec invalid = { bad, "Apollo", 8192 };
        previousGeneration = installSnapshot().generation;
        assert(!startInstall(invalid) && installSnapshot().errorCode == INSTALL_ERROR_SPEC);
        assert(installSnapshot().generation == previousGeneration + 1);
    }
    InstallSpec invalid = { "apollo.pkg", "Apollo", 0 };
    assert(!startInstall(invalid));
    FILE* file = fopen((localDirectory + "/apollo.pkg").c_str(), "r+b"); assert(file);
    assert(fputc('X', file) == 'X' && fclose(file) == 0);
    assert(startInstall(spec())); value = waitDone();
    assert(value.state == INSTALL_FAILED && value.errorCode == INSTALL_ERROR_PACKAGE && !jailbreakCalls);
    assert(unlink((localDirectory + "/apollo.pkg").c_str()) == 0);
    assert(symlink((systemDirectory + "/apollo.pkg").c_str(), (localDirectory + "/apollo.pkg").c_str()) == 0);
    assert(startInstall(spec())); value = waitDone();
    assert(value.state == INSTALL_FAILED && value.errorCode == INSTALL_ERROR_FILE && !jailbreakCalls);
    // O_NONBLOCK prevents an unexpected FIFO from hanging before type checks.
    assert(unlink((localDirectory + "/apollo.pkg").c_str()) == 0);
    assert(mkfifo((localDirectory + "/apollo.pkg").c_str(), 0600) == 0);
    assert(startInstall(spec())); value = waitDone();
    assert(value.state == INSTALL_FAILED && value.errorCode == INSTALL_ERROR_FILE &&
           value.nativeCode == EINVAL && !jailbreakCalls);
    reset(NORMAL);
    InstallSpec wrongSize = spec(); --wrongSize.expectedBytes;
    assert(startInstall(wrongSize)); value = waitDone();
    assert(value.state == INSTALL_FAILED && value.errorCode == INSTALL_ERROR_PACKAGE && !jailbreakCalls);
    // The source directory itself must also be a real directory, not a symlink.
    assert(rename(localDirectory.c_str(), (localDirectory + "-real").c_str()) == 0);
    assert(symlink((localDirectory + "-real").c_str(), localDirectory.c_str()) == 0);
    assert(startInstall(spec())); value = waitDone();
    assert(value.state == INSTALL_FAILED && value.errorCode == INSTALL_ERROR_FILE &&
           (value.nativeCode == ELOOP || value.nativeCode == ENOTDIR) && !jailbreakCalls);
    assert(noFollowOpens > 0 && unavailableLstatCalls == 0);
    puts("All native installer ABI, privileges, local path/copy, progress, preservation, errors and cancellation tests passed.");
}
