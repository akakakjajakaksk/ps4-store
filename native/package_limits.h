#ifndef PEPPY_PACKAGE_LIMITS_H
#define PEPPY_PACKAGE_LIMITS_H

#include <stdint.h>

// Keep download and installation validation consistent for complete packages.
// This is a bounded file size, independent of small streaming buffer sizes.
constexpr uint64_t PEPPY_MAX_PACKAGE_BYTES = 256ULL * 1024ULL * 1024ULL * 1024ULL;
static_assert(PEPPY_MAX_PACKAGE_BYTES < (1ULL << 63), "package size fits signed file offsets");

#endif
