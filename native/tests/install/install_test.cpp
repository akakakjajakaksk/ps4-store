#include <assert.h>
#include <atomic>
#include <stdarg.h>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

static std::string testRoot, localDirectory, systemDirectory, copyDirectory, systemRoot;
#define PEPPY_DOWNLOAD_DIRECTORY localDirectory.c_str()
#define PEPPY_INSTALL_SYSTEM_DIRECTORY systemDirectory.c_str()
#define PEPPY_INSTALL_COPY_DIRECTORY copyDirectory.c_str()
#define PEPPY_INSTALL_ROOT_DIRECTORY systemRoot.c_str()
#define PEPPY_PKG_SERVER_HEADER "tests/install/stubs/pkg_server.h"
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
    ALIAS_DIFFERENT_INODE, ALIAS_SYMLINK, COPY_DIRECTORY_SYMLINK,
    USER_FAIL, USER_INVALID, SERVER_START_FAIL, SERVER_TRANSFER_FAIL, SERVER_STOP_FAIL,
    SERVER_EARLY_FAIL, SERVER_LATE_FAIL, SERVER_URL_INVALID, SERVER_URL_NULL,
    HTTP_PATH_REPLACED, HTTP_BGFT_NOT_FOUND, HTTP_BGFT_NOT_FOUND_API, HTTP_REGISTER_BLOCK
};
static Mode mode;
static int modules, moduleProbes, appInitCalls, appTermCalls, bgftInitCalls, bgftTermCalls;
static int threadCreateCalls;
static int registerCalls, startCalls, progressCalls, existsCalls, stopCalls, unregisterCalls;
static int jailbreakCalls, restoreCalls, titleCalls, sdkCalls, foregroundUserCalls;
static int storageRegisterCalls, httpRegisterCalls, serverStartCalls, serverStopCalls, sourceCloseCalls;
static int serverSnapshotCalls;
static PkgServerSnapshot serverTelemetry;
static uint64_t expectedPackageBytes = 8192, registeredPackageBytes, serverPackageBytes;
static bool jailbroken, nativeStarted;
static bool forceHttp, serverActive;
static int32_t forcedSdkVersion;
static int forcedSdkErrno;
static FILE* borrowedFile;
static int borrowedDescriptor = -1;
enum Event { TASK_STOP, TASK_UNREGISTER, SERVER_STOP, SOURCE_CLOSE, BGFT_TERM, APP_TERM };
static std::vector<Event> events;
static std::atomic<bool> block(false), entered(false);
static std::string registeredPath;
static std::map<int, std::string> descriptorPaths;
static int unavailableLstatCalls, unavailablePosixFstatCalls, nativeFstatCalls, noFollowOpens;
static int globalNamespaceOpens;
static const int32_t RAW = (int32_t)0x8099ee01U;
static const int32_t RAW_ENOSYS = (int32_t)0x8002004eU;
static const uint64_t NATIVE_LENGTH = 5ULL * 1024 * 1024 * 1024;
static const int32_t BGFT_NOT_FOUND = (int32_t)0x80991404U;
static const uint64_t HTTP_WIDE_BYTES = (1ULL << 32) + 71;
static const uint64_t HTTP_PROGRESS_BYTES = 37, HTTP_STOP_BYTES = 251;
static void request(int status, uint64_t sentBytes) {
    ++serverTelemetry.requests;
    serverTelemetry.status = status;
    serverTelemetry.sentBytes += sentBytes;
}
static bool httpFallback() { return forceHttp || mode == NO_SDK; }
static void credentialsExpected() { assert(jailbroken == !httpFallback()); }
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
    ++threadCreateCalls;
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
    credentialsExpected();
    assert(id == ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL || id == ORBIS_SYSMODULE_INTERNAL_BGFT);
    ++modules;
    return mode == MODULE_FAIL || mode == LOAD_FAIL_BUT_PRESENT ? (uint32_t)RAW : 0;
}
extern "C" int32_t peppyInstallSdkVersion() {
    ++sdkCalls;
    assert(errno == 0); // Stale errno must not become the SDK diagnostic.
    errno = forceHttp ? forcedSdkErrno : mode == NO_SDK ? 78 : 0;
    return forceHttp ? forcedSdkVersion : mode == NO_SDK ? -1 : 0x100;
}
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
    credentialsExpected(); ++appInitCalls; return mode == APP_INIT_FAIL ? RAW : 0;
}
extern "C" int32_t sceAppInstUtilTerminate() {
    credentialsExpected(); ++appTermCalls; events.push_back(APP_TERM);
    return mode == APP_TERM_FAIL ? RAW : 0;
}
extern "C" int32_t sceAppInstUtilGetTitleIdFromPkg(const char* path, char* title, int32_t* isApp) {
    credentialsExpected(); ++titleCalls;
    if (httpFallback()) assert(std::string(path) == localDirectory + "/apollo.pkg");
    else assert(std::string(path).find(systemRoot) == 0);
    FILE* file = fopen(path, "rb");
    assert(file); unsigned char magic[4]; assert(fread(magic, 1, 4, file) == 4);
    assert(fclose(file) == 0 && magic[0] == 0x7f && magic[1] == 'C');
    if (mode == HTTP_PATH_REPLACED) {
        assert(httpFallback());
        assert(rename(path, (std::string(path) + ".old").c_str()) == 0);
        file = fopen(path, "wb"); assert(file);
        unsigned char replacement[8192] = {};
        assert(fwrite(replacement, 1, sizeof(replacement), file) == sizeof(replacement));
        assert(fclose(file) == 0);
    }
    if (mode == TITLE_FAIL) return RAW;
    strcpy(title, mode == BAD_TITLE ? "invalid" : mode == SELF_APP ? "BREW00001" : "APOL00004");
    *isApp = mode == NON_APP ? 0 : 1;
    return 0;
}
extern "C" int32_t sceAppInstUtilAppExists(const char* title, int32_t* exists) {
    credentialsExpected(); assert(!strcmp(title, "APOL00004"));
    ++existsCalls;
    if (mode == EXISTS_FAIL || (mode == CONFIRM_FAIL && existsCalls > 1)) return RAW;
    *exists = mode == APP_EXISTS || (nativeStarted && progressCalls >= 3) ? 1 : 0;
    return 0;
}
extern "C" bool sceAppInstUtilAppIsInInstalling(const char* id) {
    credentialsExpected(); assert(!strcmp(id, "UP0001-APOL00004_00-0000000000000000"));
    return mode == DELAY_CONFIRM && progressCalls < 5;
}
extern "C" int32_t peppyBgftInit(PeppyBgftInit* init) {
    credentialsExpected(); assert(init->heap && init->heapSize == 1024 * 1024);
    ++bgftInitCalls;
    return mode == BGFT_INIT_FAIL ? RAW : 0;
}
extern "C" int32_t peppyBgftRegister(PeppyBgftParamEx* params, int32_t* task) {
    assert(jailbroken && params->slot == 0 && params->params.entitlementType == 5);
    assert(params->params.packageSize == expectedPackageBytes && params->params.option == 2);
    registeredPackageBytes = params->params.packageSize;
    assert(!strcmp(params->params.id, "UP0001-APOL00004_00-0000000000000000"));
    assert(!strcmp(params->params.playgoScenarioId, "0"));
    assert(!strstr(params->params.contentUrl, "://"));
    registeredPath = params->params.contentUrl;
    ++registerCalls; ++storageRegisterCalls;
    if (mode == REGISTER_FAIL) return RAW;
    if (mode == REGISTER_EXISTS) { *task = 42; return (int32_t)0x80990088U; }
    if (mode == REGISTER_BUSY) return (int32_t)0x80990086U;
    assert(registeredPath == (needsCopy(mode) ? copyDirectory : systemDirectory) + "/apollo.pkg");
    *task = 17;
    return 0;
}
extern "C" int32_t peppyInstallForegroundUser(int32_t* user) {
    assert(httpFallback() && !jailbroken);
    ++foregroundUserCalls;
    if (mode == USER_FAIL) return RAW;
    *user = mode == USER_INVALID ? -1 : 99;
    return 0;
}
extern "C" int32_t peppyBgftRegisterHttp(PeppyBgftParam* params, int32_t* task) {
    assert(httpFallback() && !jailbroken && serverActive && borrowedFile);
    assert(params->userId == 99 && params->entitlementType == 5);
    assert(params->packageSize == expectedPackageBytes && params->option == 0x10002);
    registeredPackageBytes = params->packageSize;
    assert(!strcmp(params->id, "UP0001-APOL00004_00-0000000000000000"));
    assert(!strcmp(params->contentUrl, "http://127.0.0.1:17991/package.pkg"));
    assert(!strcmp(params->contentName, "Apollo Save Tool"));
    assert(params->iconPath && !strcmp(params->iconPath, ""));
    assert(params->packageType && !strcmp(params->packageType, "PS4GD"));
    assert(params->packageSubType && !strcmp(params->packageSubType, ""));
    assert(params->playgoScenarioId && !strcmp(params->playgoScenarioId, "0"));
    registeredPath = params->contentUrl;
    ++registerCalls; ++httpRegisterCalls;
    if (mode == HTTP_REGISTER_BLOCK) {
        entered.store(true);
        while (block.load()) usleep(20);
    }
    if (mode == REGISTER_FAIL) request(404, 113);
    else {
        request(200, 0); // A metadata request precedes the body request.
        request(206, HTTP_WIDE_BYTES);
    }
    if (mode == REGISTER_FAIL) { *task = 42; return RAW; }
    if (mode == REGISTER_EXISTS) { *task = 42; return (int32_t)0x80990088U; }
    if (mode == REGISTER_BUSY) { *task = 42; return (int32_t)0x80990086U; }
    *task = 17;
    return 0;
}
extern "C" int32_t peppyBgftRegisterDebug(PeppyBgftParam* params, int32_t* task) {
    return peppyBgftRegisterHttp(params, task);
}
extern "C" int32_t sceBgftServiceDownloadStartTask(int32_t task) {
    credentialsExpected(); assert(task == 17); ++startCalls;
    if (mode == START_FAIL) return RAW;
    nativeStarted = true;
    return 0;
}
extern "C" int32_t peppyBgftProgress(int32_t task, PeppyBgftProgress* progress) {
    credentialsExpected(); assert(task == 17); ++progressCalls;
    if (httpFallback()) {
        assert(serverActive && borrowedFile);
        bool missing = mode == HTTP_BGFT_NOT_FOUND || mode == HTTP_BGFT_NOT_FOUND_API;
        request(missing ? 404 : 206, missing ? 0 : HTTP_PROGRESS_BYTES);
    }
    if (mode == BLOCK_PROGRESS || mode == STOP_FAIL || mode == UNREGISTER_FAIL) {
        entered.store(true);
        while (block.load()) usleep(20);
    }
    if (mode == PROGRESS_API_FAIL) return RAW;
    if (mode == HTTP_BGFT_NOT_FOUND_API) return BGFT_NOT_FOUND;
    progress->errorResult = mode == PROGRESS_RESULT_FAIL ? RAW : mode == HTTP_BGFT_NOT_FOUND ? BGFT_NOT_FOUND : 0;
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
    credentialsExpected(); assert(task == 17);
    if (httpFallback()) assert(serverActive && borrowedFile);
    ++stopCalls; events.push_back(TASK_STOP); return mode == STOP_FAIL ? RAW : 0;
}
extern "C" int32_t sceBgftServiceIntDownloadUnregisterTask(int32_t task) {
    credentialsExpected(); assert(task == 17);
    if (httpFallback()) assert(serverActive && borrowedFile);
    ++unregisterCalls; events.push_back(TASK_UNREGISTER);
    return mode == UNREGISTER_FAIL ? RAW : 0;
}
extern "C" int32_t sceBgftServiceIntTerm() {
    credentialsExpected(); ++bgftTermCalls; events.push_back(BGFT_TERM);
    return mode == BGFT_TERM_FAIL ? RAW : 0;
}
PkgServer::PkgServer() : file_(0), descriptor_(-1), active_(false) {
    serverTelemetry = PkgServerSnapshot();
}
PkgServer::~PkgServer() { assert(!active_ && !file_); }
int32_t PkgServer::start(FILE* file, uint64_t size) {
    assert(httpFallback() && !active_ && file && size == expectedPackageBytes);
    serverPackageBytes = size;
    assert(nativeFstatCalls > 0);
    ++serverStartCalls;
    file_ = borrowedFile = file;
    descriptor_ = borrowedDescriptor = fileno(file);
    assert(descriptorPaths[descriptor_] == localDirectory + "/apollo.pkg");
    unsigned char magic[4];
    assert(pread(descriptor_, magic, sizeof(magic), 0) == (ssize_t)sizeof(magic));
    assert(magic[0] == 0x7f && magic[1] == 'C' && magic[2] == 'N' && magic[3] == 'T');
    if (mode == SERVER_START_FAIL) return RAW;
    active_ = serverActive = true;
    return 0;
}
const char* PkgServer::url() const {
    if (mode == SERVER_URL_INVALID) return "http://example.org/package.pkg";
    if (mode == SERVER_URL_NULL) return 0;
    return "http://127.0.0.1:17991/package.pkg";
}
int32_t PkgServer::errorCode() const {
    if (active_) {
        assert(file_ == borrowedFile && fileno(file_) == descriptor_);
        assert(fcntl(descriptor_, F_GETFD) >= 0);
    }
    if (mode == SERVER_EARLY_FAIL && active_) return RAW;
    if (mode == SERVER_TRANSFER_FAIL && nativeStarted && progressCalls >= 1) return RAW;
    if (mode == SERVER_LATE_FAIL && nativeStarted && progressCalls >= 3) return RAW;
    return 0;
}
PkgServerSnapshot PkgServer::snapshot() const {
    ++serverSnapshotCalls;
    if (file_) assert(fileno(file_) == descriptor_ && fcntl(descriptor_, F_GETFD) >= 0);
    // HTTP error responses are diagnostics; they do not imply a server/socket
    // failure. In particular, a 404 must not replace BGFT's own error result.
    return serverTelemetry;
}
int32_t PkgServer::stop() {
    if (!file_) return 0;
    assert(file_ == borrowedFile && fileno(file_) == descriptor_);
    assert(fcntl(descriptor_, F_GETFD) >= 0);
    // stop() guarantees quiescence even when reporting a shutdown error.
    ++serverStopCalls; events.push_back(SERVER_STOP);
    if (serverTelemetry.requests) {
        // An in-flight request finishes while stop waits for quiescence. A
        // cleanup snapshot taken only before stop would lose these counters.
        request(serverTelemetry.status, HTTP_STOP_BYTES);
    }
    active_ = serverActive = false;
    file_ = 0; descriptor_ = -1;
    return mode == SERVER_STOP_FAIL ? RAW : 0;
}
extern "C" ssize_t __real_write(int, const void*, size_t);
extern "C" int __real_fsync(int);
extern "C" int __real_close(int);
extern "C" int __real_fclose(FILE*);
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
    if (!systemRoot.empty() && std::string(path).find(systemRoot) == 0) ++globalNamespaceOpens;
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
extern "C" int __wrap_fstat(int, struct stat*) {
    // POSIX metadata has an incompatible ABI in the packaged PS4 SDK.
    // Every production metadata check must use sceKernelFstat instead.
    ++unavailablePosixFstatCalls;
    errno = ENOSYS;
    return -1;
}
template<typename T> static void nativeField(unsigned char* bytes, size_t offset, T value) {
    assert(offset + sizeof(value) <= 120);
    memcpy(bytes + offset, &value, sizeof(value));
}
static void encodeNativeStat(void* nativeInfo, const struct stat& host) {
    // Offsets are independent of PeppyFileInfo and the host's struct stat.
    // PS4 is little-endian x86-64. Write precisely the kernel's 120 bytes.
    unsigned char* bytes = static_cast<unsigned char*>(nativeInfo);
    memset(bytes, 0, 120);
    nativeField<uint32_t>(bytes, 0, (uint32_t)host.st_dev);
    nativeField<uint32_t>(bytes, 4, (uint32_t)host.st_ino);
    nativeField<uint16_t>(bytes, 8, (uint16_t)host.st_mode);
    nativeField<uint16_t>(bytes, 10, (uint16_t)host.st_nlink);
    nativeField<uint32_t>(bytes, 12, (uint32_t)host.st_uid);
    nativeField<uint32_t>(bytes, 16, (uint32_t)host.st_gid);
    nativeField<uint32_t>(bytes, 20, (uint32_t)host.st_rdev);
    nativeField<int64_t>(bytes, 24, host.st_atim.tv_sec);
    nativeField<int64_t>(bytes, 32, host.st_atim.tv_nsec);
    nativeField<int64_t>(bytes, 40, host.st_mtim.tv_sec);
    nativeField<int64_t>(bytes, 48, host.st_mtim.tv_nsec);
    nativeField<int64_t>(bytes, 56, host.st_ctim.tv_sec);
    nativeField<int64_t>(bytes, 64, host.st_ctim.tv_nsec);
    nativeField<int64_t>(bytes, 72, host.st_size);
    nativeField<int64_t>(bytes, 80, host.st_blocks);
    nativeField<int32_t>(bytes, 88, (int32_t)host.st_blksize);
    nativeField<uint32_t>(bytes, 92, 0x12345678U);
    nativeField<uint32_t>(bytes, 96, 0x23456789U);
    nativeField<int32_t>(bytes, 100, 0x34567890);
    nativeField<int64_t>(bytes, 104, 0x0123456789abcdefLL);
    nativeField<int64_t>(bytes, 112, 987654321);
}
extern "C" int32_t sceKernelFstat(int32_t descriptor, void* nativeInfo) {
    ++nativeFstatCalls;
    assert(nativeInfo);
    const std::string& path = descriptorPaths[descriptor];
    if ((mode == SOURCE_STAT_FAIL && path == localDirectory + "/apollo.pkg") ||
        (mode == GLOBAL_STAT_FAIL && path == systemDirectory + "/apollo.pkg") ||
        (mode == COPY_STAT_FAIL && path == copyDirectory + "/apollo.pkg.part")) {
        // Unlike the libc wrapper, the native export returns the SCE code.
        // Leave errno stale to catch accidental native-error conversion.
        errno = EINVAL;
        return RAW_ENOSYS;
    }
    struct stat host;
    if (__real_fstat(descriptor, &host))
        return (int32_t)(0x80020000U | (uint32_t)errno);
    encodeNativeStat(nativeInfo, host);
    return 0;
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
    if (fd == borrowedDescriptor) assert(!serverActive);
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
extern "C" int __wrap_fclose(FILE* file) {
    if (file == borrowedFile) {
        assert(!serverActive);
        assert(fileno(file) == borrowedDescriptor && fcntl(borrowedDescriptor, F_GETFD) >= 0);
        ++sourceCloseCalls; events.push_back(SOURCE_CLOSE);
        descriptorPaths.erase(borrowedDescriptor);
        borrowedFile = 0; borrowedDescriptor = -1;
    }
    return __real_fclose(file);
}
extern "C" int __wrap_rename(const char* from, const char* to) {
    if (mode == COPY_RENAME_FAIL && jailbroken && strstr(from, ".part")) { errno = EIO; return -1; }
    return __real_rename(from, to);
}

static void reset(Mode next, bool alias = true) {
    assert(__atomic_load_n(&g_installBusy, __ATOMIC_ACQUIRE) == 0);
    mode = next;
    modules = moduleProbes = appInitCalls = appTermCalls = bgftInitCalls = bgftTermCalls = 0;
    threadCreateCalls = 0;
    registerCalls = startCalls = progressCalls = existsCalls = stopCalls = unregisterCalls = 0;
    jailbreakCalls = restoreCalls = titleCalls = 0;
    sdkCalls = foregroundUserCalls = storageRegisterCalls = httpRegisterCalls = 0;
    serverStartCalls = serverStopCalls = sourceCloseCalls = 0;
    serverSnapshotCalls = 0; serverTelemetry = PkgServerSnapshot();
    expectedPackageBytes = 8192; registeredPackageBytes = serverPackageBytes = 0;
    jailbroken = nativeStarted = false;
    assert(!serverActive && !borrowedFile);
    forceHttp = false; forcedSdkVersion = -1; forcedSdkErrno = 78;
    borrowedDescriptor = -1; events.clear();
    registeredPath.clear();
    descriptorPaths.clear(); noFollowOpens = nativeFstatCalls = globalNamespaceOpens = 0;
    block.store(next == BLOCK_PROGRESS || next == STOP_FAIL || next == UNREGISTER_FAIL ||
                next == COPY_CANCEL || next == HTTP_REGISTER_BLOCK);
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
    header[0x77] = 0x1A; // PKG_CONTENT_TYPE_GD, big-endian.
    const char* id = next == SELF_APP ? "UP0001-BREW00001_00-0000000000000000"
                                      : "UP0001-APOL00004_00-0000000000000000";
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
static InstallSpec spec() {
    InstallSpec value = { "apollo.pkg", "Apollo Save Tool", expectedPackageBytes };
    return value;
}
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
static void noHttpTelemetry(const InstallSnapshot& value) {
    assert(value.httpRequests == 0 && value.httpStatus == 0 && value.httpBytes == 0);
}
static void httpTelemetry(const InstallSnapshot& value, uint32_t requests,
                          int32_t status, uint64_t bytes) {
    assert(value.httpRequests == requests && value.httpStatus == status && value.httpBytes == bytes);
}
static InstallSnapshot run(Mode next) {
    reset(next, !needsCopy(next));
    assert(startInstall(spec()));
    InstallSnapshot value = waitDone();
    if (!httpFallback()) {
        noHttpTelemetry(value);
        assert(serverSnapshotCalls == 0);
    }
    return value;
}
static void prepareHttp(Mode next, int32_t version = -1, int sdkError = 78) {
    reset(next, false);
    forceHttp = true; forcedSdkVersion = version; forcedSdkErrno = sdkError;
    errno = EINVAL;
}
static InstallSnapshot runHttp(Mode next, int32_t version = -1, int sdkError = 78) {
    prepareHttp(next, version, sdkError);
    assert(startInstall(spec()));
    InstallSnapshot value = waitDone();
    assert(value.mode == INSTALL_MODE_HTTP_LOCAL);
    assert(value.sdkVersion == (uint32_t)version && value.sdkErrno == sdkError);
    assert(sdkCalls == 1 && !jailbreakCalls && !restoreCalls && !jailbroken);
    assert(!storageRegisterCalls && !globalNamespaceOpens);
    assert(access(copyDirectory.c_str(), F_OK) != 0);
    assert(!serverActive && !borrowedFile);
    httpTelemetry(value, serverTelemetry.requests, serverTelemetry.status, serverTelemetry.sentBytes);
    assert(access((localDirectory + "/apollo.pkg").c_str(), F_OK) == 0);
    return value;
}
static size_t eventIndex(Event event) {
    for (size_t i = 0; i < events.size(); ++i) if (events[i] == event) return i;
    assert(!"expected lifecycle event absent"); return events.size();
}
static void serverClosedAfterStop() {
    assert(serverStartCalls == 1 && serverStopCalls == 1 && sourceCloseCalls == 1);
    assert(eventIndex(SERVER_STOP) < eventIndex(SOURCE_CLOSE));
}
static void failedHttp(Mode next, int category, int where, int32_t native) {
    InstallSnapshot value = runHttp(next);
    if (value.state != INSTALL_FAILED || value.errorCode != category ||
        value.stage != where || value.nativeCode != native)
        fprintf(stderr, "HTTP mode=%d state=%d category=%d stage=%d native=%d (expected %d/%d/%d)\n",
                next, value.state, value.errorCode, value.stage, value.nativeCode,
                category, where, native);
    assert(value.state == INSTALL_FAILED && value.errorCode == category);
    assert(value.stage == where && value.nativeCode == native);
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
static void httpFallbackTests() {
    InstallSnapshot value;
    for (int sdkError : { 78, 1, 0 }) {
        value = runHttp(NORMAL, -1, sdkError);
        assert(value.state == INSTALL_DONE && value.percent == 100 && value.errorCode == 0);
        assert(httpRegisterCalls == 1 && foregroundUserCalls == 1 && titleCalls == 0);
        assert(progressCalls == 3 && !stopCalls && !unregisterCalls);
        serverClosedAfterStop();
    }
    value = runHttp(NORMAL, 0x200, 0);
    assert(value.state == INSTALL_DONE && httpRegisterCalls == 1);
    serverClosedAfterStop();
    value = runHttp(DELAY_CONFIRM);
    assert(value.state == INSTALL_DONE && progressCalls == 5);
    serverClosedAfterStop();
    failedHttp(MODULE_FAIL, INSTALL_ERROR_MODULE, INSTALL_STAGE_MODULE_APP, RAW);
    assert(!appInitCalls && !serverStartCalls);
    failedHttp(APP_INIT_FAIL, INSTALL_ERROR_SERVICE, INSTALL_STAGE_APP_INIT, RAW);
    assert(!serverStartCalls && !registerCalls);
    failedHttp(BGFT_INIT_FAIL, INSTALL_ERROR_SERVICE, INSTALL_STAGE_BGFT_INIT, RAW);
    assert(!serverStartCalls && !registerCalls);
    failedHttp(USER_FAIL, INSTALL_ERROR_USER, INSTALL_STAGE_USER, RAW);
    assert(!bgftInitCalls && !serverStartCalls);
    failedHttp(USER_INVALID, INSTALL_ERROR_USER, INSTALL_STAGE_USER, EINVAL);
    assert(!bgftInitCalls && !serverStartCalls);
    // HTTP mode serves the already-open validated descriptor, so replacing the
    // sandbox path after validation must not affect installation.
    value = runHttp(HTTP_PATH_REPLACED);
    assert(value.state == INSTALL_DONE && value.errorCode == 0);
    assert(httpRegisterCalls == 1 && serverStartCalls == 1);
    serverClosedAfterStop();
    failedHttp(APP_EXISTS, INSTALL_ERROR_ALREADY_INSTALLED, INSTALL_STAGE_EXISTS, 0);
    assert(!foregroundUserCalls && !bgftInitCalls && !serverStartCalls && !registerCalls);
    failedHttp(SELF_APP, INSTALL_ERROR_SELF, INSTALL_STAGE_TITLE, 0);
    assert(!serverStartCalls && !registerCalls);
    failedHttp(SERVER_START_FAIL, INSTALL_ERROR_SERVER, INSTALL_STAGE_SERVER_START, RAW);
    serverClosedAfterStop(); assert(!registerCalls && !stopCalls && !unregisterCalls);
    failedHttp(SERVER_EARLY_FAIL, INSTALL_ERROR_SERVER, INSTALL_STAGE_SERVER_TRANSFER, RAW);
    serverClosedAfterStop(); assert(!registerCalls && !stopCalls && !unregisterCalls);
    for (Mode next : { SERVER_URL_INVALID, SERVER_URL_NULL }) {
        failedHttp(next, INSTALL_ERROR_SERVER, INSTALL_STAGE_SERVER_START, EINVAL);
        serverClosedAfterStop(); assert(!registerCalls && !stopCalls && !unregisterCalls);
    }
    failedHttp(REGISTER_FAIL, INSTALL_ERROR_TASK, INSTALL_STAGE_REGISTER, RAW);
    serverClosedAfterStop();
    assert(!stopCalls && !unregisterCalls && installSnapshot().taskId == -1);
    failedHttp(REGISTER_EXISTS, INSTALL_ERROR_ALREADY_INSTALLED, INSTALL_STAGE_REGISTER,
               (int32_t)0x80990088U);
    serverClosedAfterStop();
    assert(!stopCalls && !unregisterCalls && installSnapshot().taskId == -1);
    failedHttp(REGISTER_BUSY, INSTALL_ERROR_BUSY, INSTALL_STAGE_REGISTER, (int32_t)0x80990086U);
    serverClosedAfterStop(); assert(!stopCalls && !unregisterCalls);
    for (Mode next : { SERVER_TRANSFER_FAIL, SERVER_LATE_FAIL }) {
        failedHttp(next, INSTALL_ERROR_SERVER, INSTALL_STAGE_SERVER_TRANSFER, RAW);
        assert(installSnapshot().percent < 100 && stopCalls == 1 && unregisterCalls == 1);
        serverClosedAfterStop();
        assert(eventIndex(TASK_STOP) < eventIndex(TASK_UNREGISTER));
        assert(eventIndex(TASK_UNREGISTER) < eventIndex(SERVER_STOP));
    }
    failedHttp(START_FAIL, INSTALL_ERROR_TASK, INSTALL_STAGE_START, RAW);
    serverClosedAfterStop();
    assert(stopCalls == 1 && unregisterCalls == 1);
    assert(eventIndex(TASK_UNREGISTER) < eventIndex(SERVER_STOP));
    failedHttp(PROGRESS_API_FAIL, INSTALL_ERROR_PROGRESS, INSTALL_STAGE_PROGRESS, RAW);
    serverClosedAfterStop();
    failedHttp(PROGRESS_RESULT_FAIL, INSTALL_ERROR_PROGRESS, INSTALL_STAGE_PROGRESS, RAW);
    serverClosedAfterStop();
    failedHttp(CONFIRM_FAIL, INSTALL_ERROR_SERVICE, INSTALL_STAGE_CONFIRM, RAW);
    serverClosedAfterStop();
    failedHttp(SERVER_STOP_FAIL, INSTALL_ERROR_CLEANUP, INSTALL_STAGE_SERVER_STOP, RAW);
    serverClosedAfterStop();
    assert(installSnapshot().cleanupCode == RAW && !startInstall(spec()));
    for (Mode next : { BLOCK_PROGRESS, STOP_FAIL, UNREGISTER_FAIL }) {
        prepareHttp(next); assert(startInstall(spec())); waitEntered();
        uint32_t generation = installSnapshot().generation;
        assert(!startInstall(spec()) && installSnapshot().generation == generation);
        cancelInstall(); assert(installSnapshot().state == INSTALL_RUNNING);
        block.store(false); value = waitDone();
        assert(value.mode == INSTALL_MODE_HTTP_LOCAL && value.sdkVersion == 0xffffffffU && value.sdkErrno == 78);
        assert(!jailbreakCalls && !restoreCalls && !globalNamespaceOpens);
        assert(stopCalls == 1 && unregisterCalls == 1);
        serverClosedAfterStop();
        assert(eventIndex(TASK_STOP) < eventIndex(TASK_UNREGISTER));
        assert(eventIndex(TASK_UNREGISTER) < eventIndex(SERVER_STOP));
        if (next == BLOCK_PROGRESS) assert(value.state == INSTALL_CANCELLED);
        else {
            assert(value.state == INSTALL_FAILED && value.errorCode == INSTALL_ERROR_CLEANUP);
            assert(value.cleanupCode == RAW && !startInstall(spec()));
        }
    }
}
static void httpTelemetryTests() {
    InstallSnapshot value = runHttp(NORMAL);
    assert(value.state == INSTALL_DONE && value.percent == 100 && serverSnapshotCalls > 0);
    // Counters are cumulative HTTP traffic, separate from native BGFT
    // progress. Use bytes above UINT32_MAX to exercise the full public width.
    httpTelemetry(value, 6, 206, HTTP_WIDE_BYTES + 3 * HTTP_PROGRESS_BYTES + HTTP_STOP_BYTES);
    assert(value.httpBytes > UINT32_MAX && value.httpBytes != value.received);
    serverClosedAfterStop();

    failedHttp(REGISTER_FAIL, INSTALL_ERROR_TASK, INSTALL_STAGE_REGISTER, RAW);
    value = installSnapshot();
    httpTelemetry(value, 2, 404, 113 + HTTP_STOP_BYTES);
    assert(value.taskId == -1 && !stopCalls && !unregisterCalls);
    serverClosedAfterStop();

    for (Mode next : { HTTP_BGFT_NOT_FOUND, HTTP_BGFT_NOT_FOUND_API }) {
        failedHttp(next, INSTALL_ERROR_PROGRESS, INSTALL_STAGE_PROGRESS, BGFT_NOT_FOUND);
        value = installSnapshot();
        httpTelemetry(value, 4, 404, HTTP_WIDE_BYTES + HTTP_STOP_BYTES);
        assert(value.nativeCode == BGFT_NOT_FOUND && value.cleanupCode == 0);
        assert(value.percent < 100 && stopCalls == 1 && unregisterCalls == 1);
        serverClosedAfterStop();
        assert(eventIndex(TASK_UNREGISTER) < eventIndex(SERVER_STOP));
        // errorCode() remains zero for this HTTP 404. Both the API-return and
        // errorResult paths must retain the native failure alongside transport
        // diagnostics, instead of producing a generic server failure.
        FILE* log = fopen((localDirectory + "/install.log").c_str(), "r"); assert(log);
        std::string text;
        char line[1024];
        while (fgets(line, sizeof(line), log)) text += line;
        assert(!ferror(log) && fclose(log) == 0);
        size_t finalLineStart = text.rfind('\n', text.size() - 2);
        std::string finalLine = text.substr(finalLineStart == std::string::npos ? 0 : finalLineStart + 1);
        assert(finalLine.find("native=0x80991404") != std::string::npos);
        assert(finalLine.find("http_requests=4 http_status=404") != std::string::npos);
    }

    prepareHttp(BLOCK_PROGRESS); assert(startInstall(spec())); waitEntered();
    InstallSnapshot during = installSnapshot();
    httpTelemetry(during, 2, 206, HTTP_WIDE_BYTES);
    assert(during.state == INSTALL_RUNNING);
    assert(!startInstall(spec()));
    httpTelemetry(installSnapshot(), during.httpRequests, during.httpStatus, during.httpBytes);
    cancelInstall(); block.store(false); value = waitDone();
    assert(value.state == INSTALL_CANCELLED && value.errorCode == 0);
    httpTelemetry(value, 4, 206, HTTP_WIDE_BYTES + HTTP_PROGRESS_BYTES + HTTP_STOP_BYTES);
    assert(value.httpBytes > during.httpBytes && value.httpRequests > during.httpRequests);
    serverClosedAfterStop();
    assert(eventIndex(TASK_UNREGISTER) < eventIndex(SERVER_STOP));

    // Every accepted request clears stale telemetry, including failures that
    // occur before worker startup. A storage install never samples HTTP.
    value = run(NORMAL);
    assert(value.state == INSTALL_DONE && value.mode == INSTALL_MODE_STORAGE);
    noHttpTelemetry(value);
    assert(serverSnapshotCalls == 0);
    for (Mode next : { ATTR_FAIL, DETACH_FAIL, CREATE_FAIL }) {
        value = runHttp(NORMAL); assert(value.httpRequests > 0);
        reset(next);
        assert(!startInstall(spec())); value = installSnapshot();
        assert(value.state == INSTALL_FAILED && value.errorCode == INSTALL_ERROR_THREAD);
        noHttpTelemetry(value);
        assert(value.mode == INSTALL_MODE_NONE && !serverSnapshotCalls);
    }
    value = runHttp(NORMAL); assert(value.httpRequests > 0);
    reset(NORMAL);
    InstallSpec invalid = { "../evil.pkg", "Apollo", 8192 };
    assert(!startInstall(invalid)); value = installSnapshot();
    assert(value.state == INSTALL_FAILED && value.errorCode == INSTALL_ERROR_SPEC);
    noHttpTelemetry(value);
    assert(value.mode == INSTALL_MODE_NONE && !serverSnapshotCalls);

    // The loopback server can be running while BGFT has issued no requests.
    // Its public telemetry must be zero, rather than leftovers from a prior
    // installation or guessed HTTP success based on native progress.
    prepareHttp(HTTP_REGISTER_BLOCK); assert(startInstall(spec())); waitEntered();
    value = installSnapshot();
    assert(value.state == INSTALL_RUNNING && value.mode == INSTALL_MODE_HTTP_LOCAL && serverActive);
    noHttpTelemetry(value);
    assert(serverTelemetry.requests == 0 && serverSnapshotCalls > 0);
    block.store(false); value = waitDone();
    assert(value.state == INSTALL_DONE);
    httpTelemetry(value, 6, 206, HTTP_WIDE_BYTES + 3 * HTTP_PROGRESS_BYTES + HTTP_STOP_BYTES);
    serverClosedAfterStop();
}
static void largePackageTests() {
    // Explicit test boundaries are independent of the production limit so a
    // mistaken lower cap cannot make this test silently agree with it.
    const uint64_t acceptedCap = 256ULL * 1024 * 1024 * 1024;
    struct Case { uint64_t bytes; Mode fault; int error; int stage; };
    const Case cases[] = {
        { 5ULL * 1024 * 1024 * 1024 + 123, REGISTER_FAIL, INSTALL_ERROR_TASK, INSTALL_STAGE_REGISTER },
        { acceptedCap, PROGRESS_API_FAIL, INSTALL_ERROR_PROGRESS, INSTALL_STAGE_PROGRESS }
    };
    for (const Case& test : cases) {
        prepareHttp(test.fault);
        expectedPackageBytes = test.bytes;
        std::string path = localDirectory + "/apollo.pkg";
        FILE* file = fopen(path.c_str(), "r+b"); assert(file);
        // Keep the small valid PKG header, create a hole for the body, and
        // fail at a native boundary. No huge allocation, copy or transfer.
        assert(ftruncate(fileno(file), (off_t)test.bytes) == 0);
        struct stat host;
        assert(__real_fstat(fileno(file), &host) == 0);
        assert((uint64_t)host.st_size == test.bytes && host.st_blocks < 1024);
        assert(fclose(file) == 0);
        assert(startInstall(spec()));
        InstallSnapshot value = waitDone();
        assert(value.state == INSTALL_FAILED && value.errorCode == test.error);
        assert(value.stage == test.stage && value.nativeCode == RAW);
        assert(value.mode == INSTALL_MODE_HTTP_LOCAL && value.sdkVersion == 0xffffffffU);
        assert(threadCreateCalls == 1 && sdkCalls == 1 && httpRegisterCalls == 1);
        assert(!jailbreakCalls && !restoreCalls && !globalNamespaceOpens && !storageRegisterCalls);
        assert(titleCalls == 0 && nativeFstatCalls > 0);
        assert(registeredPackageBytes == test.bytes && serverPackageBytes == test.bytes);
        assert(registeredPackageBytes > UINT32_MAX);
        assert(!serverActive && !borrowedFile && access(copyDirectory.c_str(), F_OK) != 0);
        serverClosedAfterStop();
        if (test.fault == REGISTER_FAIL) {
            assert(value.taskId == -1 && !progressCalls && !stopCalls && !unregisterCalls);
        } else {
            assert(progressCalls == 1 && stopCalls == 1 && unregisterCalls == 1);
            assert(eventIndex(TASK_UNREGISTER) < eventIndex(SERVER_STOP));
        }
        assert(unlink(path.c_str()) == 0);
    }
    for (uint64_t tooLarge : { acceptedCap + 1, UINT64_MAX }) {
        reset(NORMAL);
        InstallSpec invalid = spec(); invalid.expectedBytes = tooLarge;
        uint32_t generation = installSnapshot().generation;
        assert(!startInstall(invalid));
        InstallSnapshot value = installSnapshot();
        assert(value.generation == generation + 1 && value.state == INSTALL_FAILED);
        assert(value.errorCode == INSTALL_ERROR_SPEC && value.stage == INSTALL_STAGE_SPEC);
        assert(value.mode == INSTALL_MODE_NONE && value.sdkVersion == 0 && value.sdkErrno == 0);
        assert(!threadCreateCalls && !sdkCalls && !nativeFstatCalls && !serverStartCalls && !registerCalls);
        assert(registeredPackageBytes == 0 && serverPackageBytes == 0);
        noHttpTelemetry(value);
    }
}
int main() {
    struct stat unavailable;
    assert(lstat("/tmp", &unavailable) == -1 && errno == ENOSYS);
    assert(unavailableLstatCalls == 1); unavailableLstatCalls = 0;
    // Exercise the native alias with a file larger than UINT32_MAX. Its size
    // lives at offset 72; offset 80 holds a distinct block count. A libc stat
    // cast would decode the wrong value and would also overwrite the canary.
    reset(NORMAL);
    int nativeDescriptor = __real_open((localDirectory + "/native-layout.bin").c_str(),
                                       O_CREAT | O_RDWR | O_EXCL, 0600);
    assert(nativeDescriptor >= 0);
    assert(ftruncate(nativeDescriptor, (off_t)(NATIVE_LENGTH + 123)) == 0);
    struct stat hostInfo;
    assert(__real_fstat(nativeDescriptor, &hostInfo) == 0);
    struct {
        uint64_t before;
        PeppyFileInfo info;
        uint64_t after;
    } guarded = { 0x1122334455667788ULL, {}, 0x8877665544332211ULL };
    assert(peppyInstallFstat(nativeDescriptor, &guarded.info) == 0);
    assert(guarded.before == 0x1122334455667788ULL && guarded.after == 0x8877665544332211ULL);
    assert(guarded.info.st_dev == (uint32_t)hostInfo.st_dev &&
           guarded.info.st_ino == (uint32_t)hostInfo.st_ino);
    assert(guarded.info.st_mode == (uint16_t)hostInfo.st_mode &&
           guarded.info.st_nlink == (uint16_t)hostInfo.st_nlink);
    assert(guarded.info.st_uid == (uint32_t)hostInfo.st_uid &&
           guarded.info.st_gid == (uint32_t)hostInfo.st_gid &&
           guarded.info.st_rdev == (uint32_t)hostInfo.st_rdev);
    assert(guarded.info.st_size == (int64_t)(NATIVE_LENGTH + 123) &&
           guarded.info.st_blocks == hostInfo.st_blocks &&
           guarded.info.st_size != guarded.info.st_blocks);
    assert(guarded.info.st_blksize == hostInfo.st_blksize &&
           guarded.info.st_flags == 0x12345678U && guarded.info.st_gen == 0x23456789U &&
           guarded.info.st_lspare == 0x34567890);
    assert(guarded.info.birthTime[0] == 0x0123456789abcdefLL &&
           guarded.info.birthTime[1] == 987654321);
    assert(__real_close(nativeDescriptor) == 0);
    InstallSnapshot value = run(NORMAL);
    assert(value.state == INSTALL_DONE && value.percent == 100 && value.errorCode == 0);
    assert(value.mode == INSTALL_MODE_STORAGE && value.sdkVersion == 0x100 && value.sdkErrno == 0);
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
    value = run(NO_SDK); assert(value.state == INSTALL_DONE && !jailbreakCalls && !restoreCalls);
    assert(value.mode == INSTALL_MODE_HTTP_LOCAL && value.sdkVersion == 0xffffffffU && value.sdkErrno == 78);
    serverClosedAfterStop();
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
        noHttpTelemetry(value);
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
    failed(SOURCE_STAT_FAIL, INSTALL_ERROR_FILE, INSTALL_STAGE_FILE, RAW_ENOSYS);
    assert(!jailbreakCalls && !registerCalls);
    failed(GLOBAL_STAT_FAIL, INSTALL_ERROR_GLOBAL_PATH, INSTALL_STAGE_GLOBAL_PATH, RAW_ENOSYS);
    assert(!registerCalls && access(copyDirectory.c_str(), F_OK) != 0);
    failed(COPY_STAT_FAIL, INSTALL_ERROR_COPY, INSTALL_STAGE_COPY, RAW_ENOSYS);
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
    assert(noFollowOpens > 0 && nativeFstatCalls > 0 &&
           unavailableLstatCalls == 0 && unavailablePosixFstatCalls == 0);
    httpFallbackTests();
    httpTelemetryTests();
    largePackageTests();
    assert(unavailableLstatCalls == 0 && unavailablePosixFstatCalls == 0);
    puts("All native installer ABI, SDK/storage and HTTP fallback/telemetry, lifetime, progress, preservation, errors and cancellation tests passed.");
}
