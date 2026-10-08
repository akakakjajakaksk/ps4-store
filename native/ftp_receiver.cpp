#include "ftp_receiver.h"
#include "user_pkg_header.h"

#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>

namespace {

const uint16_t CONTROL_PORT = 2121;
const uint16_t CONTROL_FALLBACK_FIRST = 2150;
const uint16_t CONTROL_FALLBACK_LAST = 2159;
const uint16_t PASSIVE_FIRST = 2122;
const uint16_t PASSIVE_LAST = 2141;
const size_t LINE_CAP = 512;
const size_t IO_CAP = 256 * 1024;
const int NET_SOL_SOCKET = 0xffff, NET_SO_REUSEADDR = 0x0004, NET_SO_NBIO = 0x1200;
const int NET_EINTR = 4, NET_EWOULDBLOCK = 35, NET_EADDRINUSE = 48;
const uint64_t DATA_TIMEOUT = 90000000, CONTROL_TIMEOUT = 300000000;
const int32_t ERROR_TIMEOUT = -2500, ERROR_PACKAGE = -2501;

struct SocketAddress {
    uint8_t length;
    uint8_t family;
    uint16_t port;
    uint32_t address;
    uint8_t zero[8];
};

static_assert(sizeof(SocketAddress) == 16 &&
              offsetof(SocketAddress, port) == 2 &&
              offsetof(SocketAddress, address) == 4,
              "PS4 sockaddr_in ABI");

static FtpReceiverSnapshot g = {};
static OrbisPthread g_thread;
static int g_stop = 0;
static unsigned char g_snapshotLock = 0;
static int32_t g_pool = -1;
static int32_t g_listener = -1;
static int32_t g_control = -1;
static int32_t g_dataListener = -1;
static int32_t g_data = -1;
static char g_ioBuffer[IO_CAP];

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
static_assert(sizeof(PeppyFileInfo) == 120 &&
              offsetof(PeppyFileInfo, st_size) == 72,
              "PS4 native file metadata ABI");
extern "C" int32_t peppyFtpFstat(int32_t, PeppyFileInfo*) __asm__("sceKernelFstat");
extern "C" int32_t* peppyFtpNetErrno() __asm__("sceNetErrnoLoc");

static bool stopped() { return __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) != 0; }
static void lockSnapshot() {
    while (__atomic_test_and_set(&g_snapshotLock, __ATOMIC_ACQUIRE)) sceKernelUsleep(100);
}
static void unlockSnapshot() { __atomic_clear(&g_snapshotLock, __ATOMIC_RELEASE); }
static void publishError(int32_t error) {
    lockSnapshot(); g.lastError = error; unlockSnapshot();
}
static int netErrno() {
    int32_t* error = peppyFtpNetErrno();
    return error ? *error : 0;
}
static int32_t networkError(int32_t rc, int error) {
    return rc == -1 && error > 0 && error < 256 ? int32_t(0x80410100u | unsigned(error)) : rc;
}
static bool retryable(int32_t rc, int error) {
    return error == NET_EINTR || error == NET_EWOULDBLOCK ||
           uint32_t(rc) == 0x80410104u || uint32_t(rc) == 0x80410123u;
}
static bool expired(uint64_t began, uint64_t timeout) {
    return sceKernelGetProcessTime() - began >= timeout;
}
static int32_t configureSocket(int32_t fd, bool listener) {
    int enabled = 1;
    if (listener) {
        int32_t rc = sceNetSetsockopt(fd, NET_SOL_SOCKET, NET_SO_REUSEADDR, &enabled, sizeof(enabled));
        if (rc != 0) return networkError(rc, netErrno());
    }
    int32_t rc = sceNetSetsockopt(fd, NET_SOL_SOCKET, NET_SO_NBIO, &enabled, sizeof(enabled));
    return rc ? networkError(rc, netErrno()) : 0;
}

static char lowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? char(c + ('a' - 'A')) : c;
}

