#include "ftp_receiver.h"

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
const uint16_t PASSIVE_FIRST = 2122;
const uint16_t PASSIVE_LAST = 2141;
const size_t LINE_CAP = 512;
const size_t IO_CAP = 256 * 1024;

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
static volatile int g_stop = 0;
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
    if (!s || !*s || strlen(s) >= 96 || strstr(s, "..") ||
        strchr(s, '/') || strchr(s, '\\')) return false;

    for (const unsigned char* p =
             reinterpret_cast<const unsigned char*>(s); *p; ++p) {
        if (*p < 32 || *p == 127 || *p == ':') return false;
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
    if (!arg || !*arg || iequals(arg, "/") || iequals(arg, "..")) {
        root = true;
        kind = FTP_BASE;
        return true;
    }

    while (*arg == '/') ++arg;

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

    while (left && !g_stop) {
        int32_t n = sceNetSend(fd, p, left, 0);
        if (n <= 0 || size_t(n) > left) return false;
        p += n;
        left -= size_t(n);
    }
    return left == 0;
}

static bool recvLine(int32_t fd, char* out, size_t cap) {
    if (!out || cap < 2) return false;
    size_t used = 0;

    while (!g_stop) {
        char c = 0;
        int32_t n = sceNetRecv(fd, &c, 1, 0);
        if (n <= 0) return false;

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
    if (fd < 0) return fd;

    SocketAddress address = {};
    address.length = sizeof(address);
    address.family = ORBIS_NET_AF_INET;
    address.port = sceNetHtons(port);
    address.address = sceNetHtonl(0);

    int32_t rc = sceNetBind(
        fd, reinterpret_cast<OrbisNetSockaddr*>(&address), sizeof(address));
    if (rc != 0) {
        sceNetSocketClose(fd);
        return rc;
    }

    rc = sceNetListen(fd, 4);
    if (rc != 0) {
        sceNetSocketClose(fd);
        return rc;
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
    snprintf(g.ip, sizeof(g.ip), "%u.%u.%u.%u", a, b, c, d);
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
    return -1;
}

static int32_t acceptPassive() {
    if (g_dataListener < 0) return -1;

    SocketAddress peer = {};
    OrbisNetSocklen_t len = sizeof(peer);
    int32_t fd = sceNetAccept(
        g_dataListener, reinterpret_cast<OrbisNetSockaddr*>(&peer), &len);

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
        return false;
    }

    ensureInbox();
    remove(tempPath);

    FILE* f = fopen(tempPath, "wb");
    if (!f) return false;

    bool ok = true;
    bytes = 0;
    while (!g_stop) {
        int32_t n = sceNetRecv(dataFd, g_ioBuffer, sizeof(g_ioBuffer), 0);
        if (n == 0) break;
        if (n < 0 || size_t(n) > sizeof(g_ioBuffer)) {
            ok = false;
            break;
        }
        if (fwrite(g_ioBuffer, 1, size_t(n), f) != size_t(n)) {
            ok = false;
            break;
        }
        bytes += uint64_t(n);
    }

    if (fflush(f) != 0) ok = false;
    int fileFd = fileno(f);
    if (fileFd >= 0 && fsync(fileFd) != 0) ok = false;
    if (fclose(f) != 0) ok = false;

    if (g_stop) ok = false;

    if (ok) {
        remove(finalPath);
        if (rename(tempPath, finalPath) != 0) ok = false;
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
        snprintf(item.name, sizeof(item.name), "%s", name);
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

static void sendInboxListing(int32_t dataFd, int kind) {
    FtpInboxItem items[64];
    int count = appendInboxKind(items, 0, 64, kind);
    sortInbox(items, count);

    for (int i = 0; i < count && !g_stop; ++i) {
        char line[320];
        snprintf(line, sizeof(line),
                 "-rw-r--r-- 1 peppy peppy %llu Jan 01 00:00 %s\r\n",
                 (unsigned long long)items[i].bytes, items[i].name);
        if (!sendAll(dataFd, line)) break;
    }
}

static void refreshConfiguredIp() {
    g.ip[0] = 0;
    OrbisNetCtlInfo info = {};
    if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info) == 0 &&
        info.ip_address[0]) {
        snprintf(g.ip, sizeof(g.ip), "%s", info.ip_address);
    }
}

static void closeSessionSockets() {
    closeSocket(g_data);
    closeSocket(g_dataListener);
    closeSocket(g_control);
}

static bool openDataChannel(int32_t control, int32_t& dataFd) {
    if (g_dataListener < 0) {
        sendAll(control, "425 Use PASV or EPSV first\r\n");
        return false;
    }

    sendAll(control, "150 Opening data connection\r\n");
    dataFd = acceptPassive();

    if (dataFd < 0) {
        sendAll(control, "425 Data connection failed\r\n");
        return false;
    }
    return true;
}

static void* worker(void*) {
    while (!g_stop) {
        SocketAddress peer = {};
        OrbisNetSocklen_t peerLen = sizeof(peer);

        int32_t control = sceNetAccept(
            g_listener, reinterpret_cast<OrbisNetSockaddr*>(&peer), &peerLen);

        if (control < 0) {
            if (!g_stop) sceKernelUsleep(20000);
            continue;
        }

        g_control = control;
        updateIpFromSocket(control);
        sendAll(control, "220 Peppy Store FTP ready\r\n");

        bool root = true;
        int cwdKind = FTP_BASE;
        bool loggedIn = false;
        char line[LINE_CAP];

        while (!g_stop && recvLine(control, line, sizeof(line))) {
            if (startsI(line, "USER")) {
                sendAll(control, "331 Password optional\r\n");
            } else if (startsI(line, "PASS")) {
                loggedIn = true;
                sendAll(control, "230 Logged in\r\n");
            } else if (iequals(line, "SYST")) {
                sendAll(control, "215 UNIX Type: L8\r\n");
            } else if (iequals(line, "FEAT")) {
                sendAll(control,
                        "211-Features\r\n"
                        " UTF8\r\n"
                        " EPSV\r\n"
                        " PASV\r\n"
                        "211 End\r\n");
            } else if (startsI(line, "OPTS UTF8")) {
                sendAll(control, "200 UTF8 enabled\r\n");
            } else if (startsI(line, "CLNT ")) {
                sendAll(control, "200 Client noted\r\n");
            } else if (startsI(line, "TYPE ")) {
                sendAll(control, "200 Type set\r\n");
            } else if (iequals(line, "NOOP")) {
                sendAll(control, "200 OK\r\n");
            } else if (iequals(line, "PWD") || iequals(line, "XPWD")) {
                char reply[96];
                if (root) {
                    snprintf(reply, sizeof(reply), "257 \"/\"\r\n");
                } else {
                    snprintf(reply, sizeof(reply), "257 \"/%s\"\r\n",
                             kindName(cwdKind));
                }
                sendAll(control, reply);
            } else if (startsI(line, "CWD ") || startsI(line, "XCWD ")) {
                const char* arg = strchr(line, ' ');
                if (arg) ++arg;

                int nextKind = cwdKind;
                bool nextRoot = root;

                if (parseVirtualDir(arg, nextKind, nextRoot)) {
                    cwdKind = nextKind;
                    root = nextRoot;
                    sendAll(control, "250 Directory changed\r\n");
                } else {
                    sendAll(control, "550 Directory unavailable\r\n");
                }
            } else if (iequals(line, "CDUP")) {
                root = true;
                cwdKind = FTP_BASE;
                sendAll(control, "200 Directory changed\r\n");
            } else if (iequals(line, "PASV")) {
                uint16_t port = 0;
                if (openPassive(port) != 0) {
                    sendAll(control, "425 Cannot open passive connection\r\n");
                    continue;
                }

                unsigned a, b, c, d;
                if (!passiveAddress(a, b, c, d)) {
                    if (!updateIpFromSocket(control) || !passiveAddress(a, b, c, d)) {
                        closeSocket(g_dataListener);
                        sendAll(control, "425 Cannot determine console IP\r\n");
                        continue;
                    }
                }

                char reply[128];
                snprintf(reply, sizeof(reply),
                         "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)\r\n",
                         a, b, c, d, unsigned(port / 256), unsigned(port % 256));
                sendAll(control, reply);
            } else if (iequals(line, "EPSV")) {
                uint16_t port = 0;
                if (openPassive(port) != 0) {
                    sendAll(control, "425 Cannot open passive connection\r\n");
                    continue;
                }

                char reply[96];
                snprintf(reply, sizeof(reply),
                         "229 Entering Extended Passive Mode (|||%u|)\r\n",
                         unsigned(port));
                sendAll(control, reply);
            } else if (startsI(line, "STOR ")) {
                if (!loggedIn) {
                    sendAll(control, "530 Login with USER and PASS first\r\n");
                    continue;
                }

                const char* name = line + 5;
                if (!safeName(name)) {
                    sendAll(control, "553 Invalid PKG filename\r\n");
                    continue;
                }

                int32_t dataFd = -1;
                if (!openDataChannel(control, dataFd)) continue;

                int kind = root ? kindFromName(name) : cwdKind;
                uint64_t bytes = 0;
                bool ok = receivePackage(dataFd, kind, name, bytes);

                closeSocket(g_data);

                if (ok) {
                    ++g.filesReceived;
                    g.bytesReceived += bytes;
                    g.lastError = 0;
                    snprintf(g.lastFile, sizeof(g.lastFile), "%s", name);
                    snprintf(g.lastKind, sizeof(g.lastKind), "%s", kindName(kind));
                    sendAll(control, "226 Transfer complete\r\n");
                } else {
                    g.lastError = -1;
                    sendAll(control, "451 Transfer failed\r\n");
                }
            } else if (startsI(line, "LIST") ||
                       startsI(line, "NLST") ||
                       startsI(line, "MLSD")) {
                int32_t dataFd = -1;
                if (!openDataChannel(control, dataFd)) continue;

                if (root) {
                    sendAll(dataFd,
                            "drwxr-xr-x 1 peppy peppy 0 Jan 01 00:00 base\r\n"
                            "drwxr-xr-x 1 peppy peppy 0 Jan 01 00:00 update\r\n"
                            "drwxr-xr-x 1 peppy peppy 0 Jan 01 00:00 dlc\r\n");
                } else {
                    sendInboxListing(dataFd, cwdKind);
                }

                closeSocket(g_data);
                sendAll(control, "226 Directory send OK\r\n");
            } else if (iequals(line, "QUIT")) {
                sendAll(control, "221 Bye\r\n");
                break;
            } else {
                sendAll(control, "502 Command not supported\r\n");
            }
        }

        closeSessionSockets();
    }

    return 0;
}

}

