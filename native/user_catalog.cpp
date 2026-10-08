#include "user_catalog.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// OpenOrbis musl forwards fstat to the PS4 export without adapting struct stat,
// and its PS4 fstatat/lstat returns ENOSYS. The kernel writes this 120-byte
// layout (16-bit mode/nlink and size at offset 72), not musl's size-at-80 layout.
// Keep native metadata explicit; host tests map their POSIX metadata separately.
// https://github.com/OpenOrbis/musl/blob/master/src/stat/fstat.c
// https://github.com/OpenOrbis/musl/blob/master/src/stat/fstatat.c
struct PeppyUserCatalogFileInfo {
    uint32_t device, inode;
    uint16_t mode, links;
    uint32_t uid, gid, rdev;
    int64_t accessTime[2], modificationTime[2], changeTime[2];
    int64_t sizeBytes, blocks;
    int32_t blockSize;
    uint32_t flags, generation;
    int32_t spare;
    int64_t birthTime[2];
};
static_assert(sizeof(PeppyUserCatalogFileInfo) == 120 &&
              offsetof(PeppyUserCatalogFileInfo, mode) == 8 &&
              offsetof(PeppyUserCatalogFileInfo, links) == 10 &&
              offsetof(PeppyUserCatalogFileInfo, sizeBytes) == 72 &&
              offsetof(PeppyUserCatalogFileInfo, blocks) == 80,
              "PS4 user catalog native file metadata ABI");
#if defined(__FreeBSD__) || defined(PS4) || defined(PEPPY_USER_CATALOG_NATIVE_FILE_ABI)
extern "C" int32_t peppyCatalogNativeFstat(int32_t, PeppyUserCatalogFileInfo*) __asm__("sceKernelFstat");
#endif
#if defined(__FreeBSD__) || defined(PS4)
static_assert(O_NOFOLLOW == 0x100 && O_NONBLOCK == 0x4, "PS4 catalog open flags ABI");
#endif

