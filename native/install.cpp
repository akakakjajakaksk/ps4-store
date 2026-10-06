#include "install.h"
#include "package_limits.h"

#ifndef PEPPY_PKG_SERVER_HEADER
#define PEPPY_PKG_SERVER_HEADER "pkg_server.h"
#endif
#include PEPPY_PKG_SERVER_HEADER

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/AppInstUtil.h>
#include <orbis/Bgft.h>

static_assert(sizeof(size_t) == 8 && sizeof(off_t) == 8,
              "PS4 package installation requires 64-bit lengths and file offsets");

#if defined(__FreeBSD__) || defined(PS4)
// These are PS4 open flags, rather than the host's Linux values.
static_assert(O_NOFOLLOW == 0x100 && O_DIRECTORY == 0x20000, "PS4 open flags ABI");
#endif

// The packaged musl stat uses a 32-bit mode_t, placing st_size at offset 80.
// The PS4 kernel writes a 16-bit mode and st_size at offset 72. Keep its layout
// explicit; passing the incompatible POSIX struct would read the block count
// as the file size. The native export uses this 120-byte FreeBSD layout.
struct PeppyFileInfo {
    uint32_t st_dev, st_ino;
    uint16_t st_mode, st_nlink;
    uint32_t st_uid, st_gid, st_rdev;
    int64_t accessTime[2], modificationTime[2], changeTime[2];
    int64_t st_size, st_blocks;
    int32_t st_blksize;
    uint32_t st_flags, st_gen;
    int32_t st_lspare;
    int64_t birthTime[2];
};
static_assert(sizeof(PeppyFileInfo) == 120 && offsetof(PeppyFileInfo, st_dev) == 0 &&
              offsetof(PeppyFileInfo, st_ino) == 4 && offsetof(PeppyFileInfo, st_mode) == 8 &&
              offsetof(PeppyFileInfo, st_size) == 72 && offsetof(PeppyFileInfo, st_blocks) == 80,
              "PS4 native file metadata ABI");
extern "C" int32_t peppyInstallFstat(int32_t, PeppyFileInfo*) __asm__("sceKernelFstat");

// OpenOrbis 0.5.4 BGFT counters/packageSize are uint32_t. The native ABI
// uses unsigned long (64 bits on PS4), documented by flatz and Itemzflow:
// https://github.com/flatz/ps4_stub_lib_maker_v2/blob/master/include/bgft.h
struct PeppyBgftInit { void* heap; size_t heapSize; };
struct PeppyBgftParam {
    int32_t userId, entitlementType;
    const char *id, *contentUrl, *contentExUrl, *contentName, *iconPath, *skuId;
    uint32_t option;
    const char *playgoScenarioId, *releaseDate, *packageType, *packageSubType;
    uint64_t packageSize;
};
struct PeppyBgftParamEx { PeppyBgftParam params; uint32_t slot; };
struct PeppyBgftProgress {
    uint32_t bits;
    int32_t errorResult;
    uint64_t length, transferred, lengthTotal, transferredTotal;
    uint32_t numIndex, numTotal, restSec, restSecTotal;
    int32_t preparingPercent, localCopyPercent;
};
static_assert(sizeof(PeppyBgftInit) == 16, "native BGFT init ABI");
static_assert(sizeof(PeppyBgftParam) == 104, "native BGFT param ABI");
static_assert(offsetof(PeppyBgftParam, packageSize) == 96, "BGFT package size ABI");
static_assert(sizeof(PeppyBgftParamEx) == 112 && offsetof(PeppyBgftParamEx, slot) == 104,
              "native BGFT extended param ABI");
static_assert(sizeof(PeppyBgftProgress) == 64 &&
              offsetof(PeppyBgftProgress, transferred) == 16 &&
              offsetof(PeppyBgftProgress, localCopyPercent) == 60, "native BGFT progress ABI");
extern "C" int32_t peppyInstallModuleLoaded(OrbisSysModuleInternal)
    __asm__("sceSysmoduleIsLoadedInternal");
extern "C" int32_t peppyBgftInit(PeppyBgftInit*) __asm__("sceBgftServiceIntInit");
extern "C" int32_t peppyBgftRegister(PeppyBgftParamEx*, int32_t*)
    __asm__("sceBgftServiceIntDownloadRegisterTaskByStorageEx");
extern "C" int32_t peppyBgftRegisterHttp(PeppyBgftParam*, int32_t*)
    __asm__("sceBgftServiceIntDownloadRegisterTask");
extern "C" int32_t peppyBgftRegisterDebug(PeppyBgftParam*, int32_t*)
    __asm__("sceBgftServiceIntDebugDownloadRegisterPkg");
extern "C" int32_t peppyBgftProgress(int32_t, PeppyBgftProgress*)
    __asm__("sceBgftServiceDownloadGetProgress");
extern "C" int32_t peppyInstallForegroundUser(int32_t*)
    __asm__("sceUserServiceGetForegroundUser");

// The official GoldHEN SDK saves/restores credentials and namespace without
// firmware-specific kernel offsets. The bridge is vendored separately under MIT.
struct PeppyJailbreakBackup {
    uint32_t uid, ruid, rgid, groups;
    uint64_t paid, caps[2];
    void *prison, *cdir, *jdir, *rdir;
};
static_assert(sizeof(PeppyJailbreakBackup) == 72 &&
              offsetof(PeppyJailbreakBackup, prison) == 40, "GoldHEN SDK backup ABI");
extern "C" int32_t peppyInstallSdkVersion() __asm__("sys_sdk_version");
extern "C" int32_t peppyInstallJailbreak(PeppyJailbreakBackup*) __asm__("sys_sdk_jailbreak");
extern "C" int32_t peppyInstallRestore(PeppyJailbreakBackup*) __asm__("sys_sdk_unjailbreak");