bool ftpReceiverStart() {
    if (g.running) return true;

    g.lastError = 0;
    g.filesReceived = 0;
    g.bytesReceived = 0;
    g.lastFile[0] = 0;
    g.lastKind[0] = 0;

    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NETCTL);

    sceNetInit();
    sceNetCtlInit();

    g_pool = sceNetPoolCreate("peppy-ftp", 1024 * 1024, 0);
    if (g_pool < 0) {
        g.lastError = g_pool;
        return false;
    }

    refreshConfiguredIp();
    g_listener = makeListener(CONTROL_PORT, "peppy-ftp-control");
    if (g_listener < 0) {
        g.lastError = g_listener;
        sceNetPoolDestroy(g_pool);
        g_pool = -1;
        return false;
    }

    g_stop = 0;
    int32_t rc = scePthreadCreate(&g_thread, 0, worker, 0, "peppy-ftp");
    if (rc != 0) {
        g.lastError = rc;
        closeSocket(g_listener);
        sceNetPoolDestroy(g_pool);
        g_pool = -1;
        return false;
    }

    g.running = true;
    g.port = CONTROL_PORT;
    return true;
}

void ftpReceiverStop() {
    if (!g.running) return;

    g_stop = 1;
    closeSessionSockets();
    closeSocket(g_listener);
    scePthreadJoin(g_thread, 0);

    if (g_pool >= 0) {
        sceNetPoolDestroy(g_pool);
        g_pool = -1;
    }

    g.running = false;
}

FtpReceiverSnapshot ftpReceiverSnapshot() {
    return g;
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
