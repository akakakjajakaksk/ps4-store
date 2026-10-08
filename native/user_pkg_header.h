#ifndef PEPPY_USER_PKG_HEADER_H
#define PEPPY_USER_PKG_HEADER_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "package_limits.h"

// Values match the existing FTP inbox and installer ABI.
enum UserPackageKind {
    USER_PACKAGE_UNKNOWN = -1,
    USER_PACKAGE_BASE = 0,
    USER_PACKAGE_UPDATE = 1,
    USER_PACKAGE_DLC = 2
};

static const size_t USER_PACKAGE_HEADER_BYTES = 0x438;
static const size_t USER_PACKAGE_CONTENT_ID_BYTES = 36;

inline uint32_t userPackageBe32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint64_t userPackageBe64(const unsigned char* p) {
    return (uint64_t(userPackageBe32(p)) << 32) | userPackageBe32(p + 4);
}
inline bool userPackageCanonicalContentId(const char* p) {
    if (!p) return false;
    for (size_t i = 0; i < USER_PACKAGE_CONTENT_ID_BYTES; ++i) {
        unsigned char ch = (unsigned char)p[i];
        if (!ch) return false;
        bool letter = ch >= 'A' && ch <= 'Z';
        bool digit = ch >= '0' && ch <= '9';
        if (i == 6 || i == 19) { if (ch != '-') return false; }
        else if (i == 16) { if (ch != '_') return false; }
        else if (i < 2 || (i >= 7 && i < 11)) { if (!letter) return false; }
        else if (i < 6 || (i >= 11 && i < 16) || (i >= 17 && i < 19)) { if (!digit) return false; }
        else if (!letter && !digit) return false;
    }
    return p[USER_PACKAGE_CONTENT_ID_BYTES] == 0;
}

// Pinned reference: maxton/LibOrbisPkg commit
// 643477263b2644e0803e0f58b8726ea4e3f3b7d4, PKG/Enums.cs.
// A PS5 Content ID, an unknown base flag pattern, or Peppy's own Title ID
// never becomes a user-installable package merely because its URL says .pkg.
inline int userPackageKind(const unsigned char* header, size_t bytes) {
    if (!header || bytes < USER_PACKAGE_HEADER_BYTES ||
        memcmp(header, "\x7f" "CNT", 4)) return USER_PACKAGE_UNKNOWN;
    char contentId[USER_PACKAGE_CONTENT_ID_BYTES + 1];
    memcpy(contentId, header + 0x40, USER_PACKAGE_CONTENT_ID_BYTES);
    contentId[USER_PACKAGE_CONTENT_ID_BYTES] = 0;
    if (!userPackageCanonicalContentId(contentId) ||
        !strncmp(contentId + 7, "PPSA", 4) ||
        !strncmp(contentId + 7, "BREW00001", 9)) return USER_PACKAGE_UNKNOWN;
    uint32_t type = userPackageBe32(header + 0x74);
    uint32_t flags = userPackageBe32(header + 0x78);
    const uint32_t patchFlags = 0x61300000U;
    if (type == 0x1E) return USER_PACKAGE_UPDATE;
    if (type == 0x1B || type == 0x1C)
        return flags & patchFlags ? USER_PACKAGE_UNKNOWN : USER_PACKAGE_DLC;
    if (type != 0x1A) return USER_PACKAGE_UNKNOWN;
    if (flags & patchFlags) return USER_PACKAGE_UPDATE;
    if (flags == 0x0A000000U || flags == 0x0E000000U) return USER_PACKAGE_BASE;
    return USER_PACKAGE_UNKNOWN;
}

inline bool userPackageIsTheme(const unsigned char* header, size_t bytes) {
    // AC is the documented container for SHAREfactory/system themes. AL is
    // an add-on without data; its IRO tag alone does not establish a theme.
    if (userPackageKind(header, bytes) != USER_PACKAGE_DLC ||
        userPackageBe32(header + 0x74) != 0x1B) return false;
    uint32_t tag = userPackageBe32(header + 0x98);
    return tag == 1 || tag == 2;
}

inline bool userPackageHeaderMatches(const unsigned char* header, size_t bytes,
                                    uint64_t expectedBytes, const char* expectedContentId,
                                    int expectedKind) {
    return header && bytes >= USER_PACKAGE_HEADER_BYTES &&
           expectedBytes >= USER_PACKAGE_HEADER_BYTES && expectedBytes <= PEPPY_MAX_PACKAGE_BYTES &&
           userPackageCanonicalContentId(expectedContentId) &&
           userPackageKind(header, bytes) == expectedKind &&
           !memcmp(header + 0x40, expectedContentId, USER_PACKAGE_CONTENT_ID_BYTES) &&
           userPackageBe64(header + 0x430) == expectedBytes;
}

#endif