#ifndef PEPPY_DOWNLOAD_DIRECTORY
#define PEPPY_DOWNLOAD_DIRECTORY "/data/peppy-store/downloads"
#endif
#ifndef PEPPY_INSTALL_SYSTEM_DIRECTORY
// BGFT is a system service. Its path must be outside the application's /data
// mount; ezRemote's local installer maps /data/... to /user/data/....
#define PEPPY_INSTALL_SYSTEM_DIRECTORY "/user/data/peppy-store/downloads"
#endif
#ifndef PEPPY_INSTALL_COPY_DIRECTORY
#define PEPPY_INSTALL_COPY_DIRECTORY "/user/data/peppy-store/install"
#endif
#ifndef PEPPY_INSTALL_ROOT_DIRECTORY
#define PEPPY_INSTALL_ROOT_DIRECTORY "/user/data/peppy-store"
#endif
#define PEPPY_INBOX_BASE_DIRECTORY "/data/peppy-store/inbox/base"
#define PEPPY_INBOX_UPDATE_DIRECTORY "/data/peppy-store/inbox/update"
#define PEPPY_INBOX_DLC_DIRECTORY "/data/peppy-store/inbox/dlc"
#ifndef PEPPY_TITLE_ID
#define PEPPY_TITLE_ID "BREW00001"
#endif