static bool iequals(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (lowerAscii(*a) != lowerAscii(*b)) return false;
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

static bool startsI(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    while (*prefix) {
        if (!*s || lowerAscii(*s) != lowerAscii(*prefix)) return false;
        ++s;
        ++prefix;
    }
    return true;
}

static bool endsPkg(const char* s) {
    if (!s) return false;
    size_t n = strlen(s);
    return n > 4 &&
           lowerAscii(s[n - 4]) == '.' &&
           lowerAscii(s[n - 3]) == 'p' &&
           lowerAscii(s[n - 2]) == 'k' &&
           lowerAscii(s[n - 1]) == 'g';
}

static bool safeName(const char* s) {
    if (!s || !*s || strlen(s) >= sizeof(FtpInboxItem().name) ||
        strchr(s, '/') || strchr(s, '\\')) return false;

    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    while (*p) {
        if (*p < 0x80) {
            if (*p < 32 || *p == 127) return false;
            ++p;
            continue;
        }
        unsigned first = *p++;
        int continuation = first >= 0xc2 && first <= 0xdf ? 1 :
                           first >= 0xe0 && first <= 0xef ? 2 :
                           first >= 0xf0 && first <= 0xf4 ? 3 : 0;
        if (!continuation || !*p ||
            (first == 0xe0 && *p < 0xa0) || (first == 0xed && *p >= 0xa0) ||
            (first == 0xf0 && *p < 0x90) || (first == 0xf4 && *p >= 0x90)) return false;
        for (int i = 0; i < continuation; ++i) {
            if (*p < 0x80 || *p > 0xbf) return false;
            ++p;
        }
    }
    return endsPkg(s);
}

static const char* kindName(int kind) {
    if (kind == FTP_UPDATE) return "update";
    if (kind == FTP_DLC) return "dlc";
    return "base";
}

static int kindFromName(const char* name) {
    if (startsI(name, "update_")) return FTP_UPDATE;
    if (startsI(name, "dlc_")) return FTP_DLC;
    return FTP_BASE;
}

static bool parseVirtualDir(const char* arg, int& kind, bool& root) {
    if (!arg || strlen(arg) >= LINE_CAP) return false;
    char normalized[LINE_CAP];
    snprintf(normalized, sizeof(normalized), "%s", arg);
    size_t length = strlen(normalized);
    while (length && normalized[length - 1] == '/') normalized[--length] = 0;
    arg = normalized;
    while (*arg == '/') ++arg;
    if (startsI(arg, "./")) arg += 2;
    if (startsI(arg, "../")) arg += 3;
    if (!*arg || iequals(arg, ".") || iequals(arg, "..")) {
        root = true;
        kind = FTP_BASE;
        return true;
    }

    if (iequals(arg, "base")) {
        root = false;
        kind = FTP_BASE;
        return true;
    }
    if (iequals(arg, "update") || iequals(arg, "updates")) {
        root = false;
        kind = FTP_UPDATE;
        return true;
    }
    if (iequals(arg, "dlc") || iequals(arg, "dlcs")) {
        root = false;
        kind = FTP_DLC;
        return true;
    }
    return false;
}

static bool resolveTarget(const char* arg, int cwdKind, bool cwdRoot,
                          int& kind, const char*& name) {
    if (!arg || !*arg) return false;
    const char* slash = strrchr(arg, '/');
    name = slash ? slash + 1 : arg;
    kind = cwdRoot ? kindFromName(name) : cwdKind;
    if (slash) {
        char directory[LINE_CAP];
        size_t length = size_t(slash - arg);
        if (length >= sizeof(directory)) return false;
        memcpy(directory, arg, length); directory[length] = 0;
        bool root = cwdRoot;
        if (iequals(directory, ".") && !cwdRoot) kind = cwdKind;
        else if (!parseVirtualDir(directory, kind, root)) return false;
        else if (root) kind = kindFromName(name);
    }
    return safeName(name);
}

static void closeSocket(int32_t& fd) {
    if (fd < 0) return;
    int32_t owned = fd;
    fd = -1;
    sceNetSocketClose(owned);
}

static bool sendAll(int32_t fd, const char* text) {
    if (fd < 0 || !text) return false;
    const char* p = text;
    size_t left = strlen(text);

    uint64_t progressed = sceKernelGetProcessTime();
    while (left && !stopped()) {
        int32_t n = sceNetSend(fd, p, left, 0);
        int error = n < 0 ? netErrno() : 0;
        if (n < 0 && retryable(n, error) && !expired(progressed, DATA_TIMEOUT)) {
            sceKernelUsleep(2000); continue;
        }
        if (n <= 0 || size_t(n) > left) return false;
        p += n;
        left -= size_t(n);
        progressed = sceKernelGetProcessTime();
    }
    return left == 0;
}

static bool recvLine(int32_t fd, char* out, size_t cap) {
    if (!out || cap < 2) return false;
    size_t used = 0;
    uint64_t progressed = sceKernelGetProcessTime();

    while (!stopped()) {
        char c = 0;
        int32_t n = sceNetRecv(fd, &c, 1, 0);
        int error = n < 0 ? netErrno() : 0;
        if (n < 0 && retryable(n, error) && !expired(progressed, CONTROL_TIMEOUT)) {
            sceKernelUsleep(2000); continue;
        }
        if (n <= 0) return false;
        progressed = sceKernelGetProcessTime();

        if (c == '\n') {
            out[used] = 0;
            return true;
        }
        if (c == '\r') continue;

        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc == 127 || used + 1 >= cap) return false;
        out[used++] = c;
    }
    return false;
}