namespace {
const size_t MAX_TABLE_BYTES = 32768;
const size_t MAX_SFO_BYTES = 65536;
const size_t MAX_FILE_BYTES = USER_CATALOG_MAX_ITEMS * 3700 + 24;

int fileInfo(int descriptor, PeppyUserCatalogFileInfo& info) {
    memset(&info, 0, sizeof(info));
#if defined(__FreeBSD__) || defined(PS4) || defined(PEPPY_USER_CATALOG_NATIVE_FILE_ABI)
    return peppyCatalogNativeFstat(descriptor, &info) == 0 ? 0 : -1;
#else
    struct stat host;
    if (fstat(descriptor, &host) != 0) return -1;
    info.device = (uint32_t)host.st_dev; info.inode = (uint32_t)host.st_ino;
    info.mode = (uint16_t)host.st_mode; info.links = (uint16_t)host.st_nlink;
    info.uid = (uint32_t)host.st_uid; info.gid = (uint32_t)host.st_gid; info.rdev = (uint32_t)host.st_rdev;
    info.accessTime[0] = host.st_atim.tv_sec; info.accessTime[1] = host.st_atim.tv_nsec;
    info.modificationTime[0] = host.st_mtim.tv_sec; info.modificationTime[1] = host.st_mtim.tv_nsec;
    info.changeTime[0] = host.st_ctim.tv_sec; info.changeTime[1] = host.st_ctim.tv_nsec;
    info.sizeBytes = host.st_size; info.blocks = host.st_blocks; info.blockSize = (int32_t)host.st_blksize;
    return 0;
#endif
}

size_t boundedLength(const char* p, size_t limit) {
    if (!p) return limit;
    size_t n = 0;
    while (n < limit && p[n]) ++n;
    return n;
}
bool lowerEqual(const char* a, size_t n, const char* b) {
    if (strlen(b) != n) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool urlParts(const char* url, const char*& host, size_t& hostBytes, const char*& path) {
    size_t length = boundedLength(url, USER_CATALOG_MAX_URL_BYTES + 1);
    if (length < 12 || length > USER_CATALOG_MAX_URL_BYTES || strncmp(url, "https://", 8)) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)url[i];
        if (ch <= 32 || ch >= 127 || ch == '\\' || ch == '#' || ch == '"' ||
            ch == '<' || ch == '>' || ch == '`') return false;
        if (ch == '%') {
            if (i + 2 >= length) return false;
            int a = hexDigit(url[i + 1]), b = hexDigit(url[i + 2]);
            if (a < 0 || b < 0) return false;
            int value = a * 16 + b;
            if (value < 32 || value == 127 || value == '\\' || value == '/') return false;
            i += 2;
        }
    }
    host = url + 8;
    path = strchr(host, '/');
    if (!path || !path[1] || path[1] == '?') return false;
    const char* end = path;
    // An explicit HTTPS default port is accepted; other ports/IPv6 literals
    // and credentials are refused. DNS resolution is checked by the adapter.
    const char* colon = (const char*)memchr(host, ':', size_t(end - host));
    if (colon) {
        if (size_t(end - colon) != 4 || memcmp(colon, ":443", 4)) return false;
        end = colon;
    }
    hostBytes = size_t(end - host);
    if (hostBytes > 253 || hostBytes < 3 || host[0] == '.' || end[-1] == '.') return false;
    const char* label = host;
    bool dotted = false, letterInLast = false;
    for (const char* p = host; p <= end; ++p) {
        if (p == end || *p == '.') {
            size_t n = size_t(p - label);
            if (!n || n > 63 || label[0] == '-' || p[-1] == '-') return false;
            if (p == end) {
                for (size_t j = 0; j < n; ++j) {
                    char c = label[j];
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) letterInLast = true;
                }
                const char* local[] = { "local", "localhost", "internal", "lan", "home", "test", "invalid", "example" };
                for (size_t j = 0; j < sizeof(local) / sizeof(local[0]); ++j)
                    if (lowerEqual(label, n, local[j])) return false;
            } else dotted = true;
            label = p + 1;
        } else {
            unsigned char c = (unsigned char)*p;
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-')) return false;
        }
    }
    if (!dotted || !letterInLast) return false; // Includes numeric/hex IPv4 forms.
    // Refuse dot path segments, including percent-encoded dots. Their server
    // normalization must not let an apparent author URL escape its prefix.
    const char* segment = path + 1;
    for (const char* p = segment;; ++p) {
        if (*p == '/' || *p == '?' || !*p) {
            unsigned int dots = 0;
            bool onlyDots = true;
            for (const char* q = segment; q < p; ++q) {
                if (*q == '.') ++dots;
                else if (*q == '%' && p - q >= 3 && hexDigit(q[1]) == 2 && hexDigit(q[2]) == 14) {
                    ++dots; q += 2;
                } else onlyDots = false;
            }
            if (onlyDots && (dots == 1 || dots == 2)) return false;
            if (*p == '?' || !*p) break;
            segment = p + 1;
        }
    }
    return true;
}
uint16_t le16(const unsigned char* p) { return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8); }
uint32_t le32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void put16(unsigned char* p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
void put32(unsigned char* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
void put64(unsigned char* p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
uint64_t le64(const unsigned char* p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }
uint32_t checksum(const unsigned char* p, size_t n) {
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < n; ++i) hash = (hash ^ p[i]) * 16777619U;
    return hash; // Corruption detection, never authentication.
}
bool span(size_t offset, size_t bytes, size_t total) { return offset <= total && bytes <= total - offset; }
bool displayUtf8(const char* text, size_t bytes, bool multiline = false) {
    if (!text || !bytes) return false;
    for (size_t i = 0; i < bytes;) {
        unsigned char first = (unsigned char)text[i++];
        if (first < 0x80) {
            if ((first < 32 && !(multiline && (first == '\n' || first == '\r' || first == '\t'))) || first == 127) return false;
            continue;
        }
        int extra = first >= 0xC2 && first <= 0xDF ? 1 :
                    (first >= 0xE0 && first <= 0xEF ? 2 : (first >= 0xF0 && first <= 0xF4 ? 3 : 0));
        if (!extra || size_t(extra) > bytes - i) return false;
        uint32_t code = first & (extra == 1 ? 31U : (extra == 2 ? 15U : 7U));
        for (int j = 0; j < extra; ++j) {
            unsigned char c = (unsigned char)text[i++];
            if ((c & 0xC0) != 0x80) return false;
            code = (code << 6) | (c & 63U);
        }
        if ((extra == 1 && code < 0x80) || (extra == 2 && code < 0x800) ||
            (extra == 3 && code < 0x10000) || code > 0x10FFFF ||
            (code >= 0xD800 && code <= 0xDFFF) || (code >= 0x80 && code <= 0x9F) ||
            (code >= 0x202A && code <= 0x202E) || (code >= 0x2066 && code <= 0x2069)) return false;
    }
    return true;
}
bool sfoVersionText(const char* text, size_t n) {
    if (!n || n > 16) return false;
    bool dot = false;
    for (size_t i = 0; i < n; ++i) {
        if (text[i] == '.') { if (dot || !i || i + 1 == n) return false; dot = true; }
        else if (text[i] < '0' || text[i] > '9') return false;
    }
    return dot;
}
void makeFilename(UserCatalogEntry& entry) {
    const char* kind = entry.kind == USER_PACKAGE_BASE ? "base" : (entry.kind == USER_PACKAGE_UPDATE ? "update" : "dlc");
    char versionSegment[24] = {};
    const char* version = "unknown";
    if (entry.versionKnown) {
        size_t n = boundedLength(entry.version, sizeof(entry.version));
        if (n < sizeof(entry.version) && sfoVersionText(entry.version, n)) version = entry.version;
        else {
            // Human release labels may include UTF-8, spaces, or slashes. They
            // never become pathname fragments. Duplicate identity still uses
            // the complete original label, not this deterministic filename.
            uint64_t hash = 14695981039346656037ULL;
            for (size_t i = 0; i < n; ++i) hash = (hash ^ (unsigned char)entry.version[i]) * 1099511628211ULL;
            snprintf(versionSegment, sizeof(versionSegment), "v%08x%08x", (unsigned int)(hash >> 32), (unsigned int)hash);
            version = versionSegment;
        }
    }
    snprintf(entry.filename, sizeof(entry.filename), "%s-%s-%s.pkg", entry.contentId, kind,
             version);
}
bool validEntry(const UserCatalogEntry& entry) {
    if (!userCatalogPublicHttpsUrl(entry.url) || !userPackageCanonicalContentId(entry.contentId) ||
        !strncmp(entry.contentId + 7, "PPSA", 4) || !strncmp(entry.contentId + 7, "BREW00001", 9) ||
        entry.sizeBytes < USER_PACKAGE_HEADER_BYTES || entry.sizeBytes > PEPPY_MAX_PACKAGE_BYTES ||
        memcmp(entry.titleId, entry.contentId + 7, 9) || entry.titleId[9] ||
        entry.source != userCatalogSourceForUrl(entry.url)) return false;
    size_t nameBytes = boundedLength(entry.name, sizeof(entry.name));
    size_t versionBytes = boundedLength(entry.version, sizeof(entry.version));
    if (nameBytes >= sizeof(entry.name) || !displayUtf8(entry.name, nameBytes) ||
        versionBytes >= sizeof(entry.version) || (entry.versionKnown ? !displayUtf8(entry.version, versionBytes) : versionBytes != 0)) return false;
    const char* prose[] = { entry.description, entry.requiresData };
    for (size_t i = 0; i < 2; ++i) {
        size_t n = boundedLength(prose[i], 601);
        if (n == 601 || (n && !displayUtf8(prose[i], n, true))) return false;
    }
    size_t hashBytes = boundedLength(entry.sha256, sizeof(entry.sha256));
    if (hashBytes && hashBytes != 64) return false;
    for (size_t i = 0; i < hashBytes; ++i) if (hexDigit(entry.sha256[i]) < 0) return false;
    if (entry.displayCategory < 0 || entry.displayCategory > 7 ||
        ((entry.displayCategory == 1 || entry.displayCategory == 2 || entry.displayCategory == 3 || entry.displayCategory == 4) && entry.kind != USER_PACKAGE_BASE) ||
        (entry.displayCategory == 5 && entry.kind != USER_PACKAGE_UPDATE) ||
        (entry.displayCategory == 6 && (entry.kind != USER_PACKAGE_DLC || entry.isTheme)) ||
        (entry.displayCategory == 7 && !entry.isTheme)) return false;
    unsigned char header[USER_PACKAGE_HEADER_BYTES] = {};
    memcpy(header, "\x7f" "CNT", 4);
    memcpy(header + 0x40, entry.contentId, 36);
    for (int i = 0; i < 4; ++i) {
        header[0x74 + i] = (unsigned char)(entry.contentType >> (24 - 8 * i));
        header[0x78 + i] = (unsigned char)(entry.contentFlags >> (24 - 8 * i));
        header[0x98 + i] = (unsigned char)(entry.iroTag >> (24 - 8 * i));
    }
    if (userPackageKind(header, sizeof(header)) != entry.kind ||
        entry.isTheme != userPackageIsTheme(header, sizeof(header))) return false;
    UserCatalogEntry canonical = entry;
    makeFilename(canonical);
    return boundedLength(entry.filename, sizeof(entry.filename)) < sizeof(entry.filename) &&
           !strcmp(canonical.filename, entry.filename);
}
bool duplicate(const UserCatalogEntry& a, const UserCatalogEntry& b) {
    if (!strcmp(a.url, b.url) || !strcmp(a.filename, b.filename)) return true;
    if (a.kind != b.kind || strcmp(a.contentId, b.contentId)) return false;
    // Unknown versions cannot safely be distinguished from known revisions.
    return !a.versionKnown || !b.versionKnown || !strcmp(a.version, b.version);
}
int rangeRead(const char* url, UserCatalogRangeReader reader, void* context,
              uint64_t offset, size_t bytes, unsigned char* output, uint64_t& total) {
    UserCatalogRangeInfo info = {};
    if (!reader(context, url, offset, bytes, output, &info)) return USER_CATALOG_ERROR_NETWORK;
    if (info.received != bytes || info.totalBytes < USER_PACKAGE_HEADER_BYTES ||
        info.totalBytes > PEPPY_MAX_PACKAGE_BYTES || offset > info.totalBytes || bytes > info.totalBytes - offset ||
        (total && info.totalBytes != total) || !userCatalogPublicHttpsUrl(info.effectiveUrl)) return USER_CATALOG_ERROR_RANGE;
    total = info.totalBytes;
    return USER_CATALOG_OK;
}
int parseSfo(const unsigned char* input, size_t bytes, UserCatalogEntry& entry) {
    size_t wrapper = bytes >= 4 && !memcmp(input, "SCEC", 4) ? 0x800 : 0;
    if (!span(wrapper, 20, bytes)) return USER_CATALOG_ERROR_METADATA;
    const unsigned char* sfo = input + wrapper;
    bytes -= wrapper;
    if (memcmp(sfo, "\0PSF", 4)) return USER_CATALOG_ERROR_METADATA;
    uint32_t keys = le32(sfo + 8), data = le32(sfo + 12), count = le32(sfo + 16);
    if (count > 256 || !span(20, size_t(count) * 16, bytes) || keys < 20 + size_t(count) * 16 ||
        keys > data || data > bytes) return USER_CATALOG_ERROR_METADATA;
    bool titleSeen = false, versionSeen = false, cidSeen = false, idSeen = false;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char* item = sfo + 20 + size_t(i) * 16;
        uint16_t keyOffset = le16(item), format = le16(item + 2);
        uint32_t length = le32(item + 4), maximum = le32(item + 8), offset = le32(item + 12);
        if (keyOffset >= data - keys || length > maximum || !span(data, offset, bytes) ||
            !span(size_t(data) + offset, maximum, bytes)) return USER_CATALOG_ERROR_METADATA;
        const char* key = (const char*)sfo + keys + keyOffset;
        size_t keyLimit = data - keys - keyOffset;
        size_t keyBytes = boundedLength(key, keyLimit);
        if (!keyBytes || keyBytes == keyLimit) return USER_CATALOG_ERROR_METADATA;
        const char* value = (const char*)sfo + data + offset;
        bool relevant = !strcmp(key, "TITLE") || !strcmp(key, "APP_VER") ||
                        !strcmp(key, "CONTENT_ID") || !strcmp(key, "TITLE_ID");
        if (!relevant) continue;
        if (format != 0x0204 || !length || value[length - 1] ||
            boundedLength(value, length) != length - 1) return USER_CATALOG_ERROR_METADATA;
        size_t n = length - 1;
        if (!strcmp(key, "TITLE")) {
            if (titleSeen || !n || n >= sizeof(entry.name) || !displayUtf8(value, n)) return USER_CATALOG_ERROR_METADATA;
            memcpy(entry.name, value, n + 1); entry.titleKnown = true; titleSeen = true;
        } else if (!strcmp(key, "APP_VER")) {
            if (versionSeen || !sfoVersionText(value, n)) return USER_CATALOG_ERROR_METADATA;
            memcpy(entry.version, value, n + 1); entry.versionKnown = true; versionSeen = true;
        } else if (!strcmp(key, "CONTENT_ID")) {
            if (cidSeen || n != 36 || memcmp(value, entry.contentId, 36)) return USER_CATALOG_ERROR_METADATA;
            cidSeen = true;
        } else {
            if (idSeen || n != 9 || memcmp(value, entry.titleId, 9)) return USER_CATALOG_ERROR_METADATA;
            idSeen = true;
        }
    }
    return USER_CATALOG_OK;
}
bool regularDestination(const char* path) {
    // O_NOFOLLOW rejects symlinks atomically. Never call OpenOrbis's unavailable
    // lstat/fstatat or substitute a following stat call.
    int descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) return errno == ENOENT;
    PeppyUserCatalogFileInfo info;
    bool regular = fileInfo(descriptor, info) == 0 && S_ISREG(info.mode) && info.links == 1;
    return close(descriptor) == 0 && regular;
}
bool safePath(const char* path) {
    size_t n = boundedLength(path, 1024);
    return n && n < 1024 && path[0] == '/' && !strstr(path, "/../") && !strstr(path, "/./") &&
           n >= 4 && !strcmp(path + n - 4, ".dat");
}
bool writeAll(int fd, const unsigned char* data, size_t bytes) {
    size_t offset = 0;
    while (offset < bytes) {
        ssize_t n = write(fd, data + offset, bytes - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        offset += size_t(n);
    }
    return true;
}
}

