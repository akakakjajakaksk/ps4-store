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
    DOWNLOAD_ERROR_NOT_READY = -1103,
    DOWNLOAD_ERROR_TLS = -1200,
    DOWNLOAD_ERROR_FILESYSTEM = -1300,
    DOWNLOAD_ERROR_LENGTH = -1400,
    DOWNLOAD_ERROR_PACKAGE = -1401,
    DOWNLOAD_ERROR_HASH = -1402
};

enum DownloadStage {
    DOWNLOAD_STAGE_NONE = 0, DOWNLOAD_STAGE_DIRECTORY,
    DOWNLOAD_STAGE_MODULE_NET, DOWNLOAD_STAGE_MODULE_SSL,
    DOWNLOAD_STAGE_MODULE_HTTP, DOWNLOAD_STAGE_MODULE_NETCTL,
    DOWNLOAD_STAGE_NETCTL_INIT, DOWNLOAD_STAGE_NETCTL_STATE,
    DOWNLOAD_STAGE_NET_INIT, DOWNLOAD_STAGE_NET_POOL, DOWNLOAD_STAGE_SSL_INIT,
    DOWNLOAD_STAGE_HTTP_INIT, DOWNLOAD_STAGE_TEMPLATE, DOWNLOAD_STAGE_TLS_OPTIONS,
    DOWNLOAD_STAGE_REDIRECT_OPTION, DOWNLOAD_STAGE_RESOLVE_TIMEOUT,
    DOWNLOAD_STAGE_CONNECT_TIMEOUT, DOWNLOAD_STAGE_SEND_TIMEOUT,
    DOWNLOAD_STAGE_RECV_TIMEOUT, DOWNLOAD_STAGE_CONNECTION,
    DOWNLOAD_STAGE_REQUEST, DOWNLOAD_STAGE_REQUEST_HEADER, DOWNLOAD_STAGE_SEND,
    DOWNLOAD_STAGE_STATUS, DOWNLOAD_STAGE_HEADERS, DOWNLOAD_STAGE_CONTENT_LENGTH,
    DOWNLOAD_STAGE_FILE_OPEN, DOWNLOAD_STAGE_READ, DOWNLOAD_STAGE_PACKAGE,
    DOWNLOAD_STAGE_HASH, DOWNLOAD_STAGE_FILE_WRITE, DOWNLOAD_STAGE_FILE_FLUSH,
    DOWNLOAD_STAGE_FILE_CLOSE, DOWNLOAD_STAGE_FILE_RENAME,
    DOWNLOAD_STAGE_FILE_CLEANUP, DOWNLOAD_STAGE_FINISHED,
    DOWNLOAD_STAGE_THREAD, DOWNLOAD_STAGE_SPEC
};

struct DownloadSnapshot {
    int state;
    uint64_t received;
    uint64_t total;
    int errorCode;
    int stage;
    int32_t nativeCode;
    int32_t networkCode;
    int32_t sslCode;
    uint32_t sslDetails;
    int networkState;
};

// Only one transfer can run. A rejected spec or thread failure sets FAILED.
// Completion means a downloaded file, not an installed application.
// Start/cancel are serialized by the controller thread; the worker publishes
// progress atomically. sha256 is optional plain 64-character hexadecimal text.
bool startDownload(const DownloadSpec& spec);
void cancelDownload();
DownloadSnapshot downloadSnapshot();
const char* downloadStageName(int stage);

#endif