static int32_t makeListener(uint16_t port, const char* name) {
    int32_t fd = sceNetSocket(name, ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    if (fd < 0) return networkError(fd, netErrno());
    int32_t rc = configureSocket(fd, true);
    if (rc != 0) { sceNetSocketClose(fd); return rc; }

    SocketAddress address = {};
    address.length = sizeof(address);
    address.family = ORBIS_NET_AF_INET;
    address.port = sceNetHtons(port);
    address.address = sceNetHtonl(0);

    rc = sceNetBind(
        fd, reinterpret_cast<OrbisNetSockaddr*>(&address), sizeof(address));
    if (rc != 0) {
        int32_t error = networkError(rc, netErrno());
        sceNetSocketClose(fd);
        return error;
    }

    rc = sceNetListen(fd, 4);
    if (rc != 0) {
        int32_t error = networkError(rc, netErrno());
        sceNetSocketClose(fd);
        return error;
    }
    return fd;
}

static bool updateIpFromSocket(int32_t fd) {
    SocketAddress local = {};
    OrbisNetSocklen_t len = sizeof(local);

    if (sceNetGetsockname(fd, reinterpret_cast<OrbisNetSockaddr*>(&local), &len) != 0 ||
        len != sizeof(local) || local.family != ORBIS_NET_AF_INET ||
        local.address == 0) {
        return false;
    }

    uint32_t host = sceNetNtohl(local.address);
    unsigned a = (host >> 24) & 255;
    unsigned b = (host >> 16) & 255;
    unsigned c = (host >> 8) & 255;
    unsigned d = host & 255;
    lockSnapshot();
    snprintf(g.ip, sizeof(g.ip), "%u.%u.%u.%u", a, b, c, d);
    unlockSnapshot();
    return true;
}

static bool passiveAddress(unsigned& a, unsigned& b, unsigned& c, unsigned& d) {
    a = b = c = d = 0;
    if (sscanf(g.ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;

    return a <= 255 && b <= 255 && c <= 255 && d <= 255 &&
           !(a == 0 && b == 0 && c == 0 && d == 0) &&
           !(a == 127 && b == 0 && c == 0 && d == 1);
}

static int32_t openPassive(uint16_t& chosen) {
    closeSocket(g_dataListener);

    for (uint16_t port = PASSIVE_FIRST; port <= PASSIVE_LAST; ++port) {
        int32_t fd = makeListener(port, "peppy-ftp-data");
        if (fd >= 0) {
            g_dataListener = fd;
            chosen = port;
            return 0;
        }
    }
    return int32_t(0x80410130u);
}

static int32_t acceptPassive(uint32_t expectedPeer) {
    if (g_dataListener < 0) return -1;

    int32_t fd = -1;
    uint64_t began = sceKernelGetProcessTime();
    while (!stopped() && !expired(began, DATA_TIMEOUT)) {
        SocketAddress peer = {};
        OrbisNetSocklen_t len = sizeof(peer);
        fd = sceNetAccept(g_dataListener, reinterpret_cast<OrbisNetSockaddr*>(&peer), &len);
        int error = fd < 0 ? netErrno() : 0;
        if (fd >= 0) {
            if (len == sizeof(peer) && peer.family == ORBIS_NET_AF_INET &&
                peer.address == expectedPeer && configureSocket(fd, false) == 0) break;
            sceNetSocketClose(fd); fd = -1;
        } else if (!retryable(fd, error)) { publishError(networkError(fd, error)); break; }
        sceKernelUsleep(2000);
    }
    if (fd < 0 && !stopped() && expired(began, DATA_TIMEOUT)) publishError(ERROR_TIMEOUT);
    closeSocket(g_dataListener);
    if (fd >= 0) g_data = fd;
    return fd;
}

static void ensureInbox() {
    mkdir("/data/peppy-store", 0777);
    mkdir("/data/peppy-store/inbox", 0777);
    mkdir("/data/peppy-store/inbox/base", 0777);
    mkdir("/data/peppy-store/inbox/update", 0777);
    mkdir("/data/peppy-store/inbox/dlc", 0777);
}

static bool buildPaths(int kind, const char* name,
                       char* finalPath, size_t finalCap,
                       char* tempPath, size_t tempCap) {
    if (!safeName(name)) return false;

    const char* folder = kindName(kind);
    int n1 = snprintf(finalPath, finalCap,
                      "/data/peppy-store/inbox/%s/%s", folder, name);
    int n2 = snprintf(tempPath, tempCap,
                      "/data/peppy-store/inbox/%s/.%s.part", folder, name);

    return n1 > 0 && n2 > 0 &&
           size_t(n1) < finalCap && size_t(n2) < tempCap;
}

static bool receivePackage(int32_t dataFd, int kind, const char* name,
                           uint64_t& bytes) {
    char finalPath[256];
    char tempPath[272];

    if (!buildPaths(kind, name, finalPath, sizeof(finalPath),
                    tempPath, sizeof(tempPath))) {
        publishError(ERROR_PACKAGE); return false;
    }

    ensureInbox();
    remove(tempPath);

    int fileFd = open(tempPath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fileFd < 0) { publishError(-errno); return false; }

    bool ok = true;
    bytes = 0;
    unsigned char header[USER_PACKAGE_HEADER_BYTES] = {};
    size_t headerBytes = 0;
    uint64_t expectedBytes = 0, progressed = sceKernelGetProcessTime();
    while (!stopped()) {
        int32_t n = sceNetRecv(dataFd, g_ioBuffer, sizeof(g_ioBuffer), 0);
        int error = n < 0 ? netErrno() : 0;
        if (n == 0) break;
        if (n < 0 && retryable(n, error) && !expired(progressed, DATA_TIMEOUT)) {
            sceKernelUsleep(2000); continue;
        }
        if (n < 0 || size_t(n) > sizeof(g_ioBuffer)) {
            publishError(n < 0 && retryable(n, error) ? ERROR_TIMEOUT : networkError(n, error));
            ok = false;
            break;
        }
        progressed = sceKernelGetProcessTime();
        if (headerBytes < sizeof(header)) {
            size_t take = sizeof(header) - headerBytes;
            if (take > size_t(n)) take = size_t(n);
            memcpy(header + headerBytes, g_ioBuffer, take); headerBytes += take;
            if (headerBytes == sizeof(header)) {
                expectedBytes = userPackageBe64(header + 0x430);
                if (memcmp(header, "\x7f" "CNT", 4) || expectedBytes < sizeof(header) ||
                    expectedBytes > PEPPY_MAX_PACKAGE_BYTES) { publishError(ERROR_PACKAGE); ok = false; break; }
            }
        }
        if (bytes > PEPPY_MAX_PACKAGE_BYTES - uint64_t(n) ||
            (expectedBytes && bytes + uint64_t(n) > expectedBytes)) {
            publishError(ERROR_PACKAGE); ok = false; break;
        }

        size_t offset = 0;
        while (offset < size_t(n)) {
            ssize_t written = write(fileFd, g_ioBuffer + offset, size_t(n) - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                publishError(written < 0 ? -errno : -EIO);
                ok = false;
                break;
            }
            offset += size_t(written);
        }
        if (!ok) break;
        bytes += uint64_t(n);
    }

    // An orderly EOF can also be an interrupted upload. Never publish a
    // truncated, oversized or empty .pkg as ready for installation.
    if (ok && (!expectedBytes || bytes != expectedBytes)) { publishError(ERROR_PACKAGE); ok = false; }
    if (ok && !stopped() && fsync(fileFd) != 0) { publishError(-errno); ok = false; }
    if (close(fileFd) != 0) { publishError(-errno); ok = false; }
    if (stopped()) ok = false;

    if (ok) {
        // rename atomically replaces an old upload only after the new PKG is
        // complete; failed transfers leave the previous file intact.
        if (rename(tempPath, finalPath) != 0) { publishError(-errno); ok = false; }
    }

    if (!ok) remove(tempPath);
    return ok;
}

static bool inboxFileSize(const char* path, uint64_t& bytes) {
    bytes = 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;

    PeppyFileInfo info = {};
    int rc = peppyFtpFstat(fd, &info);
    int closeRc = close(fd);
    if (rc != 0 || closeRc != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0)
        return false;

    bytes = uint64_t(info.st_size);
    return true;
}

static int appendInboxKind(FtpInboxItem* items, int count, int capacity, int kind) {
    if (!items || capacity <= 0 || count >= capacity) return count;

    char directory[160];
    int length = snprintf(directory, sizeof(directory),
                          "/data/peppy-store/inbox/%s", kindName(kind));
    if (length <= 0 || size_t(length) >= sizeof(directory)) return count;

    DIR* dir = opendir(directory);
    if (!dir) return count;

    while (count < capacity) {
        struct dirent* entry = readdir(dir);
        if (!entry) break;

        const char* name = entry->d_name;
        if (!safeName(name)) continue;

        char path[256];
        int used = snprintf(path, sizeof(path), "%s/%s", directory, name);
        if (used <= 0 || size_t(used) >= sizeof(path)) continue;

        uint64_t bytes = 0;
        if (!inboxFileSize(path, bytes)) continue;

        FtpInboxItem& item = items[count++];
        item.kind = kind;
        item.bytes = bytes;
        memcpy(item.name, name, strlen(name) + 1); // safeName bounded the complete UTF-8 name.
        snprintf(item.path, sizeof(item.path), "%s", path);
    }

    closedir(dir);
    return count;
}

static void sortInbox(FtpInboxItem* items, int count) {
    if (!items || count <= 1) return;

    for (int i = 1; i < count; ++i) {
        FtpInboxItem value = items[i];
        int j = i - 1;
        while (j >= 0) {
            bool after = items[j].kind > value.kind ||
                         (items[j].kind == value.kind &&
                          strcmp(items[j].name, value.name) > 0);
            if (!after) break;
            items[j + 1] = items[j];
            --j;
        }
        items[j + 1] = value;
    }
}

enum ListingFormat { LISTING_UNIX, LISTING_NAMES, LISTING_FACTS };
static bool sendListingEntry(int32_t dataFd, const char* name, uint64_t bytes,
                             bool directory, ListingFormat format) {
    char line[384];
    if (format == LISTING_NAMES) snprintf(line, sizeof(line), "%s\r\n", name);
    else if (format == LISTING_FACTS)
        snprintf(line, sizeof(line), "type=%s;size=%llu;perm=%s; %s\r\n",
                 directory ? "dir" : "file", (unsigned long long)bytes,
                 directory ? "el" : "w", name);
    else snprintf(line, sizeof(line), "%s 1 peppy peppy %llu Jan 01 00:00 %s\r\n",
                  directory ? "drwxr-xr-x" : "-rw-r--r--",
                  (unsigned long long)bytes, name);
    return sendAll(dataFd, line);
}
static bool sendInboxListing(int32_t dataFd, int kind, ListingFormat format) {
    char directory[160];
    snprintf(directory, sizeof(directory), "/data/peppy-store/inbox/%s", kindName(kind));
    DIR* dir = opendir(directory);
    if (!dir) return false;
    bool ok = true;
    while (!stopped()) {
        struct dirent* entry = readdir(dir);
        if (!entry) break;
        if (!safeName(entry->d_name)) continue;
        char path[256], temporary[272];
        if (!buildPaths(kind, entry->d_name, path, sizeof(path), temporary, sizeof(temporary))) continue;
        uint64_t bytes = 0;
        if (!inboxFileSize(path, bytes)) continue;
        if (!sendListingEntry(dataFd, entry->d_name, bytes, false, format)) { ok = false; break; }
    }
    closedir(dir);
    return ok && !stopped();
}

static void refreshConfiguredIp() {
    OrbisNetCtlInfo info = {};
    lockSnapshot();
    g.ip[0] = 0;
    if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info) == 0 &&
        info.ip_address[0]) {
        snprintf(g.ip, sizeof(g.ip), "%s", info.ip_address);
    }
    unlockSnapshot();
}

static void closeSessionSockets() {
    closeSocket(g_data);
    closeSocket(g_dataListener);
    closeSocket(g_control);
}

static bool openDataChannel(int32_t control, uint32_t peer, int32_t& dataFd) {
    if (g_dataListener < 0) {
        sendAll(control, "425 Use PASV or EPSV first\r\n");
        return false;
    }

    if (!sendAll(control, "150 Opening data connection\r\n")) {
        closeSocket(g_dataListener); return false;
    }
    dataFd = acceptPassive(peer);

    if (dataFd < 0) {
        sendAll(control, "425 Data connection failed\r\n");
        return false;
    }
    return true;
}

static void* worker(void*) {
    while (!stopped()) {
        SocketAddress peer = {};
        OrbisNetSocklen_t peerLen = sizeof(peer);
        int32_t control = sceNetAccept(
            g_listener, reinterpret_cast<OrbisNetSockaddr*>(&peer), &peerLen);
        int error = control < 0 ? netErrno() : 0;
        if (control < 0) {
            if (!retryable(control, error)) publishError(networkError(control, error));
            if (!stopped()) sceKernelUsleep(2000);
            continue;
        }
        if (configureSocket(control, false) != 0 || peerLen != sizeof(peer) ||
            peer.family != ORBIS_NET_AF_INET) { sceNetSocketClose(control); continue; }
        g_control = control;
        updateIpFromSocket(control);
        if (!sendAll(control, "220 Peppy Store FTP ready\r\n")) { closeSessionSockets(); continue; }

        bool root = true, loggedIn = false;
        int cwdKind = FTP_BASE;
        char line[LINE_CAP];
        while (!stopped() && recvLine(control, line, sizeof(line))) {
            char* arg = strchr(line, ' ');
            if (arg) { *arg++ = 0; while (*arg == ' ') ++arg; }
            else arg = line + strlen(line);
            if (iequals(line, "USER")) {
                loggedIn = false;
                sendAll(control, "331 Password optional\r\n");
            } else if (iequals(line, "PASS")) {
                loggedIn = true;
                sendAll(control, "230 Logged in\r\n");
            } else if (iequals(line, "SYST")) {
                sendAll(control, "215 UNIX Type: L8\r\n");
            } else if (iequals(line, "FEAT")) {
                sendAll(control, "211-Features\r\n UTF8\r\n EPSV\r\n PASV\r\n SIZE\r\n MLST type*;size*;perm*;\r\n211 End\r\n");
            } else if (iequals(line, "OPTS") && iequals(arg, "UTF8 ON")) {
                sendAll(control, "200 UTF8 enabled\r\n");
            } else if (iequals(line, "CLNT")) {
                sendAll(control, "200 Client noted\r\n");
            } else if (iequals(line, "TYPE")) {
                sendAll(control, iequals(arg, "I") || iequals(arg, "A") ?
                        "200 Type set\r\n" : "504 Unsupported transfer type\r\n");
            } else if (iequals(line, "NOOP")) {
                sendAll(control, "200 OK\r\n");
            } else if (iequals(line, "QUIT")) {
                sendAll(control, "221 Bye\r\n"); break;
            } else if (!loggedIn) {
                sendAll(control, "530 Login with USER and PASS first\r\n");
            } else if (iequals(line, "PWD") || iequals(line, "XPWD")) {
                char reply[96];
                snprintf(reply, sizeof(reply), "257 \"/%s\"\r\n", root ? "" : kindName(cwdKind));
                sendAll(control, reply);
            } else if (iequals(line, "CWD") || iequals(line, "XCWD")) {
                int nextKind = cwdKind;
                bool nextRoot = root;
                if (iequals(arg, ".") || parseVirtualDir(arg, nextKind, nextRoot)) {
                    cwdKind = nextKind; root = nextRoot;
                    sendAll(control, "250 Directory changed\r\n");
                } else sendAll(control, "550 Directory unavailable\r\n");
            } else if (iequals(line, "CDUP")) {
                root = true; cwdKind = FTP_BASE;
                sendAll(control, "200 Directory changed\r\n");
            } else if (iequals(line, "PASV") || iequals(line, "EPSV")) {
                uint16_t port = 0;
                int32_t rc = openPassive(port);
                if (rc != 0) {
                    publishError(rc); sendAll(control, "425 Cannot open passive connection\r\n"); continue;
                }
                char reply[128];
                if (iequals(line, "EPSV")) {
                    if (*arg && !iequals(arg, "1")) {
                        closeSocket(g_dataListener); sendAll(control, "522 IPv4 EPSV only\r\n"); continue;
                    }
                    snprintf(reply, sizeof(reply), "229 Entering Extended Passive Mode (|||%u|)\r\n", unsigned(port));
                } else {
                    unsigned a, b, c, d;
                    if (!passiveAddress(a, b, c, d) &&
                        (!updateIpFromSocket(control) || !passiveAddress(a, b, c, d))) {
                        closeSocket(g_dataListener); sendAll(control, "425 Cannot determine console IP\r\n"); continue;
                    }
                    snprintf(reply, sizeof(reply), "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)\r\n",
                             a, b, c, d, unsigned(port / 256), unsigned(port % 256));
                }
                sendAll(control, reply);
            } else if (iequals(line, "STOR")) {
                const char* name = 0;
                int kind = cwdKind;
                if (!resolveTarget(arg, cwdKind, root, kind, name)) {
                    sendAll(control, "553 Invalid PKG filename or directory\r\n"); continue;
                }
                int32_t dataFd = -1;
                if (!openDataChannel(control, peer.address, dataFd)) continue;
                uint64_t bytes = 0;
                bool ok = receivePackage(dataFd, kind, name, bytes);
                closeSocket(g_data);
                if (ok) {
                    lockSnapshot();
                    ++g.filesReceived; g.bytesReceived += bytes; g.lastError = 0;
                    snprintf(g.lastFile, sizeof(g.lastFile), "%s", name);
                    snprintf(g.lastKind, sizeof(g.lastKind), "%s", kindName(kind));
                    unlockSnapshot();
                    sendAll(control, "226 Transfer complete\r\n");
                } else sendAll(control, "451 Transfer failed; PKG not published\r\n");
            } else if (iequals(line, "LIST") || iequals(line, "NLST") || iequals(line, "MLSD")) {
                int listingKind = cwdKind;
                bool listingRoot = root;
                const char* directory = arg;
                // Some clients send LIST -a/-la. Options affect neither this
                // virtual directory nor the hidden partial-upload policy.
                while (*directory == '-') {
                    const char* space = strchr(directory, ' ');
                    directory = space ? space + 1 : directory + strlen(directory);
                    while (*directory == ' ') ++directory;
                }
                if (*directory && !iequals(directory, ".") &&
                    !parseVirtualDir(directory, listingKind, listingRoot)) {
                    sendAll(control, "550 Directory unavailable\r\n"); continue;
                }
                int32_t dataFd = -1;
                if (!openDataChannel(control, peer.address, dataFd)) continue;
                ListingFormat format = iequals(line, "MLSD") ? LISTING_FACTS :
                                       iequals(line, "NLST") ? LISTING_NAMES : LISTING_UNIX;
                bool ok = true;
                if (listingRoot) {
                    for (int kind = FTP_BASE; kind <= FTP_DLC && ok; ++kind)
                        ok = sendListingEntry(dataFd, kindName(kind), 0, true, format);
                } else { ensureInbox(); ok = sendInboxListing(dataFd, listingKind, format); }
                closeSocket(g_data);
                sendAll(control, ok ? "226 Directory send OK\r\n" : "426 Directory transfer failed\r\n");
            } else if (iequals(line, "SIZE") || iequals(line, "MLST")) {
                int kind = cwdKind;
                const char* name = 0;
                char finalPath[256], tempPath[272], reply[384];
                uint64_t bytes = 0;
                if (!resolveTarget(arg, cwdKind, root, kind, name) ||
                    !buildPaths(kind, name, finalPath, sizeof(finalPath), tempPath, sizeof(tempPath)) ||
                    !inboxFileSize(finalPath, bytes)) {
                    sendAll(control, "550 PKG unavailable\r\n"); continue;
                }
                if (iequals(line, "SIZE")) snprintf(reply, sizeof(reply), "213 %llu\r\n", (unsigned long long)bytes);
                else snprintf(reply, sizeof(reply), "250-Listing\r\n type=file;size=%llu;perm=w; %s\r\n250 End\r\n",
                              (unsigned long long)bytes, name);
                sendAll(control, reply);
            } else if (iequals(line, "REST")) {
                sendAll(control, iequals(arg, "0") ? "350 Restart at zero accepted\r\n" :
                        "504 Resume is unavailable; upload the complete PKG\r\n");
            } else if (iequals(line, "ABOR")) {
                closeSocket(g_data); closeSocket(g_dataListener);
                sendAll(control, "226 No active transfer\r\n");
            } else sendAll(control, "502 Command not supported\r\n");
        }
        closeSessionSockets();
    }
    closeSessionSockets();
    return 0;
}

}