bool userCatalogPublicHttpsUrl(const char* url) {
    const char* host = 0; const char* path = 0; size_t hostBytes = 0;
    return urlParts(url, host, hostBytes, path);
}
int userCatalogSourceForUrl(const char* url) {
    const char* host = 0; const char* path = 0; size_t hostBytes = 0;
    if (!urlParts(url, host, hostBytes, path) || !lowerEqual(host, hostBytes, "github.com")) return USER_SOURCE_COMMUNITY_UNKNOWN;
    // Recognized author/repository provenance only. A GitHub host alone never
    // establishes ownership, licensing, a checked signature, or firmware support.
    const char* repos[] = {
        "bucanero/apollo-ps4", "cy33hc/ps4-ezremote-client", "LightningMods/PS4-Store",
        "0x199/ps4-ipi", "Backporter/ps4_remote_pkg_installer-OOSDK", "gen04177/freedoom-ps4",
        "gen04177/rarch-ps4", "buckethatboy3/pplay-Ps4-Media-Player", "Cpasjuste/pemu",
        "ItsJokerZz/FPKGi", "bucanero/PS4CheatsManager", "EmiiBytee/np2kai-ps4",
        "bizkut/ps4-mgba", "Mayo1970/ioQuake3-PS4", "JaimeJimenezG/Moonlight-ps4",
        "iHaiDeeZ/DolphinPS4", "01cedric/WoWPS", "diasurgical/DevilutionX",
        "marcussacana/FridayNightFunkin", "cy33hc/ps4-webdav-client", "victorrjimenezz/PS4-4PT",
        "kekeeek/KEKE-HOME", "xXxTheDarkprogramerxXx/PS4-PluginX", "Al-Azif/ps4-payload-guest",
        "kalaposfos13/ps4-homebrew-base", "GoldHEN/GoldHEN_Cheat_Manager", "Ninedark9/PS4PackageLink",
        "skidgfx/PS4-2048", "lorsanta/SDLPoP-PS4", "iHaiDeeZ/mari0-ps4", "EmiiBytee/Touhou-PS4",
        "jaca772/fallout2-ce-ps4", "MDashK/Sonic-Time-Twisted-PS4", "alechurri/shipofharkinian-ps4",
        "alechurri/2ship2harkinian-ps4", "MDashK/sonic-1-sms-remake-ps4", "MDashK/Sonic-2-SMS-Remake-PS4"
    };
    for (size_t i = 0; i < sizeof(repos) / sizeof(repos[0]); ++i) {
        size_t n = strlen(repos[i]);
        if (strlen(path + 1) <= n + 19) continue;
        if (!lowerEqual(path + 1, n, repos[i]) || strncmp(path + 1 + n, "/releases/download/", 19)) continue;
        const char* tag = path + 1 + n + 19;
        const char* slash = strchr(tag, '/');
        if (slash && slash != tag && slash[1] && !strchr(slash + 1, '/')) return USER_SOURCE_RECOGNIZED_AUTHOR_RELEASE;
    }
    return USER_SOURCE_COMMUNITY_UNKNOWN;
}
bool userCatalogEntryValid(const UserCatalogEntry& entry) { return validEntry(entry); }
bool userCatalogPrepareEntry(UserCatalogEntry* entry) {
    if (!entry || !userPackageCanonicalContentId(entry->contentId)) return false;
    UserCatalogEntry prepared = *entry;
    memcpy(prepared.titleId, prepared.contentId + 7, 9); prepared.titleId[9] = 0;
    prepared.source = userCatalogSourceForUrl(prepared.url);
    if (prepared.isTheme && prepared.contentType == 0x1B && prepared.iroTag == 1 && !prepared.requiresData[0])
        snprintf(prepared.requiresData, sizeof(prepared.requiresData), "Tema SHAREfactory; requer SHAREfactory para usar.");
    if (!prepared.displayCategory) prepared.displayCategory = prepared.isTheme ? 7 :
        (prepared.kind == USER_PACKAGE_UPDATE ? 5 : (prepared.kind == USER_PACKAGE_DLC ? 6 : 3));
    makeFilename(prepared);
    if (!validEntry(prepared)) return false;
    *entry = prepared;
    return true;
}
const char* userCatalogKindName(int kind) {
    if (kind == USER_PACKAGE_BASE) return "Base do usuário";
    if (kind == USER_PACKAGE_UPDATE) return "Update do usuário";
    if (kind == USER_PACKAGE_DLC) return "DLC do usuário";
    return "Tipo desconhecido";
}
const char* userCatalogDefaultPath() { return "/data/peppy-store/user-catalog.dat"; }
int userCatalogReadUrlListFile(const char* path, char** output, size_t* bytes) {
    if (output) *output = 0;
    if (bytes) *bytes = 0;
    if (!output || !bytes || !path || !path[0] || boundedLength(path, 1024) == 1024) return USER_CATALOG_ERROR_FILE;
    int descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0) return USER_CATALOG_ERROR_FILE;
    PeppyUserCatalogFileInfo before;
    if (fileInfo(descriptor, before) != 0 || !S_ISREG(before.mode) || before.links != 1) {
        close(descriptor); return USER_CATALOG_ERROR_FILE;
    }
    if (before.sizeBytes <= 0 || uint64_t(before.sizeBytes) > USER_CATALOG_MAX_LIST_BYTES) {
        close(descriptor); return USER_CATALOG_ERROR_LIST;
    }
    size_t total = size_t(before.sizeBytes);
    char* list = (char*)malloc(total + 1);
    if (!list) { close(descriptor); return USER_CATALOG_ERROR_MEMORY; }
    size_t received = 0;
    while (received < total) {
        ssize_t got = read(descriptor, list + received, total - received);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        received += size_t(got);
    }
    char extra = 0;
    PeppyUserCatalogFileInfo after;
    bool okay = received == total && read(descriptor, &extra, 1) == 0 && fileInfo(descriptor, after) == 0 &&
        before.device == after.device && before.inode == after.inode && before.sizeBytes == after.sizeBytes &&
        before.modificationTime[0] == after.modificationTime[0] && before.modificationTime[1] == after.modificationTime[1] &&
        before.changeTime[0] == after.changeTime[0] && before.changeTime[1] == after.changeTime[1];
    if (close(descriptor) != 0) okay = false;
    if (!okay) { free(list); return USER_CATALOG_ERROR_FILE; }
    if (memchr(list, 0, total)) { free(list); return USER_CATALOG_ERROR_LIST; }
    list[total] = 0;
    *output = list; *bytes = total;
    return USER_CATALOG_OK;
}
const char* userCatalogErrorMessage(int error) {
    switch (error) {
    case USER_CATALOG_OK: return "PKG adicionado à biblioteca.";
    case USER_CATALOG_ERROR_URL: return "Use um link HTTPS direto para o PKG.";
    case USER_CATALOG_ERROR_NETWORK: return "Não foi possível consultar o link.";
    case USER_CATALOG_ERROR_RANGE: return "O servidor não retornou os dados solicitados corretamente.";
    case USER_CATALOG_ERROR_HEADER: return "O arquivo não é um PKG PS4 de tipo reconhecido.";
    case USER_CATALOG_ERROR_METADATA: return "Os metadados do PKG são inválidos ou inconsistentes.";
    case USER_CATALOG_ERROR_DUPLICATE: return "Este PKG já está na biblioteca.";
    case USER_CATALOG_ERROR_FULL: return "A biblioteca pessoal atingiu 1024 PKGs.";
    case USER_CATALOG_ERROR_MEMORY: return "Memória insuficiente para adicionar o PKG.";
    case USER_CATALOG_ERROR_NOT_FOUND: return "A biblioteca pessoal ainda não foi criada.";
    case USER_CATALOG_ERROR_LIST: return "A lista excede os limites ou contém dados inválidos.";
    default: return "Não foi possível salvar ou abrir a biblioteca pessoal.";
    }
}

