#ifndef PEPPY_DOWNLOADS_H
#define PEPPY_DOWNLOADS_H

#include <stdint.h>

struct DownloadSpec {
    const char* url;
    const char* filename;
    uint64_t expectedBytes;
    const char* sha256;
};

enum DownloadState { IDLE = 0, RUNNING = 1, DONE = 2, FAILED = 3, CANCELLED = 4 };

enum DownloadError {
    DOWNLOAD_ERROR_SPEC = -1000,
    DOWNLOAD_ERROR_THREAD = -1001,
    DOWNLOAD_ERROR_NETWORK = -1100,
    DOWNLOAD_ERROR_HTTP = -1101,
    DOWNLOAD_ERROR_REDIRECT = -1102,
    DOWNLOAD_ERROR_TLS = -1200,
    DOWNLOAD_ERROR_FILESYSTEM = -1300,
    DOWNLOAD_ERROR_LENGTH = -1400,
    DOWNLOAD_ERROR_PACKAGE = -1401,
    DOWNLOAD_ERROR_HASH = -1402
};

struct DownloadSnapshot {
    int state;
    uint64_t received;
    uint64_t total;
    int errorCode;
};

// Only one transfer can run. A rejected spec or thread failure sets FAILED.
// Completion means a downloaded file, not an installed application.
// Start/cancel are serialized by the controller thread; the worker publishes
// progress atomically. sha256 is optional plain 64-character hexadecimal text.
bool startDownload(const DownloadSpec& spec);
void cancelDownload();
DownloadSnapshot downloadSnapshot();

#endif
