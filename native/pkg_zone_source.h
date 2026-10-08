#ifndef PEPPY_PKG_ZONE_SOURCE_H
#define PEPPY_PKG_ZONE_SOURCE_H

#include <stddef.h>
#include <string.h>

// Reviewed PS4 binary-download route, not the provider's search/API, website
// pages or PS5 catalog. Credentials, query strings and alternate hosts/ports
// are excluded. Package identity and size are still pinned by each caller.
namespace peppyPkgZone {
static const char PREFIX[] = "https://pkg-zone.com/download/ps4/";
static const size_t TITLE_BYTES = 9;
inline bool isDownloadUrl(const char* url, size_t length, size_t* originLength = 0) {
    const size_t prefixBytes = sizeof(PREFIX) - 1;
    const char suffix[] = "/latest";
    if (!url || length != prefixBytes + TITLE_BYTES + sizeof(suffix) - 1 ||
        memcmp(url, PREFIX, prefixBytes)) return false;
    const char* title = url + prefixBytes;
    if (memcmp(title, "CUSA", 4)) return false;
    for (size_t at = 4; at < TITLE_BYTES; ++at)
        if (title[at] < '0' || title[at] > '9') return false;
    if (memcmp(title + TITLE_BYTES, suffix, sizeof(suffix) - 1)) return false;
    if (originLength) *originLength = sizeof("https://pkg-zone.com") - 1;
    return true;
}
inline bool sameDownloadTarget(const char* current, size_t currentLength,
                               const char* next, size_t nextLength) {
    return isDownloadUrl(current, currentLength) && isDownloadUrl(next, nextLength) &&
        !memcmp(current + sizeof(PREFIX) - 1, next + sizeof(PREFIX) - 1, TITLE_BYTES);
}
inline bool matchesTitleId(const char* url, size_t length, const char* title, size_t titleLength) {
    if (!title || titleLength != TITLE_BYTES || !isDownloadUrl(url, length)) return false;
    const char* routeTitle = url + sizeof(PREFIX) - 1;
    if (!memcmp(routeTitle, title, TITLE_BYTES)) return true;
    // The observed YouTube route is indexed as CUSA01116, but its binary has
    // CUSA01015 in the Content ID. Retain and pin that actual binary identity.
    return !memcmp(routeTitle, "CUSA01116", TITLE_BYTES) && !memcmp(title, "CUSA01015", TITLE_BYTES);
}
} // namespace peppyPkgZone

#endif
