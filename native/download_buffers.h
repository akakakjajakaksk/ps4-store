#ifndef PEPPY_DOWNLOAD_BUFFERS_H
#define PEPPY_DOWNLOAD_BUFFERS_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// A bounded working set, independent of PKG size. This is an application cap,
// not a claim about the maximum receive buffer supported by PS4 firmware.
namespace peppyDownloadBuffers {
const size_t PREFERRED_BYTES = 1024 * 1024;
const size_t FALLBACK_BYTES = 256 * 1024;

class Buffers {
    bool pair;
    Buffers(const Buffers&);
    Buffers& operator=(const Buffers&);
    bool allocate(size_t bytes) {
        first = static_cast<uint8_t*>(malloc(bytes));
        if (first && pair) second = static_cast<uint8_t*>(malloc(bytes));
        if (first && (!pair || second)) { capacity = bytes; return true; }
        // Both lanes must use the same actual capacity. Never retain a larger
        // first allocation beside a smaller fallback second allocation.
        free(second); free(first); second = first = 0;
        return false;
    }
public:
    uint8_t* first;
    uint8_t* second;
    size_t capacity;
    explicit Buffers(bool two = true) : pair(two), first(0), second(0), capacity(0) {
        if (!allocate(PREFERRED_BYTES)) allocate(FALLBACK_BYTES);
    }
    ~Buffers() { free(second); free(first); }
    bool ready() const { return first && (!pair || second); }
};
} // namespace peppyDownloadBuffers

#endif