int probeUserPackage(const char* url, UserCatalogRangeReader reader, void* context, UserCatalogEntry* output) {
    if (!output || !userCatalogPublicHttpsUrl(url)) return USER_CATALOG_ERROR_URL;
    if (!reader) return USER_CATALOG_ERROR_NETWORK;
    unsigned char header[USER_PACKAGE_HEADER_BYTES] = {};
    uint64_t total = 0;
    int result = rangeRead(url, reader, context, 0, sizeof(header), header, total);
    if (result != USER_CATALOG_OK) return result;
    int kind = userPackageKind(header, sizeof(header));
    if (kind == USER_PACKAGE_UNKNOWN || userPackageBe64(header + 0x430) != total) return USER_CATALOG_ERROR_HEADER;
    UserCatalogEntry entry = {};
    memcpy(entry.url, url, strlen(url) + 1);
    memcpy(entry.contentId, header + 0x40, 36);
    memcpy(entry.titleId, entry.contentId + 7, 9);
    memcpy(entry.name, entry.titleId, sizeof(entry.titleId));
    entry.sizeBytes = total; entry.kind = kind; entry.source = userCatalogSourceForUrl(url);
    entry.contentType = userPackageBe32(header + 0x74); entry.contentFlags = userPackageBe32(header + 0x78);
    entry.iroTag = userPackageBe32(header + 0x98);
    entry.isTheme = userPackageIsTheme(header, sizeof(header));
    uint32_t count = userPackageBe32(header + 0x10), tableOffset = userPackageBe32(header + 0x18);
    uint64_t tableBytes = uint64_t(count) * 32;
    if (count && (tableOffset < USER_PACKAGE_HEADER_BYTES || tableOffset > total || tableBytes > total - tableOffset))
        return USER_CATALOG_ERROR_METADATA;
    if (count && tableBytes <= MAX_TABLE_BYTES) {
        unsigned char* table = (unsigned char*)malloc(size_t(tableBytes));
        if (!table) return USER_CATALOG_ERROR_MEMORY;
        result = rangeRead(url, reader, context, tableOffset, size_t(tableBytes), table, total);
        uint32_t sfoOffset = 0, sfoBytes = 0;
        bool sfoFound = false, sfoEncrypted = false;
        if (result == USER_CATALOG_OK) for (uint32_t i = 0; i < count; ++i) {
            const unsigned char* item = table + size_t(i) * 32;
            if (userPackageBe32(item) != 0x1000) continue;
            if (sfoFound) { result = USER_CATALOG_ERROR_METADATA; break; }
            sfoFound = true; sfoEncrypted = (userPackageBe32(item + 8) & 0x80000000U) != 0;
            sfoOffset = userPackageBe32(item + 16); sfoBytes = userPackageBe32(item + 20);
            if (sfoOffset < USER_PACKAGE_HEADER_BYTES || sfoOffset > total || sfoBytes > total - sfoOffset)
                result = USER_CATALOG_ERROR_METADATA;
        }
        free(table);
        if (result != USER_CATALOG_OK) return result;
        if (sfoFound && !sfoEncrypted && sfoBytes <= MAX_SFO_BYTES) {
            if (sfoBytes < 20) return USER_CATALOG_ERROR_METADATA;
            unsigned char* sfo = (unsigned char*)malloc(sfoBytes);
            if (!sfo) return USER_CATALOG_ERROR_MEMORY;
            result = rangeRead(url, reader, context, sfoOffset, sfoBytes, sfo, total);
            if (result == USER_CATALOG_OK) result = parseSfo(sfo, sfoBytes, entry);
            free(sfo);
            if (result != USER_CATALOG_OK) return result;
        }
    }
    if (!userCatalogPrepareEntry(&entry)) return USER_CATALOG_ERROR_METADATA;
    *output = entry;
    return USER_CATALOG_OK;
}

