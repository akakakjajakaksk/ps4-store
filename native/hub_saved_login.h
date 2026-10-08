#ifndef PEPPY_HUB_SAVED_LOGIN_H
#define PEPPY_HUB_SAVED_LOGIN_H

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Credentials stay in this console's private app directory. This is filesystem
// access control, not a platform keychain or encryption. No role is stored here:
// every restored login needs a successful response from the configured server.
namespace peppyHubSavedLogin {
struct Record {
    char origin[1024], username[65], password[129], token[257];
    uint64_t expiresAt;
};
struct NativeFileInfo {
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
static_assert(sizeof(NativeFileInfo) == 120 && offsetof(NativeFileInfo, mode) == 8 &&
    offsetof(NativeFileInfo, sizeBytes) == 72, "PS4 saved-login file metadata ABI");
#if defined(__FreeBSD__) || defined(PS4) || defined(PEPPY_HUB_NATIVE_FILE_ABI)
extern "C" int32_t savedLoginNativeFstat(int32_t, NativeFileInfo*) __asm__("sceKernelFstat");
#endif
inline void wipe(void* data, size_t bytes) {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(data);
    while (bytes--) *p++ = 0;
}
inline bool fileInfo(int fd, NativeFileInfo& info) {
    memset(&info, 0, sizeof(info));
#if defined(__FreeBSD__) || defined(PS4) || defined(PEPPY_HUB_NATIVE_FILE_ABI)
    return savedLoginNativeFstat(fd, &info) == 0;
#else
    struct stat st;
    if (fstat(fd, &st)) return false;
    info.device = uint32_t(st.st_dev); info.inode = uint32_t(st.st_ino);
    info.mode = uint16_t(st.st_mode); info.links = uint16_t(st.st_nlink);
    info.sizeBytes = st.st_size;
    return true;
#endif
}
inline bool safePath(const char* path) {
    if (!path || path[0] != '/' || !path[1] || strlen(path) > 1023 || strstr(path, "//")) return false;
    const char* part = path + 1;
    do {
        const char* end = strchr(part, '/'); size_t n = end ? size_t(end - part) : strlen(part);
        if (!n || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) return false;
        for (size_t i = 0; i < n; ++i) if ((unsigned char)part[i] < 32 || part[i] == '\\') return false;
        if (!end) return true;
        part = end + 1;
    } while (*part);
    return false;
}
inline bool privateParent(const char* path) {
    if (!safePath(path)) return false;
    char parent[1024]; strcpy(parent, path); char* slash = strrchr(parent, '/');
    if (!slash || slash == parent) return false;
    *slash = 0;
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    NativeFileInfo info;
    bool ok = fileInfo(fd, info) && S_ISDIR(info.mode) && (info.mode & 0777) == 0700;
    close(fd); return ok;
}
inline bool regularDestination(const char* path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT;
    NativeFileInfo info;
    bool ok = fileInfo(fd, info) && S_ISREG(info.mode) && info.links == 1;
    close(fd); return ok;
}
inline bool forget(const char* path) {
    if (!privateParent(path)) return false;
    // unlink removes a final symlink itself and never follows its target. This
    // fallback intentionally drops the username too when atomic save fails.
    return unlink(path) == 0 || errno == ENOENT;
}
inline size_t boundedLength(const char* text, size_t cap) {
    size_t n = 0; while (n < cap && text[n]) ++n; return n;
}
inline uint32_t checksum(const unsigned char* data, size_t bytes) {
    uint32_t value = 2166136261U;
    for (size_t i = 0; i < bytes; ++i) value = (value ^ data[i]) * 16777619U;
    return value; // Accidental corruption detection, not authentication.
}
inline void put32(unsigned char* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}
inline uint32_t get32(const unsigned char* p) {
    uint32_t value = 0; for (unsigned i = 0; i < 4; ++i) value |= uint32_t(p[i]) << (8 * i); return value;
}
inline bool save(const char* path, const Record& record) {
    if (!privateParent(path) || !regularDestination(path)) return false;
    unsigned char bytes[1536] = {};
    const char* fields[] = {record.origin, record.username, record.password, record.token};
    const size_t caps[] = {sizeof(record.origin), sizeof(record.username), sizeof(record.password), sizeof(record.token)};
    size_t used = 28;
    memcpy(bytes, "PEPPYLOG", 8); put32(bytes + 8, 1);
    for (unsigned i = 0; i < 8; ++i) bytes[20 + i] = (unsigned char)(record.expiresAt >> (8 * i));
    bool ok = true;
    for (size_t i = 0; i < 4 && ok; ++i) {
        size_t n = boundedLength(fields[i], caps[i]);
        ok = n < caps[i] && used + 2 + n <= sizeof(bytes);
        if (ok) { bytes[used++] = (unsigned char)n; bytes[used++] = (unsigned char)(n >> 8); memcpy(bytes + used, fields[i], n); used += n; }
    }
    if (!ok) { wipe(bytes, sizeof(bytes)); return false; }
    put32(bytes + 12, uint32_t(used)); put32(bytes + 16, checksum(bytes + 20, used - 20));
    char temporary[1048]; snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
    int fd = mkstemp(temporary); ok = fd >= 0;
    if (ok) ok = fchmod(fd, 0600) == 0;
    size_t done = 0;
    while (ok && done < used) {
        ssize_t n = write(fd, bytes + done, used - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false; else done += size_t(n);
    }
    if (ok) ok = fsync(fd) == 0;
    if (fd >= 0 && close(fd)) ok = false;
    if (ok) ok = privateParent(path) && regularDestination(path) && rename(temporary, path) == 0;
    if (!ok && fd >= 0) unlink(temporary);
    wipe(bytes, sizeof(bytes)); return ok;
}
// 1 = loaded, 0 = absent, -1 = malformed/unreadable/non-private.
inline int load(const char* path, Record& output) {
    wipe(&output, sizeof(output));
    if (!privateParent(path)) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    NativeFileInfo before, after; unsigned char bytes[1536] = {};
    bool ok = fileInfo(fd, before) && S_ISREG(before.mode) && before.links == 1 &&
        (before.mode & 0777) == 0600 && before.sizeBytes >= 36 && before.sizeBytes <= int64_t(sizeof(bytes));
    size_t wanted = ok ? size_t(before.sizeBytes) : 0, done = 0;
    while (ok && done < wanted) {
        ssize_t n = read(fd, bytes + done, wanted - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false; else done += size_t(n);
    }
    unsigned char extra;
    ok = ok && read(fd, &extra, 1) == 0 && fileInfo(fd, after) && before.device == after.device &&
        before.inode == after.inode && before.sizeBytes == after.sizeBytes && before.mode == after.mode && after.links == 1;
    close(fd);
    ok = ok && !memcmp(bytes, "PEPPYLOG", 8) && get32(bytes + 8) == 1 && get32(bytes + 12) == wanted &&
        get32(bytes + 16) == checksum(bytes + 20, wanted - 20);
    size_t cursor = 28; char* fields[] = {output.origin, output.username, output.password, output.token};
    const size_t caps[] = {sizeof(output.origin), sizeof(output.username), sizeof(output.password), sizeof(output.token)};
    for (unsigned i = 0; i < 8 && ok; ++i) output.expiresAt |= uint64_t(bytes[20 + i]) << (8 * i);
    for (size_t i = 0; i < 4 && ok; ++i) {
        ok = cursor + 2 <= wanted;
        size_t n = ok ? size_t(bytes[cursor]) | (size_t(bytes[cursor + 1]) << 8) : 0; cursor += 2;
        ok = ok && n < caps[i] && cursor + n <= wanted && !memchr(bytes + cursor, 0, n);
        if (ok) { memcpy(fields[i], bytes + cursor, n); fields[i][n] = 0; cursor += n; }
    }
    ok = ok && cursor == wanted;
    wipe(bytes, sizeof(bytes));
    if (!ok) wipe(&output, sizeof(output));
    return ok ? 1 : -1;
}
} // namespace peppyHubSavedLogin
#endif