namespace {
const size_t NAME_CAP = 96, TITLE_CAP = 256, PATH_CAP = 512;
const size_t HEADER_BYTES = 0x438, CONTENT_ID_OFFSET = 0x40, CONTENT_ID_BYTES = 36;
const size_t BGFT_HEAP_BYTES = 1024 * 1024;
const unsigned POLL_US = 250000;
const unsigned MAX_POLLS = 30 * 60 * 1000000U / POLL_US;

int g_installState = INSTALL_IDLE, g_installBusy = 0, g_installCancel = 0;
int g_installPercent = 0, g_installError = 0, g_installStage = 0;
int32_t g_installNative = 0, g_installCleanup = 0;
int g_installCleanupStage = 0, g_installUnsafe = 0;
int g_installTask = -1, g_installPreparing = 0, g_installCopy = 0;
uint64_t g_installReceived = 0, g_installTotal = 0;
uint32_t g_installBits = 0;
uint32_t g_installGeneration = 0;
int g_installMode = INSTALL_MODE_NONE, g_installSdkErrno = 0;
uint32_t g_installSdkVersion = 0;
uint32_t g_installHttpRequests = 0;
int32_t g_installHttpStatus = 0;
uint64_t g_installHttpBytes = 0;
char g_installFilename[NAME_CAP], g_installName[TITLE_CAP];
char g_installSourcePath[PATH_CAP];
int g_installInboxKind = -1;
uint64_t g_installExpected = 0;
FILE* g_installLog = 0;

size_t lengthBounded(const char* value, size_t maximum) {
    if (!value) return maximum;
    size_t size = 0;
    while (size < maximum && value[size]) ++size;
    return size;
}
char asciiLower(char c) {
    return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c;
}
bool validFilename(const char* value) {
    size_t size = lengthBounded(value, NAME_CAP);
    if (size < 5 || size >= NAME_CAP || value[0] == '.' || strstr(value, ".."))
        return false;
    if (value[size - 4] != '.' ||
        asciiLower(value[size - 3]) != 'p' ||
        asciiLower(value[size - 2]) != 'k' ||
        asciiLower(value[size - 1]) != 'g') return false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char ch = (unsigned char)value[i];
        if (ch < 32 || ch == 127 || ch == '/' || ch == '\\' || ch == ':')
            return false;
    }
    return true;
}
uint32_t readBe32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
int classifyPackage(const unsigned char* header, const char*& packageType, bool& patch) {
    const uint32_t contentType = readBe32(header + 0x74);
    const uint32_t flags = readBe32(header + 0x78);
    const uint32_t FIRST_PATCH = 0x00100000;
    const uint32_t SUBSEQUENT_PATCH = 0x40000000;
    const uint32_t DELTA_PATCH = 0x41000000;
    const uint32_t CUMULATIVE_PATCH = 0x60000000;

    patch = false;
    if (contentType == 0x1A) packageType = "PS4GD";
    else if (contentType == 0x1B) packageType = "PS4AC";
    else if (contentType == 0x1C) packageType = "PS4AL";
    else if (contentType == 0x1E) packageType = "PS4DP";
    else return -1;

    patch = contentType == 0x1E ||
            (flags & FIRST_PATCH) ||
            (flags & SUBSEQUENT_PATCH) ||
            (flags & DELTA_PATCH) ||
            (flags & CUMULATIVE_PATCH);
    if (patch) return 1;
    if (contentType == 0x1B || contentType == 0x1C) return 2;
    return 0;
}
const char* inboxDirectory(int kind) {
    if (kind == 0) return PEPPY_INBOX_BASE_DIRECTORY;
    if (kind == 1) return PEPPY_INBOX_UPDATE_DIRECTORY;
    if (kind == 2) return PEPPY_INBOX_DLC_DIRECTORY;
    return 0;
}
bool validInboxSource(const char* path, int kind, const char* filename) {
    const char* root = inboxDirectory(kind);
    if (!root || !path || !filename) return false;
    size_t rootLength = strlen(root);
    if (strncmp(path, root, rootLength) || path[rootLength] != '/') return false;
    const char* leaf = path + rootLength + 1;
    return validFilename(leaf) && !strcmp(leaf, filename) &&
           lengthBounded(path, PATH_CAP) < PATH_CAP;
}
bool validName(const char* value) {
    size_t size = lengthBounded(value, TITLE_CAP);
    if (!size || size >= TITLE_CAP) return false;
    for (size_t i = 0; i < size; ++i)
        if ((unsigned char)value[i] < 32 || (unsigned char)value[i] == 127) return false;
    return true;
}
bool validTitleId(const char* value) {
    if (lengthBounded(value, 16) != 9) return false;
    for (int i = 0; i < 9; ++i)
        if (!(value[i] >= 'A' && value[i] <= 'Z') &&
            !(value[i] >= '0' && value[i] <= '9')) return false;
    return true;
}
bool cancelled() { return __atomic_load_n(&g_installCancel, __ATOMIC_ACQUIRE) != 0; }
void stage(int value) { __atomic_store_n(&g_installStage, value, __ATOMIC_RELEASE); }
void logCall(int where, int32_t native) {
    if (!g_installLog) return;
    int rc = fprintf(g_installLog,
        "stage=%d native=0x%08X task=%d mode=%d sdk=0x%08X sdk_errno=%d http_requests=%u http_status=%d http_bytes=%llu\n", where,
        (unsigned)native, __atomic_load_n(&g_installTask, __ATOMIC_ACQUIRE),
        __atomic_load_n(&g_installMode, __ATOMIC_ACQUIRE),
        __atomic_load_n(&g_installSdkVersion, __ATOMIC_ACQUIRE),
        __atomic_load_n(&g_installSdkErrno, __ATOMIC_ACQUIRE),
        (unsigned)__atomic_load_n(&g_installHttpRequests, __ATOMIC_ACQUIRE),
        __atomic_load_n(&g_installHttpStatus, __ATOMIC_ACQUIRE),
        (unsigned long long)__atomic_load_n(&g_installHttpBytes, __ATOMIC_ACQUIRE));
    if (rc < 0 || fflush(g_installLog)) { fclose(g_installLog); g_installLog = 0; }
}
int fail(int category, int where, int32_t native) {
    stage(where);
    __atomic_store_n(&g_installNative, native, __ATOMIC_RELEASE);
    logCall(where, native);
    return category;
}
void cleanupError(int where, int32_t native) {
    if (!native) return;
    if (where == INSTALL_STAGE_RESTORE) {
        // Restoring the application's credentials takes priority over other
        // cleanup diagnostics; earlier failures remain in install.log.
        __atomic_store_n(&g_installCleanup, native, __ATOMIC_RELEASE);
        __atomic_store_n(&g_installCleanupStage, where, __ATOMIC_RELEASE);
        logCall(where, native);
        return;
    }
    int32_t zero = 0;
    if (__atomic_compare_exchange_n(&g_installCleanup, &zero, native, false,
                                   __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        __atomic_store_n(&g_installCleanupStage, where, __ATOMIC_RELEASE);
    logCall(where, native);
}
int module(OrbisSysModuleInternal id, int where) {
    stage(where);
    if (peppyInstallModuleLoaded(id) == 0) return 0;
    int32_t rc = (int32_t)sceSysmoduleLoadModuleInternal(id);
    logCall(where, rc);
    if (rc && peppyInstallModuleLoaded(id) != 0) return fail(INSTALL_ERROR_MODULE, where, rc);
    return 0;
}

void sampleHttp(const PkgServer& server) {
    // These observations are diagnostic only: they never replace the native
    // BGFT result or decide whether the installation completed successfully.
    PkgServerSnapshot value = server.snapshot();
    __atomic_store_n(&g_installHttpRequests, value.requests, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installHttpStatus, value.status, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installHttpBytes, value.sentBytes, __ATOMIC_RELEASE);
}

struct InstallResources {
    FILE* file;
    void* heap;
    bool appReady, bgftReady, completed, jailbroken;
    bool serverAttempted;
    PkgServer server;
    PeppyJailbreakBackup backup;
    char partialPath[PATH_CAP];
    int32_t task;
    InstallResources() : file(0), heap(0), appReady(false), bgftReady(false),
                         completed(false), jailbroken(false), serverAttempted(false),
                         backup(), task(-1) { partialPath[0] = 0; }
    ~InstallResources() {
        if (serverAttempted) sampleHttp(server);
        if (task >= 0 && !completed) {
            int32_t rc = sceBgftServiceDownloadStopTask(task);
            cleanupError(INSTALL_STAGE_STOP, rc);
            if (rc) __atomic_store_n(&g_installUnsafe, 1, __ATOMIC_RELEASE);
            rc = sceBgftServiceIntDownloadUnregisterTask(task);
            cleanupError(INSTALL_STAGE_UNREGISTER, rc);
            if (rc) __atomic_store_n(&g_installUnsafe, 1, __ATOMIC_RELEASE);
        }
        if (serverAttempted) {
            // stop waits for every server worker even when it reports an
            // error. Keep the borrowed FILE alive until that join finishes.
            sampleHttp(server);
            int32_t rc = server.stop();
            // stop is quiescent and retains counters, including requests or
            // partial writes made while native task cleanup was in progress.
            sampleHttp(server);
            cleanupError(INSTALL_STAGE_SERVER_STOP, rc);
            if (rc) __atomic_store_n(&g_installUnsafe, 1, __ATOMIC_RELEASE);
        }
        if (bgftReady) {
            int32_t rc = sceBgftServiceIntTerm();
            cleanupError(INSTALL_STAGE_BGFT_TERM, rc);
            if (rc) __atomic_store_n(&g_installUnsafe, 1, __ATOMIC_RELEASE);
            // A failed term can leave the native service using its heap. Keep
            // it allocated rather than creating a use-after-free in that case.
            if (!rc) { free(heap); heap = 0; }
        } else { free(heap); heap = 0; }
        if (appReady) {
            int32_t rc = sceAppInstUtilTerminate();
            cleanupError(INSTALL_STAGE_APP_TERM, rc);
            if (rc) __atomic_store_n(&g_installUnsafe, 1, __ATOMIC_RELEASE);
        }
        if (partialPath[0] && unlink(partialPath) && errno != ENOENT)
            cleanupError(INSTALL_STAGE_COPY, errno);
        if (file && fclose(file)) cleanupError(INSTALL_STAGE_FILE, errno ? errno : EIO);
        if (jailbroken) {
            int32_t rc = peppyInstallRestore(&backup);
            cleanupError(INSTALL_STAGE_RESTORE, rc);
            if (rc) __atomic_store_n(&g_installUnsafe, 1, __ATOMIC_RELEASE);
        }
    }
};

// OpenOrbis musl lstat calls fstatat, whose PS4 implementation always returns
// ENOSYS. Atomically refuse symlinks with O_NOFOLLOW, then inspect the opened
// object with sceKernelFstat's native layout; never substitute a following stat call.
// https://github.com/OpenOrbis/musl/blob/master/src/stat/fstatat.c
// https://github.com/OpenOrbis/musl/blob/master/arch/ps4/bits/fcntl.h
int pathInfo(const char* path, bool directory, PeppyFileInfo& info) {
    int flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK;
    if (directory) flags |= O_DIRECTORY;
    int descriptor = open(path, flags);
    if (descriptor < 0) return errno;
    int error = peppyInstallFstat(descriptor, &info);
    if (close(descriptor) && !error) error = errno;
    if (!error && directory && !S_ISDIR(info.st_mode)) error = ENOTDIR;
    return error;
}
int checkedDirectory(const char* path) {
    if (mkdir(path, 0755) && errno != EEXIST) return errno;
    PeppyFileInfo info;
    return pathInfo(path, true, info);
}
bool sameFile(const PeppyFileInfo& a, const PeppyFileInfo& b) {
    return S_ISREG(b.st_mode) && a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size;
}
int systemFile(InstallResources& owned, const PeppyFileInfo& source, char* destination, size_t capacity) {
    stage(INSTALL_STAGE_GLOBAL_PATH);
    PeppyFileInfo global;
    int aliasError = pathInfo(destination, false, global);
    if (!aliasError && sameFile(source, global)) return 0;
    // Only a missing path, a different inode/type or a refused symlink can
    // require the safe FD copy. Metadata/access failures keep their real error.
    if (aliasError && aliasError != ENOENT && aliasError != ENOTDIR && aliasError != ELOOP)
        return fail(INSTALL_ERROR_GLOBAL_PATH, INSTALL_STAGE_GLOBAL_PATH, aliasError);
    // Namespace mounts differ between firmware/exploit setups. Copy from the
    // validated, still-open descriptor; never reopen its former sandbox path.
    stage(INSTALL_STAGE_COPY);
    int rc = checkedDirectory(PEPPY_INSTALL_ROOT_DIRECTORY);
    if (!rc) rc = checkedDirectory(PEPPY_INSTALL_COPY_DIRECTORY);
    if (rc) return fail(INSTALL_ERROR_GLOBAL_PATH, INSTALL_STAGE_GLOBAL_PATH, rc);
    int size = snprintf(destination, capacity, "%s/%s", PEPPY_INSTALL_COPY_DIRECTORY, g_installFilename);
    if (size < 0 || (size_t)size >= capacity)
        return fail(INSTALL_ERROR_GLOBAL_PATH, INSTALL_STAGE_GLOBAL_PATH, ENAMETOOLONG);
    size = snprintf(owned.partialPath, sizeof(owned.partialPath), "%s.part", destination);
    if (size < 0 || (size_t)size >= sizeof(owned.partialPath)) {
        owned.partialPath[0] = 0;
        return fail(INSTALL_ERROR_COPY, INSTALL_STAGE_COPY, ENAMETOOLONG);
    }
    int flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW;
    int output = open(owned.partialPath, flags, 0644);
    if (output < 0) {
        owned.partialPath[0] = 0; // Do not remove a preexisting file we never owned.
        return fail(INSTALL_ERROR_COPY, INSTALL_STAGE_COPY, errno);
    }
    PeppyFileInfo created, copiedInfo;
    int error = peppyInstallFstat(output, &created);
    if (!error && !S_ISREG(created.st_mode)) error = EINVAL;
    if (!error && fseek(owned.file, 0, SEEK_SET)) error = errno ? errno : EIO;
    unsigned char buffer[64 * 1024];
    uint64_t copied = 0;
    while (!error && copied < g_installExpected && !cancelled()) {
        size_t want = (size_t)((g_installExpected - copied) < sizeof(buffer)
                              ? g_installExpected - copied : sizeof(buffer));
        size_t amount = fread(buffer, 1, want, owned.file);
        if (amount != want) { error = errno ? errno : EIO; break; }
        size_t offset = 0;
        while (offset < amount) {
            ssize_t written = write(output, buffer + offset, amount - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { error = errno ? errno : EIO; break; }
            offset += (size_t)written;
        }
        if (!error) {
            copied += amount;
            __atomic_store_n(&g_installReceived, copied, __ATOMIC_RELEASE);
            __atomic_store_n(&g_installTotal, g_installExpected, __ATOMIC_RELEASE);
        }
    }
    if (!error && !cancelled() && fsync(output)) error = errno;
    if (!error && !cancelled()) error = peppyInstallFstat(output, &copiedInfo);
    if (!error && !cancelled() && (!S_ISREG(copiedInfo.st_mode) ||
        created.st_dev != copiedInfo.st_dev || created.st_ino != copiedInfo.st_ino ||
        copiedInfo.st_size != source.st_size)) error = EINVAL;
    if (close(output) && !error) error = errno;
    if (error) return fail(INSTALL_ERROR_COPY, INSTALL_STAGE_COPY, error);
    if (cancelled()) return 0;
    int metadataError = peppyInstallFstat(fileno(owned.file), &global);
    if (metadataError) return fail(INSTALL_ERROR_COPY, INSTALL_STAGE_COPY, metadataError);
    if (!sameFile(source, global)) return fail(INSTALL_ERROR_COPY, INSTALL_STAGE_COPY, EINVAL);
    error = pathInfo(owned.partialPath, false, global);
    if (error) return fail(INSTALL_ERROR_COPY, INSTALL_STAGE_COPY, error);
    if (!sameFile(copiedInfo, global))
        return fail(INSTALL_ERROR_COPY, INSTALL_STAGE_COPY, EINVAL);
    if (rename(owned.partialPath, destination)) return fail(INSTALL_ERROR_COPY, INSTALL_STAGE_COPY, errno);
    owned.partialPath[0] = 0;
    __atomic_store_n(&g_installReceived, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installTotal, 0, __ATOMIC_RELEASE);
    return 0;
}

int runInstall() {
    InstallResources owned;
    char localPath[PATH_CAP], systemPath[PATH_CAP];
    const char* sourceDirectory = g_installInboxKind >= 0
        ? inboxDirectory(g_installInboxKind) : PEPPY_DOWNLOAD_DIRECTORY;
    int localLength = g_installSourcePath[0]
        ? snprintf(localPath, sizeof(localPath), "%s", g_installSourcePath)
        : snprintf(localPath, sizeof(localPath), "%s/%s", PEPPY_DOWNLOAD_DIRECTORY,
                   g_installFilename);
    int systemLength = !strncmp(localPath, "/data/", 6)
        ? snprintf(systemPath, sizeof(systemPath), "/user%s", localPath)
        : snprintf(systemPath, sizeof(systemPath), "%s/%s", PEPPY_INSTALL_SYSTEM_DIRECTORY,
                   g_installFilename);
    if (!sourceDirectory || localLength < 0 || (size_t)localLength >= sizeof(localPath) ||
        systemLength < 0 || (size_t)systemLength >= sizeof(systemPath))
        return fail(INSTALL_ERROR_FILE, INSTALL_STAGE_FILE, ENAMETOOLONG);
    stage(INSTALL_STAGE_FILE);
    PeppyFileInfo directoryInfo, after;
    int directoryError = pathInfo(sourceDirectory, true, directoryInfo);
    if (directoryError) return fail(INSTALL_ERROR_FILE, INSTALL_STAGE_FILE, directoryError);
    int descriptor = open(localPath, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) return fail(INSTALL_ERROR_FILE, INSTALL_STAGE_FILE, errno);
    int fileError = peppyInstallFstat(descriptor, &after);
    if (!fileError && !S_ISREG(after.st_mode)) fileError = EINVAL;
    if (fileError) {
        if (close(descriptor)) cleanupError(INSTALL_STAGE_FILE, errno);
        return fail(INSTALL_ERROR_FILE, INSTALL_STAGE_FILE, fileError);
    }
    owned.file = fdopen(descriptor, "rb");
    if (!owned.file) {
        fileError = errno ? errno : ENOMEM;
        if (close(descriptor)) cleanupError(INSTALL_STAGE_FILE, errno);
        return fail(INSTALL_ERROR_FILE, INSTALL_STAGE_FILE, fileError);
    }
    if (after.st_size < (off_t)HEADER_BYTES || (uint64_t)after.st_size != g_installExpected)
        return fail(INSTALL_ERROR_PACKAGE, INSTALL_STAGE_PACKAGE, EINVAL);
    unsigned char header[HEADER_BYTES];
    size_t got = fread(header, 1, sizeof(header), owned.file);
    if (got != sizeof(header)) return fail(INSTALL_ERROR_FILE, INSTALL_STAGE_PACKAGE, errno ? errno : EIO);
    const unsigned char magic[] = { 0x7f, 'C', 'N', 'T' };
    if (memcmp(header, magic, sizeof(magic)))
        return fail(INSTALL_ERROR_PACKAGE, INSTALL_STAGE_PACKAGE, EINVAL);
    const char* packageType = 0;
    bool patchPackage = false;
    int packageKind = classifyPackage(header, packageType, patchPackage);
    if (packageKind < 0 || !packageType)
        return fail(INSTALL_ERROR_PACKAGE, INSTALL_STAGE_PACKAGE, EINVAL);
    char contentId[CONTENT_ID_BYTES + 1];
    memcpy(contentId, header + CONTENT_ID_OFFSET, CONTENT_ID_BYTES);
    contentId[CONTENT_ID_BYTES] = 0;
    for (size_t i = 0; i < CONTENT_ID_BYTES; ++i) {
        char c = contentId[i];
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !(c >= '0' && c <= '9') && c != '-' && c != '_')
            return fail(INSTALL_ERROR_PACKAGE, INSTALL_STAGE_PACKAGE, EINVAL);
    }
    if (cancelled()) return 0;
    stage(INSTALL_STAGE_SDK);
    errno = 0;
    int32_t sdk = peppyInstallSdkVersion();
    int sdkError = errno; // The official trampoline saves its raw error here.
    __atomic_store_n(&g_installSdkVersion, (uint32_t)sdk, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installSdkErrno, sdkError, __ATOMIC_RELEASE);
    bool storage = sdk == 0x100;
    __atomic_store_n(&g_installMode,
        storage ? INSTALL_MODE_STORAGE : INSTALL_MODE_HTTP_LOCAL, __ATOMIC_RELEASE);
    logCall(INSTALL_STAGE_SDK, sdk);
    if (storage) {
        stage(INSTALL_STAGE_JAILBREAK);
        int32_t jailbreak = peppyInstallJailbreak(&owned.backup);
        // A failed credential change is never a reason to try another mode.
        if (jailbreak) return fail(INSTALL_ERROR_JAILBREAK, INSTALL_STAGE_JAILBREAK, jailbreak);
        owned.jailbroken = true;
    }
    if (cancelled()) return 0;
    if (storage) {
        int globalResult = systemFile(owned, after, systemPath, sizeof(systemPath));
        if (globalResult || cancelled()) return globalResult;
    }
    int result = module(ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL, INSTALL_STAGE_MODULE_APP);
    if (result) return result;
    result = module(ORBIS_SYSMODULE_INTERNAL_BGFT, INSTALL_STAGE_MODULE_BGFT);
    if (result) return result;
    stage(INSTALL_STAGE_APP_INIT);
    int32_t rc = sceAppInstUtilInitialize();
    if (rc) return fail(INSTALL_ERROR_SERVICE, INSTALL_STAGE_APP_INIT, rc);
    owned.appReady = true;
    char titleId[16] = {};
    stage(INSTALL_STAGE_TITLE);
    // sceAppInstUtilGetTitleIdFromPkg resolves paths in the system-service
    // namespace. An application-sandbox /data path is therefore invalid on
    // consoles where the GoldHEN SDK syscall is unavailable and we use the
    // loopback HTTP installer. The PKG header has already been read from a
    // held, validated descriptor, so derive the Title ID from its Content ID
    // in HTTP mode. Storage mode still asks AppInstUtil to parse the global
    // /user/data path and cross-checks both values below.
    if (contentId[6] != '-' || contentId[16] != '_' || contentId[19] != '-')
        return fail(INSTALL_ERROR_PACKAGE, INSTALL_STAGE_TITLE, EINVAL);
    memcpy(titleId, contentId + 7, 9);
    titleId[9] = 0;
    if (!validTitleId(titleId))
        return fail(INSTALL_ERROR_PACKAGE, INSTALL_STAGE_TITLE, EINVAL);
    if (storage) {
        char parsedTitleId[16] = {};
        int32_t isApp = -1;
        rc = sceAppInstUtilGetTitleIdFromPkg(systemPath, parsedTitleId, &isApp);
        if (rc) return fail(INSTALL_ERROR_PACKAGE, INSTALL_STAGE_TITLE, rc);
        if (!validTitleId(parsedTitleId) || (isApp != 0 && isApp != 1) ||
            strcmp(parsedTitleId, titleId))
            return fail(INSTALL_ERROR_PACKAGE, INSTALL_STAGE_TITLE, EINVAL);
    }
    if (!strcmp(titleId, PEPPY_TITLE_ID))
        return fail(INSTALL_ERROR_SELF, INSTALL_STAGE_TITLE, 0);
    stage(INSTALL_STAGE_EXISTS);
    int32_t exists = 0;
    rc = sceAppInstUtilAppExists(titleId, &exists);
    if (rc) return fail(INSTALL_ERROR_SERVICE, INSTALL_STAGE_EXISTS, rc);
    if (packageKind == 0 && exists)
        return fail(INSTALL_ERROR_ALREADY_INSTALLED, INSTALL_STAGE_EXISTS, 0);
    if (packageKind != 0 && !exists)
        return fail(INSTALL_ERROR_BASE_REQUIRED, INSTALL_STAGE_EXISTS, 0);
    if (cancelled()) return 0;
    int32_t userId = 0;
    if (!storage) {
        stage(INSTALL_STAGE_USER);
        userId = -1;
        rc = peppyInstallForegroundUser(&userId);
        if (rc) return fail(INSTALL_ERROR_USER, INSTALL_STAGE_USER, rc);
        if (userId < 0) return fail(INSTALL_ERROR_USER, INSTALL_STAGE_USER, EINVAL);
        // Native title parsing uses a pathname. Confirm it still names the
        // same file, while HTTP itself always serves the held descriptor.
        PeppyFileInfo current;
        rc = pathInfo(localPath, false, current);
        if (!rc && !sameFile(after, current)) rc = EINVAL;
        if (!rc) rc = peppyInstallFstat(fileno(owned.file), &current);
        if (!rc && !sameFile(after, current)) rc = EINVAL;
        if (rc) return fail(INSTALL_ERROR_FILE, INSTALL_STAGE_FILE, rc);
        if (fseek(owned.file, 0, SEEK_SET))
            return fail(INSTALL_ERROR_FILE, INSTALL_STAGE_FILE, errno ? errno : EIO);
    }
    stage(INSTALL_STAGE_HEAP);
    owned.heap = calloc(1, BGFT_HEAP_BYTES);
    if (!owned.heap) return fail(INSTALL_ERROR_SERVICE, INSTALL_STAGE_HEAP, ENOMEM);
    PeppyBgftInit init = { owned.heap, BGFT_HEAP_BYTES };
    stage(INSTALL_STAGE_BGFT_INIT);
    rc = peppyBgftInit(&init);
    if (rc) return fail(INSTALL_ERROR_SERVICE, INSTALL_STAGE_BGFT_INIT, rc);
    owned.bgftReady = true;
    if (cancelled()) return 0;
    if (!storage) {
        stage(INSTALL_STAGE_SERVER_START);
        owned.serverAttempted = true;
        rc = owned.server.start(owned.file, g_installExpected);
        sampleHttp(owned.server);
        if (rc) return fail(INSTALL_ERROR_SERVER, INSTALL_STAGE_SERVER_START, rc);
        rc = owned.server.errorCode();
        if (rc) return fail(INSTALL_ERROR_SERVER, INSTALL_STAGE_SERVER_TRANSFER, rc);
        const char* url = owned.server.url();
        const char loopback[] = "http://127.0.0.1:";
        size_t urlLength = lengthBounded(url, PATH_CAP);
        if (!url || urlLength >= PATH_CAP || urlLength <= sizeof(loopback) - 1 ||
            strncmp(url, loopback, sizeof(loopback) - 1))
            return fail(INSTALL_ERROR_SERVER, INSTALL_STAGE_SERVER_START, EINVAL);
        if (cancelled()) return 0;
    }
    PeppyBgftParamEx params = {};
    params.params.userId = userId;
    params.params.entitlementType = 5;
    params.params.id = contentId;
    params.params.contentUrl = storage ? systemPath : owned.server.url();
    params.params.contentName = g_installName;
    params.params.iconPath = "";
    params.params.playgoScenarioId = "0";
    // Keep Peppy's progress UI and never force an existing application update.
    // HTTP follows RPI's CDN-query option; BGFT can still append optional
    // query parameters, which the loopback server accepts.
    params.params.option = storage ? 0x2 : 0x10002;
    if (packageKind != 0) params.params.option |= 0x8; // FORCE_UPDATE for patch/add-on.
    params.params.packageType = packageType;
    params.params.packageSubType = "";
    params.params.packageSize = g_installExpected;
    params.slot = 0;
    stage(INSTALL_STAGE_REGISTER);
    int32_t candidate = -1;
    rc = storage ? peppyBgftRegister(&params, &candidate)
                 : (patchPackage
                    ? peppyBgftRegisterDebug(&params.params, &candidate)
                    : peppyBgftRegisterHttp(&params.params, &candidate));
    if (!storage) sampleHttp(owned.server);
    if (rc) {
        uint32_t native = (uint32_t)rc;
        int category = native == 0x80990088U || native == 0x80990015U
            ? INSTALL_ERROR_ALREADY_INSTALLED
            : native == 0x80990086U ? INSTALL_ERROR_BUSY : INSTALL_ERROR_TASK;
        return fail(category, INSTALL_STAGE_REGISTER, rc);
    }
    if (candidate < 0) return fail(INSTALL_ERROR_TASK, INSTALL_STAGE_REGISTER, EINVAL);
    // A failed registration may populate its output with an existing task.
    // Adopt ownership only after success, so cleanup can never stop that task.
    owned.task = candidate;
    __atomic_store_n(&g_installTask, owned.task, __ATOMIC_RELEASE);
    if (cancelled()) return 0;
    stage(INSTALL_STAGE_START);
    rc = sceBgftServiceDownloadStartTask(owned.task);
    if (rc) return fail(INSTALL_ERROR_TASK, INSTALL_STAGE_START, rc);
    for (unsigned poll = 0; poll < MAX_POLLS; ++poll) {
        if (cancelled()) return 0;
        if (!storage) {
            sampleHttp(owned.server);
            rc = owned.server.errorCode();
            if (rc) return fail(INSTALL_ERROR_SERVER, INSTALL_STAGE_SERVER_TRANSFER, rc);
        }
        stage(INSTALL_STAGE_PROGRESS);
        PeppyBgftProgress progress = {};
        rc = peppyBgftProgress(owned.task, &progress);
        if (!storage) sampleHttp(owned.server);
        if (rc) return fail(INSTALL_ERROR_PROGRESS, INSTALL_STAGE_PROGRESS, rc);
        if (progress.errorResult) return fail(INSTALL_ERROR_PROGRESS, INSTALL_STAGE_PROGRESS, progress.errorResult);
        uint64_t total = progress.lengthTotal ? progress.lengthTotal : progress.length;
        uint64_t received = progress.lengthTotal ? progress.transferredTotal : progress.transferred;
        if (received > total) return fail(INSTALL_ERROR_PROGRESS, INSTALL_STAGE_PROGRESS, EINVAL);
        __atomic_store_n(&g_installReceived, received, __ATOMIC_RELEASE);
        __atomic_store_n(&g_installTotal, total, __ATOMIC_RELEASE);
        __atomic_store_n(&g_installBits, progress.bits, __ATOMIC_RELEASE);
        __atomic_store_n(&g_installPreparing, progress.preparingPercent, __ATOMIC_RELEASE);
        __atomic_store_n(&g_installCopy, progress.localCopyPercent, __ATOMIC_RELEASE);
        int percent = total ? (int)((double)received / (double)total * 100.0) : 0;
        if (percent >= 100) percent = 99;
        __atomic_store_n(&g_installPercent, percent, __ATOMIC_RELEASE);
        if (total && received == total) {
            stage(INSTALL_STAGE_CONFIRM);
            exists = 0;
            rc = sceAppInstUtilAppExists(titleId, &exists);
            if (rc) return fail(INSTALL_ERROR_SERVICE, INSTALL_STAGE_CONFIRM, rc);
            if (exists && !sceAppInstUtilAppIsInInstalling(contentId)) {
                if (!storage) {
                    rc = owned.server.errorCode();
                    if (rc) return fail(INSTALL_ERROR_SERVER, INSTALL_STAGE_SERVER_TRANSFER, rc);
                }
                owned.completed = true;
                __atomic_store_n(&g_installPercent, 100, __ATOMIC_RELEASE);
                return 0;
            }
        }
        sceKernelUsleep(POLL_US);
    }
    return fail(INSTALL_ERROR_TIMEOUT, INSTALL_STAGE_PROGRESS, 0);
}

void* installWorker(void*) {
    char logPath[PATH_CAP];
    int length = snprintf(logPath, sizeof(logPath), "%s/install.log", PEPPY_DOWNLOAD_DIRECTORY);
    if (length > 0 && (size_t)length < sizeof(logPath)) g_installLog = fopen(logPath, "w");
    int result = runInstall();
    int32_t cleanup = __atomic_load_n(&g_installCleanup, __ATOMIC_ACQUIRE);
    int finalState;
    if (__atomic_load_n(&g_installCleanupStage, __ATOMIC_ACQUIRE) == INSTALL_STAGE_RESTORE) {
        result = fail(INSTALL_ERROR_RESTORE, INSTALL_STAGE_RESTORE, cleanup);
        finalState = INSTALL_FAILED;
    } else if (result) finalState = INSTALL_FAILED;
    else if (cleanup) {
        result = fail(INSTALL_ERROR_CLEANUP,
            __atomic_load_n(&g_installCleanupStage, __ATOMIC_ACQUIRE), cleanup);
        finalState = INSTALL_FAILED;
    } else if (__atomic_load_n(&g_installPercent, __ATOMIC_ACQUIRE) == 100) {
        finalState = INSTALL_DONE;
        stage(INSTALL_STAGE_FINISHED);
    } else finalState = INSTALL_CANCELLED;
    __atomic_store_n(&g_installError, result, __ATOMIC_RELEASE);
    logCall(__atomic_load_n(&g_installStage, __ATOMIC_ACQUIRE),
            __atomic_load_n(&g_installNative, __ATOMIC_ACQUIRE));
    if (g_installLog) { fclose(g_installLog); g_installLog = 0; }
    __atomic_store_n(&g_installState, finalState, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installBusy, 0, __ATOMIC_RELEASE);
    return 0;
}
}

static bool startInstallInternal(const InstallSpec& spec,
                                 const char* sourcePath, int inboxKind) {
    // Failed native stop/term/restore may leave an owned task or service alive.
    // Do not overwrite its backing copy or reinitialize it in this process.
    if (__atomic_load_n(&g_installUnsafe, __ATOMIC_ACQUIRE)) return false;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&g_installBusy, &expected, 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return false;
    __atomic_add_fetch(&g_installGeneration, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installCancel, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installReceived, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installTotal, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installPercent, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installError, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installNative, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installCleanup, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installCleanupStage, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installTask, -1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installBits, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installPreparing, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installCopy, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installMode, INSTALL_MODE_NONE, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installSdkVersion, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installSdkErrno, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installHttpRequests, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installHttpStatus, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_installHttpBytes, 0, __ATOMIC_RELEASE);
    stage(INSTALL_STAGE_SPEC);
    bool sourceOk = !sourcePath ||
        validInboxSource(sourcePath, inboxKind, spec.filename);
    if (!validFilename(spec.filename) || !validName(spec.name) || !sourceOk ||
        spec.expectedBytes < HEADER_BYTES || spec.expectedBytes > PEPPY_MAX_PACKAGE_BYTES) {
        __atomic_store_n(&g_installError, INSTALL_ERROR_SPEC, __ATOMIC_RELEASE);
        __atomic_store_n(&g_installState, INSTALL_FAILED, __ATOMIC_RELEASE);
        __atomic_store_n(&g_installBusy, 0, __ATOMIC_RELEASE);
        return false;
    }
    strcpy(g_installFilename, spec.filename);
    strcpy(g_installName, spec.name);
    g_installSourcePath[0] = 0;
    g_installInboxKind = -1;
    if (sourcePath) {
        snprintf(g_installSourcePath, sizeof(g_installSourcePath), "%s", sourcePath);
        g_installInboxKind = inboxKind;
    }
    g_installExpected = spec.expectedBytes;
    __atomic_store_n(&g_installState, INSTALL_RUNNING, __ATOMIC_RELEASE);
    stage(INSTALL_STAGE_THREAD);
    OrbisPthreadAttr attributes;
    int32_t rc = scePthreadAttrInit(&attributes);
    bool initialized = rc == 0;
    if (!rc) rc = scePthreadAttrSetdetachstate(&attributes, 1);
    OrbisPthread thread;
    if (!rc) rc = scePthreadCreate(&thread, &attributes, installWorker, 0, "peppy-install");
    if (initialized) scePthreadAttrDestroy(&attributes);
    if (rc) {
        __atomic_store_n(&g_installNative, rc, __ATOMIC_RELEASE);
        __atomic_store_n(&g_installError, INSTALL_ERROR_THREAD, __ATOMIC_RELEASE);
        __atomic_store_n(&g_installState, INSTALL_FAILED, __ATOMIC_RELEASE);
        __atomic_store_n(&g_installBusy, 0, __ATOMIC_RELEASE);
        return false;
    }
    return true;
}
bool startInstall(const InstallSpec& spec) {
    return startInstallInternal(spec, 0, -1);
}

bool startInboxInstall(const char* path, const char* displayName,
                       uint64_t expectedBytes, int kind) {
    if (!path || !displayName) return false;
    const char* leaf = strrchr(path, '/');
    if (!leaf || !leaf[1]) return false;
    InstallSpec spec = {leaf + 1, displayName, expectedBytes};
    return startInstallInternal(spec, path, kind);
}

void cancelInstall() {
    if (__atomic_load_n(&g_installBusy, __ATOMIC_ACQUIRE))
        __atomic_store_n(&g_installCancel, 1, __ATOMIC_RELEASE);
}
InstallSnapshot installSnapshot() {
    InstallSnapshot value;
    value.state = __atomic_load_n(&g_installState, __ATOMIC_ACQUIRE);
    value.received = __atomic_load_n(&g_installReceived, __ATOMIC_ACQUIRE);
    value.total = __atomic_load_n(&g_installTotal, __ATOMIC_ACQUIRE);
    value.percent = __atomic_load_n(&g_installPercent, __ATOMIC_ACQUIRE);
    value.errorCode = __atomic_load_n(&g_installError, __ATOMIC_ACQUIRE);
    value.stage = __atomic_load_n(&g_installStage, __ATOMIC_ACQUIRE);
    value.nativeCode = __atomic_load_n(&g_installNative, __ATOMIC_ACQUIRE);
    value.taskId = __atomic_load_n(&g_installTask, __ATOMIC_ACQUIRE);
    value.progressBits = __atomic_load_n(&g_installBits, __ATOMIC_ACQUIRE);
    value.preparingPercent = __atomic_load_n(&g_installPreparing, __ATOMIC_ACQUIRE);
    value.localCopyPercent = __atomic_load_n(&g_installCopy, __ATOMIC_ACQUIRE);
    value.cleanupCode = __atomic_load_n(&g_installCleanup, __ATOMIC_ACQUIRE);
    value.cleanupStage = __atomic_load_n(&g_installCleanupStage, __ATOMIC_ACQUIRE);
    value.generation = __atomic_load_n(&g_installGeneration, __ATOMIC_ACQUIRE);
    value.mode = __atomic_load_n(&g_installMode, __ATOMIC_ACQUIRE);
    value.sdkVersion = __atomic_load_n(&g_installSdkVersion, __ATOMIC_ACQUIRE);
    value.sdkErrno = __atomic_load_n(&g_installSdkErrno, __ATOMIC_ACQUIRE);
    value.httpRequests = __atomic_load_n(&g_installHttpRequests, __ATOMIC_ACQUIRE);
    value.httpStatus = __atomic_load_n(&g_installHttpStatus, __ATOMIC_ACQUIRE);
    value.httpBytes = __atomic_load_n(&g_installHttpBytes, __ATOMIC_ACQUIRE);
    return value;
}
const char* installStageName(int value) {
    static const char* names[] = { "Pronto", "Pacote escolhido", "Inicialização", "Arquivo local",
        "Validação PKG", "Módulo AppInstUtil", "Módulo BGFT", "AppInstUtil",
        "Identificação do app", "App existente", "Memória BGFT", "Inicialização BGFT",
        "Registro local", "Início da instalação", "Instalando", "Confirmando instalação",
        "Cancelando", "Limpeza da tarefa", "Encerrando BGFT", "Encerrando AppInstUtil", "Concluído",
        "GoldHEN SDK", "Permissões GoldHEN", "Arquivo do sistema", "Preparando pacote", "Restaurando permissões",
        "Perfil do console", "Servidor HTTP local", "Transferência local", "Encerrando servidor local" };
    return value >= 0 && (size_t)value < sizeof(names) / sizeof(names[0]) ? names[value] : "Instalação";
}