UserCatalog::UserCatalog() : entries_(0), count_(0) {}
UserCatalog::~UserCatalog() { free(entries_); }
size_t UserCatalog::count() const { return count_; }
const UserCatalogEntry* UserCatalog::at(size_t index) const { return index < count_ ? entries_ + index : 0; }
int UserCatalog::add(const UserCatalogEntry& entry) {
    if (!validEntry(entry)) return USER_CATALOG_ERROR_METADATA;
    for (size_t i = 0; i < count_; ++i) if (duplicate(entry, entries_[i])) return USER_CATALOG_ERROR_DUPLICATE;
    if (count_ == USER_CATALOG_MAX_ITEMS) return USER_CATALOG_ERROR_FULL;
    if (!entries_) {
        entries_ = (UserCatalogEntry*)calloc(USER_CATALOG_MAX_ITEMS, sizeof(UserCatalogEntry));
        if (!entries_) return USER_CATALOG_ERROR_MEMORY;
    }
    entries_[count_++] = entry;
    return USER_CATALOG_OK;
}
int UserCatalog::importUrl(const char* url, UserCatalogRangeReader reader, void* context) {
    if (count_ == USER_CATALOG_MAX_ITEMS) return USER_CATALOG_ERROR_FULL;
    UserCatalogEntry entry = {};
    int result = probeUserPackage(url, reader, context, &entry);
    return result == USER_CATALOG_OK ? add(entry) : result;
}
UserCatalogImportReport UserCatalog::importUrlList(const char* text, size_t bytes, UserCatalogRangeReader reader, void* context) {
    UserCatalogImportReport report = {};
    if (!text || bytes > USER_CATALOG_MAX_LIST_BYTES || memchr(text, 0, bytes)) {
        report.rejected = 1; report.firstError = USER_CATALOG_ERROR_LIST; return report;
    }
    for (size_t start = 0; start < bytes;) {
        size_t end = start;
        while (end < bytes && text[end] != '\n' && text[end] != '\r') ++end;
        size_t next = end;
        while (next < bytes && (text[next] == '\n' || text[next] == '\r')) ++next;
        while (start < end && (text[start] == ' ' || text[start] == '\t')) ++start;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
        if (start != end) {
            if (report.requested == USER_CATALOG_MAX_ITEMS) {
                ++report.rejected; if (!report.firstError) report.firstError = USER_CATALOG_ERROR_LIST; break;
            }
            ++report.requested;
            int result = USER_CATALOG_ERROR_URL;
            if (end - start <= USER_CATALOG_MAX_URL_BYTES) {
                char url[USER_CATALOG_MAX_URL_BYTES + 1] = {};
                memcpy(url, text + start, end - start);
                result = importUrl(url, reader, context);
            }
            if (result == USER_CATALOG_OK) ++report.added;
            else if (result == USER_CATALOG_ERROR_DUPLICATE) ++report.duplicates;
            else { ++report.rejected; if (!report.firstError) report.firstError = result; }
        }
        start = next;
    }
    return report;
}
bool UserCatalog::remove(size_t index) {
    if (index >= count_) return false;
    if (index + 1 < count_) memmove(entries_ + index, entries_ + index + 1, (count_ - index - 1) * sizeof(UserCatalogEntry));
    memset(entries_ + --count_, 0, sizeof(UserCatalogEntry));
    return true;
}
void UserCatalog::clear() { if (entries_) memset(entries_, 0, count_ * sizeof(UserCatalogEntry)); count_ = 0; }