bool ftpReceiverStart() {
    if (ftpReceiverSnapshot().running) return true;

    lockSnapshot();
    g.lastError = 0;
    g.filesReceived = 0;
    g.bytesReceived = 0;
    g.lastFile[0] = 0;
    g.lastKind[0] = 0;
    g.port = 0;
    unlockSnapshot();

    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NETCTL);

    // Raw FTP sockets only require libnet itself. A separate libnet pool is
    // useful when available, but some real PS4 setups return resolver/internal
    // errors while raw LAN sockets still work. Do not make that optional pool
    // a hard startup dependency.
    sceNetInit();
    sceNetCtlInit();

    g_pool = sceNetPoolCreate("peppy-ftp", 1024 * 1024, 0);
    if (g_pool < 0) g_pool = -1;

    refreshConfiguredIp();
    uint16_t controlPort = CONTROL_PORT;
    g_listener = makeListener(CONTROL_PORT, "peppy-ftp-control");
    // GoldHEN's FTP commonly already owns 2121. Keep its server running and
    // choose a separate Peppy port; the native screen reports the actual one.
    if (uint32_t(g_listener) == (0x80410100u | NET_EADDRINUSE)) {
        for (uint16_t port = CONTROL_FALLBACK_FIRST; port <= CONTROL_FALLBACK_LAST; ++port) {
            g_listener = makeListener(port, "peppy-ftp-control");
            if (g_listener >= 0) { controlPort = port; break; }
            if (uint32_t(g_listener) != (0x80410100u | NET_EADDRINUSE)) break;
        }
    }
    if (g_listener < 0) {
        publishError(g_listener);
        if (g_pool >= 0) {
            sceNetPoolDestroy(g_pool);
            g_pool = -1;
        }
        return false;
    }

    __atomic_store_n(&g_stop, 0, __ATOMIC_RELEASE);
    int32_t rc = scePthreadCreate(&g_thread, 0, worker, 0, "peppy-ftp");
    if (rc != 0) {
        publishError(rc);
        closeSocket(g_listener);
        if (g_pool >= 0) {
            sceNetPoolDestroy(g_pool);
            g_pool = -1;
        }
        return false;
    }

    // Listener + worker are authoritative for FTP availability. Clear any
    // non-fatal setup warning once the server is actually running.
    lockSnapshot();
    g.lastError = 0;
    g.running = true;
    g.port = controlPort;
    unlockSnapshot();
    return true;
}

void ftpReceiverStop() {
    if (!ftpReceiverSnapshot().running) return;

    __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    // The worker owns session sockets until joined. Nonblocking bounded I/O
    // observes stop without closing descriptors underneath a live system call.
    int32_t rc = scePthreadJoin(g_thread, 0);
    if (rc != 0) { publishError(rc); return; }
    closeSessionSockets();
    closeSocket(g_listener);

    if (g_pool >= 0) {
        sceNetPoolDestroy(g_pool);
        g_pool = -1;
    }

    lockSnapshot(); g.running = false; unlockSnapshot();
}

FtpReceiverSnapshot ftpReceiverSnapshot() {
    lockSnapshot(); FtpReceiverSnapshot result = g; unlockSnapshot();
    return result;
}

int ftpInboxList(FtpInboxItem* items, int capacity) {
    if (!items || capacity <= 0) return 0;

    ensureInbox();
    int count = 0;
    count = appendInboxKind(items, count, capacity, FTP_BASE);
    count = appendInboxKind(items, count, capacity, FTP_UPDATE);
    count = appendInboxKind(items, count, capacity, FTP_DLC);
    sortInbox(items, count);
    return count;
}