int UserCatalog::save(const char* path) const {
    if (!path) path = userCatalogDefaultPath();
    if (!safePath(path) || !regularDestination(path)) return USER_CATALOG_ERROR_FILE;
    unsigned char* buffer = (unsigned char*)calloc(1, MAX_FILE_BYTES);
    if (!buffer) return USER_CATALOG_ERROR_MEMORY;
    size_t used = 24;
    for (size_t i = 0; i < count_; ++i) {
        const UserCatalogEntry& entry = entries_[i];
        if (!validEntry(entry)) { free(buffer); return USER_CATALOG_ERROR_METADATA; }
        unsigned char* p = buffer + used;
        p[0] = (unsigned char)entry.kind; p[1] = (unsigned char)entry.source;
        p[2] = (entry.titleKnown ? 1 : 0) | (entry.versionKnown ? 2 : 0) | (entry.isTheme ? 4 : 0);
        p[2] |= entry.adult ? 8 : 0;
        put64(p + 4, entry.sizeBytes); put32(p + 12, entry.contentType); put32(p + 16, entry.contentFlags);
        put32(p + 20, entry.iroTag); put32(p + 24, (uint32_t)entry.displayCategory);
        used += 28;
        const char* fields[] = { entry.url, entry.name, entry.version, entry.contentId, entry.description, entry.requiresData, entry.sha256 };
        for (size_t j = 0; j < 7; ++j) {
            size_t n = strlen(fields[j]);
            put16(buffer + used, (uint16_t)n); used += 2;
            memcpy(buffer + used, fields[j], n); used += n;
        }
    }
    memcpy(buffer, "PEPPYCAT", 8); put32(buffer + 8, 2); put32(buffer + 12, (uint32_t)count_);
    put32(buffer + 16, (uint32_t)(used - 24)); put32(buffer + 20, checksum(buffer + 24, used - 24));
    char temporary[1048];
    snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
    int fd = mkstemp(temporary);
    bool ok = fd >= 0;
    if (ok) ok = fchmod(fd, 0600) == 0 && writeAll(fd, buffer, used) && fsync(fd) == 0;
    if (fd >= 0 && close(fd) < 0) ok = false;
    if (ok) ok = regularDestination(path) && rename(temporary, path) == 0;
    if (!ok && fd >= 0) unlink(temporary);
    free(buffer);
    return ok ? USER_CATALOG_OK : USER_CATALOG_ERROR_FILE;
}
int UserCatalog::load(const char* path) {
    if (!path) path = userCatalogDefaultPath();
    if (!safePath(path)) return USER_CATALOG_ERROR_FILE;
    int flags = O_RDONLY | O_NONBLOCK | O_NOFOLLOW;
    int fd = open(path, flags);
    if (fd < 0) return errno == ENOENT ? USER_CATALOG_ERROR_NOT_FOUND : USER_CATALOG_ERROR_FILE;
    PeppyUserCatalogFileInfo before;
    if (fileInfo(fd, before) != 0 || !S_ISREG(before.mode) || before.links != 1 ||
        before.sizeBytes < 24 || uint64_t(before.sizeBytes) > MAX_FILE_BYTES) { close(fd); return USER_CATALOG_ERROR_FILE; }
    size_t bytes = size_t(before.sizeBytes);
    unsigned char* buffer = (unsigned char*)malloc(bytes);
    if (!buffer) { close(fd); return USER_CATALOG_ERROR_MEMORY; }
    size_t used = 0;
    while (used < bytes) {
        ssize_t n = read(fd, buffer + used, bytes - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += size_t(n);
    }
    unsigned char extra = 0;
    PeppyUserCatalogFileInfo after;
    bool consistent = used == bytes && read(fd, &extra, 1) == 0 && fileInfo(fd, after) == 0 &&
                      before.device == after.device && before.inode == after.inode && before.sizeBytes == after.sizeBytes &&
                      before.modificationTime[0] == after.modificationTime[0] && before.modificationTime[1] == after.modificationTime[1] &&
                      before.changeTime[0] == after.changeTime[0] && before.changeTime[1] == after.changeTime[1];
    close(fd);
    uint32_t count = consistent ? le32(buffer + 12) : 0;
    if (!consistent || memcmp(buffer, "PEPPYCAT", 8) || le32(buffer + 8) != 2 || count > USER_CATALOG_MAX_ITEMS ||
        le32(buffer + 16) != bytes - 24 || le32(buffer + 20) != checksum(buffer + 24, bytes - 24)) {
        free(buffer); return USER_CATALOG_ERROR_FILE;
    }
    UserCatalog incoming;
    used = 24;
    int result = USER_CATALOG_OK;
    for (uint32_t i = 0; i < count; ++i) {
        if (!span(used, 28, bytes)) { result = USER_CATALOG_ERROR_FILE; break; }
        const unsigned char* p = buffer + used;
        UserCatalogEntry entry = {};
        entry.kind = p[0]; entry.source = p[1]; entry.titleKnown = (p[2] & 1) != 0;
        entry.versionKnown = (p[2] & 2) != 0; entry.isTheme = (p[2] & 4) != 0;
        entry.adult = (p[2] & 8) != 0;
        if ((p[2] & ~15U) || p[3]) { result = USER_CATALOG_ERROR_FILE; break; }
        entry.sizeBytes = le64(p + 4); entry.contentType = le32(p + 12); entry.contentFlags = le32(p + 16);
        entry.iroTag = le32(p + 20); entry.displayCategory = (int)le32(p + 24);
        used += 28;
        char* fields[] = { entry.url, entry.name, entry.version, entry.contentId, entry.description, entry.requiresData, entry.sha256 };
        const size_t capacities[] = { sizeof(entry.url), sizeof(entry.name), sizeof(entry.version), sizeof(entry.contentId), sizeof(entry.description), sizeof(entry.requiresData), sizeof(entry.sha256) };
        for (size_t j = 0; j < 7; ++j) {
            if (!span(used, 2, bytes)) { result = USER_CATALOG_ERROR_FILE; break; }
            size_t n = le16(buffer + used); used += 2;
            if (n >= capacities[j] || !span(used, n, bytes) || memchr(buffer + used, 0, n)) { result = USER_CATALOG_ERROR_FILE; break; }
            memcpy(fields[j], buffer + used, n); used += n;
        }
        if (result != USER_CATALOG_OK) break;
        if (!userPackageCanonicalContentId(entry.contentId)) { result = USER_CATALOG_ERROR_METADATA; break; }
        memcpy(entry.titleId, entry.contentId + 7, 9); makeFilename(entry);
        result = incoming.add(entry);
        if (result != USER_CATALOG_OK) break;
    }
    free(buffer);
    if (result == USER_CATALOG_OK && used != bytes) result = USER_CATALOG_ERROR_FILE;
    if (result != USER_CATALOG_OK) return result;
    free(entries_); entries_ = incoming.entries_; count_ = incoming.count_;
    incoming.entries_ = 0; incoming.count_ = 0;
    return USER_CATALOG_OK;
}
