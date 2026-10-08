#include "downloads.h"
#include "package_limits.h"
#include "mediafire_source.h"
#include "archive_sources.h"
#include "http_range.h"
#include "parallel_download.h"
#include "stream_download.h"
#include "user_pkg_header.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Ssl.h>
#include <orbis/Http.h>

static_assert(sizeof(size_t) == 8 && sizeof(off_t) == 8,
              "PS4 package transfers require 64-bit lengths and file offsets");

// OpenOrbis 0.5.x leaves these functions declared void(). The aliases retain
// their native symbols while supplying the completed public PS4 signatures.
extern "C" int32_t peppyHttpSetAutoRedirect(int32_t, int32_t)
    __asm__("sceHttpSetAutoRedirect");
extern "C" int32_t peppyHttpSetRecvTimeOut(int32_t, uint32_t)
    __asm__("sceHttpSetRecvTimeOut");
extern "C" int32_t peppyHttpSetResponseHeaderMaxSize(int32_t, size_t)
    __asm__("sceHttpSetResponseHeaderMaxSize");
extern "C" int32_t peppyHttpsGetSslError(int32_t, int32_t*, uint32_t*)
    __asm__("sceHttpsGetSslError");
extern "C" int32_t peppySslTerm(int32_t) __asm__("sceSslTerm");
extern "C" int32_t peppySysmoduleIsLoaded(OrbisSysModuleInternal)
    __asm__("sceSysmoduleIsLoadedInternal");
extern "C" int32_t peppyNetCtlGetState(int32_t*) __asm__("sceNetCtlGetState");
extern "C" int32_t* peppyNetErrnoLoc() __asm__("sceNetErrnoLoc");

#ifndef PEPPY_DOWNLOAD_DIRECTORY
#define PEPPY_DOWNLOAD_DIRECTORY "/data/peppy-store/downloads"
#endif

namespace {
const size_t URL_CAP = 4096;
const size_t NAME_CAP = 96;
const size_t RESPONSE_HEADER_CAP = 64 * 1024;
const size_t PACKAGE_HEADER_BYTES = 0x438, CONTENT_ID_BYTES = 36;
const uint32_t TLS_CHECKS = 0x01 | 0x04 | 0x08 | 0x10 | 0x20 | 0x80;
const int MAX_REDIRECTS = 5;

int g_state = IDLE, g_busy = 0, g_cancel = 0, g_error = 0, g_committed = 0;
uint64_t g_received = 0, g_total = 0;
int g_reqLock = 0, g_request = -1, g_parallelRequest = -1;
int g_stage = DOWNLOAD_STAGE_NONE, g_native = 0, g_network = 0, g_ssl = 0;
uint32_t g_sslDetails = 0;
int g_networkState = -1;
int g_transferMode = DOWNLOAD_MODE_CONNECTING, g_connections = 0;
char g_url[URL_CAP], g_filename[NAME_CAP], g_digest[65];
char g_contentId[CONTENT_ID_BYTES + 1];
uint64_t g_expected = 0;
int g_expectedKind = USER_PACKAGE_BASE;
// Only the worker owns its diagnostic file; URLs and redirect tokens are never
// written. Atomically published numeric diagnostics remain usable if logging fails.
FILE* g_log = 0;

void logCall(int stage, int32_t native, int32_t network = 0) {
    if (!g_log) return;
    int rc = fprintf(g_log, "stage=%d name=%s native=0x%08X net=0x%08X state=%d ssl=0x%08X detail=0x%08X\n",
        stage, downloadStageName(stage), (unsigned)native, (unsigned)network,
        __atomic_load_n(&g_networkState, __ATOMIC_ACQUIRE),
        (unsigned)__atomic_load_n(&g_ssl, __ATOMIC_ACQUIRE),
        (unsigned)__atomic_load_n(&g_sslDetails, __ATOMIC_ACQUIRE));
    if (rc < 0 || fflush(g_log) != 0) {
        fclose(g_log);
        g_log = 0;
    }
}
void stage(int value) { __atomic_store_n(&g_stage, value, __ATOMIC_RELEASE); }
int fail(int category, int where, int32_t native, int32_t network = 0) {
    stage(where);
    __atomic_store_n(&g_native, native, __ATOMIC_RELEASE);
    __atomic_store_n(&g_network, network, __ATOMIC_RELEASE);
    logCall(where, native, network);
    return category;
}
int32_t networkErrno() { int32_t* value = peppyNetErrnoLoc(); return value ? *value : 0; }

void requestLock() {
    while (__atomic_exchange_n(&g_reqLock, 1, __ATOMIC_ACQUIRE))
        sceKernelUsleep(1000);
}
void requestUnlock() { __atomic_store_n(&g_reqLock, 0, __ATOMIC_RELEASE); }
bool cancelled() { return __atomic_load_n(&g_cancel, __ATOMIC_ACQUIRE) != 0; }
void publishRequest(int request, int slot = 0) {
    requestLock();
    __atomic_store_n(slot ? &g_parallelRequest : &g_request, request, __ATOMIC_RELEASE);
    requestUnlock();
}
void abortRequests() {
    requestLock();
    const int requests[] = {g_request, g_parallelRequest};
    for (size_t i = 0; i < 2; ++i)
        if (requests[i] >= 0) sceHttpAbortRequest(requests[i]);
    requestUnlock();
}

size_t boundedLength(const char* value, size_t cap) {
    if (!value) return cap;
    size_t n = 0;
    while (n < cap && value[n]) ++n;
    return n;
}
char lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
bool equalNoCase(const char* a, size_t n, const char* b) {
    if (strlen(b) != n) return false;
    for (size_t i = 0; i < n; ++i) if (lower(a[i]) != lower(b[i])) return false;
    return true;
}
int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
bool safeFilename(const char* name) {
    size_t n = boundedLength(name, NAME_CAP);
    if (n < 5 || n == NAME_CAP || name[0] == '.' ||
        strcmp(name + n - 4, ".pkg") || strstr(name, "..")) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

bool canonicalContentId(const char* value) {
    if (boundedLength(value, CONTENT_ID_BYTES + 1) != CONTENT_ID_BYTES) return false;
    for (size_t i = 0; i < CONTENT_ID_BYTES; ++i) {
        char c = value[i];
        if (i == 6 || i == 19) { if (c != '-') return false; }
        else if (i == 16) { if (c != '_') return false; }
        else if (i < 2 || (i >= 7 && i < 11)) { if (c < 'A' || c > 'Z') return false; }
        else if ((i >= 2 && i < 6) || (i >= 11 && i < 16) || (i >= 17 && i < 19)) {
            if (c < '0' || c > '9') return false;
        } else if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9')) return false;
    }
    return true;
}
bool matchingExpectedHeader(const uint8_t* header, uint64_t required) {
    return userPackageHeaderMatches(header, PACKAGE_HEADER_BYTES, required, g_contentId, g_expectedKind);
}

// Exact hosts prevent credentials, ports, scheme downgrades and host suffix
// tricks. Release redirects use signed query strings, which are never logged.
bool githubUrl(const char* url, size_t* originLength = 0) {
    size_t n = boundedLength(url, URL_CAP);
    if (n == URL_CAP || n < 10 || strncmp(url, "https://", 8)) return false;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)url[i] <= 32 || (unsigned char)url[i] >= 127 ||
            url[i] == '#' || url[i] == '\\') return false;
    const char* start = url + 8;
    const char* end = strchr(start, '/');
    if (!end) return false;
    size_t hostLength = end - start;
    if (hostLength > 4 && !strncmp(end - 4, ":443", 4)) hostLength -= 4;
    const char* hosts[] = {"github.com", "raw.githubusercontent.com",
        "objects.githubusercontent.com", "release-assets.githubusercontent.com",
        "github-releases.githubusercontent.com"};
    bool allowed = false;
    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); ++i)
        if (equalNoCase(start, hostLength, hosts[i])) allowed = true;
    if (allowed && originLength) *originLength = end - url;
    return allowed;
}

bool mediafirePage(const char* url) {
    size_t length = boundedLength(url, URL_CAP);
    return length < URL_CAP && peppyMediafire::isPageUrl(url, length);
}
bool mediafireCdn(const char* url) {
    size_t length = boundedLength(url, URL_CAP);
    return length < URL_CAP && peppyMediafire::isCdnUrl(url, length);
}
bool archiveUrl(const char* url, size_t* originLength = 0) {
    size_t length = boundedLength(url, URL_CAP);
    if (length == URL_CAP || length < 10 || strncmp(url, "https://", 8)) return false;
    const char* host = url + 8;
    const char* path = strchr(host, '/');
    if (!path) return false;
    size_t hostLength = path - host;
    bool origin = equalNoCase(host, hostLength, "archive.org");
    bool cdn = peppyArchiveSources::approvedCdnHost(host, hostLength);
    if (!origin && !cdn) return false;

    // The catalog generator and native downloader share the exact reviewed
    // collection/host policy. Metadata pages and unobserved mirrors stay out.
    const char* file = path;
    if (origin) {
        const char prefix[] = "/download/";
        if (strncmp(file, prefix, sizeof(prefix) - 1)) return false;
        file += sizeof(prefix) - 1;
    } else {
        if (*file++ != '/') return false;
        size_t digits = 0;
        while (*file >= '0' && *file <= '9') { ++file; ++digits; }
        if (!digits || digits > 10) return false;
        const char prefix[] = "/items/";
        if (strncmp(file, prefix, sizeof(prefix) - 1)) return false;
        file += sizeof(prefix) - 1;
    }
    const char* collectionEnd = strchr(file, '/');
    if (!collectionEnd ||
        !peppyArchiveSources::approvedCollection(file, (size_t)(collectionEnd - file)))
        return false;
    file = collectionEnd + 1;
    size_t fileLength = length - (size_t)(file - url);
    if (fileLength <= 4 || strcmp(file + fileLength - 4, ".pkg")) return false;
    bool first = true;
    unsigned int continuation = 0;
    uint32_t codepoint = 0, minimum = 0;
    for (size_t i = 0; i < fileLength; ++i) {
        unsigned char value = (unsigned char)file[i];
        if (value == '%') {
            if (fileLength - i < 3 || hex(file[i + 1]) < 0 || hex(file[i + 2]) < 0)
                return false;
            value = (unsigned char)(hex(file[i + 1]) * 16 + hex(file[i + 2]));
            i += 2;
        } else if (value <= 32 || value >= 127) return false;
        if (continuation) {
            if ((value & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (value & 0x3f);
            if (!--continuation && (codepoint < minimum || codepoint > 0x10ffff ||
                (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
                (codepoint >= 0x7f && codepoint <= 0x9f))) return false;
            continue;
        }
        if (value >= 128) {
            if (value >= 0xc2 && value <= 0xdf) {
                continuation = 1; codepoint = value & 0x1f; minimum = 0x80;
            } else if (value >= 0xe0 && value <= 0xef) {
                continuation = 2; codepoint = value & 0x0f; minimum = 0x800;
            } else if (value >= 0xf0 && value <= 0xf4) {
                continuation = 3; codepoint = value & 0x07; minimum = 0x10000;
            } else return false;
            first = false;
            continue;
        }
        // A single basename permits embedded dots and encoded UTF-8, while
        // decoding cannot create separators, controls, or a second URL escape.
        if (value < 32 || value == 127 || value == '/' || value == '\\' ||
            value == '?' || value == '#' || value == '%' || value == '"' ||
            value == '<' || value == '>' || value == '`') return false;
        if (first && value == '.') return false;
        first = false;
    }
    if (continuation) return false;
    if (originLength) *originLength = path - url;
    return true;
}
bool gamebatoUrl(const char* url, size_t* originLength = 0) {
    // The public application PKG was verified at this single HTTPS location.
    // Do not extend this permission to the site's HTML or other downloads.
    const char expected[] = "https://gamebatoapp.ir/home/app.pkg";
    if (boundedLength(url, sizeof(expected)) != sizeof(expected) - 1 ||
        memcmp(url, expected, sizeof(expected) - 1)) return false;
    if (originLength) *originLength = sizeof("https://gamebatoapp.ir") - 1;
    return true;
}
bool safeUrl(const char* url, size_t* originLength = 0) {
    if (githubUrl(url, originLength) || archiveUrl(url, originLength) ||
        gamebatoUrl(url, originLength)) return true;
    size_t length = boundedLength(url, URL_CAP);
    if (length == URL_CAP) return false;
    return peppyMediafire::isPageUrl(url, length, originLength) ||
           peppyMediafire::isCdnUrl(url, length, originLength);
}
bool allowedRedirect(const char* current, const char* next) {
    // A source may redirect only within its own explicitly supported provider.
    // In particular, CDN URLs cannot redirect back into a landing page.
    if (githubUrl(current)) return githubUrl(next);
    if (archiveUrl(current)) return archiveUrl(next);
    if (gamebatoUrl(current)) return gamebatoUrl(next);
    if (mediafirePage(current)) return mediafirePage(next) || mediafireCdn(next);
    return mediafireCdn(current) && mediafireCdn(next);
}

bool redirectUrl(const char* current, const char* headers, size_t headerLength,
                 char* output) {
    if (!headers || !headerLength || headerLength > RESPONSE_HEADER_CAP) return false;
    const char* location = 0;
    size_t locationLength = 0;
    size_t pos = 0;
    while (pos < headerLength && headers[pos]) {
        size_t start = pos;
        while (pos < headerLength && headers[pos] && headers[pos] != '\n') ++pos;
        size_t end = pos;
        if (pos < headerLength && headers[pos] == '\n') ++pos;
        if (end > start && headers[end - 1] == '\r') --end;
        if (end - start >= 9 && equalNoCase(headers + start, 9, "Location:")) {
            if (location) return false;
            start += 9;
            while (start < end && (headers[start] == ' ' || headers[start] == '\t')) ++start;
            while (end > start && (headers[end - 1] == ' ' || headers[end - 1] == '\t')) --end;
            location = headers + start;
            locationLength = end - start;
        }
    }
    if (!location || !locationLength || locationLength >= URL_CAP) return false;
    size_t prefix = 0;
    if (location[0] == '/') {
        if (locationLength > 1 && location[1] == '/') return false;
        if (!safeUrl(current, &prefix)) return false;
    }
    if (prefix + locationLength >= URL_CAP) return false;
    if (prefix) memcpy(output, current, prefix);
    memcpy(output + prefix, location, locationLength);
    output[prefix + locationLength] = 0;
    return safeUrl(output) && allowedRedirect(current, output);
}

struct Sha256 {
    uint32_t state[8];
    uint8_t buffer[64];
    size_t used;
    uint64_t bytes;
    Sha256() : state{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}, used(0), bytes(0) {}
    static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* input) {
        static const uint32_t k[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)input[4*i] << 24) | ((uint32_t)input[4*i+1] << 16) |
                   ((uint32_t)input[4*i+2] << 8) | input[4*i+3];
        for (int i = 16; i < 64; ++i) {
            uint32_t a = w[i-15], b = w[i-2];
            w[i] = w[i-16] + (rotr(a,7) ^ rotr(a,18) ^ (a >> 3)) +
                   w[i-7] + (rotr(b,17) ^ rotr(b,19) ^ (b >> 10));
        }
        uint32_t a=state[0],b=state[1],c=state[2],d=state[3],
                 e=state[4],f=state[5],g=state[6],h=state[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1=h+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            uint32_t t2=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));
            h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;
        state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
    }
    void update(const uint8_t* input, size_t n) {
        bytes += n;
        while (n) {
            size_t take = 64 - used;
            if (take > n) take = n;
            memcpy(buffer + used, input, take);
            used += take;input += take;n -= take;
            if (used == 64) { block(buffer); used = 0; }
        }
    }
    void finish(uint8_t output[32]) {
        uint64_t bits = bytes * 8;
        buffer[used++] = 0x80;
        if (used > 56) { memset(buffer + used, 0, 64 - used); block(buffer); used = 0; }
        memset(buffer + used, 0, 56 - used);
        for (int i = 0; i < 8; ++i) buffer[63-i] = (uint8_t)(bits >> (8*i));
        block(buffer);
        for (int i = 0; i < 32; ++i) output[i] = (uint8_t)(state[i/4] >> (24 - 8*(i%4)));
    }
};

bool ensureDirectory(const char* path) {
    if (mkdir(path, 0777) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}
bool ensureDownloadDirectory() {
    char path[256];
    size_t n = boundedLength(PEPPY_DOWNLOAD_DIRECTORY, sizeof(path));
    if (!n || n == sizeof(path) || PEPPY_DOWNLOAD_DIRECTORY[0] != '/') return false;
    memcpy(path, PEPPY_DOWNLOAD_DIRECTORY, n + 1);
    for (size_t i = 1; i <= n; ++i) {
        if (path[i] != '/' && path[i] != 0) continue;
        char saved = path[i];
        path[i] = 0;
        bool okay = ensureDirectory(path);
        path[i] = saved;
        if (!okay) return false;
    }
    return true;
}
bool removePartial(const char* path) { return unlink(path) == 0 || errno == ENOENT; }
int transferError(int request, int where, int32_t native) {
    int32_t error = 0;
    uint32_t detail = 0;
    int32_t sslRc = peppyHttpsGetSslError(request, &error, &detail);
    __atomic_store_n(&g_ssl, sslRc == 0 ? error : sslRc, __ATOMIC_RELEASE);
    __atomic_store_n(&g_sslDetails, detail, __ATOMIC_RELEASE);
    int32_t net = 0;
    if (sceHttpGetLastErrno(request, &net) < 0) net = networkErrno();
    int category = (uint32_t)native == 0x80431073U ? DOWNLOAD_ERROR_RESPONSE_HEADERS :
                   sslRc == 0 && (error || detail) ? DOWNLOAD_ERROR_TLS : DOWNLOAD_ERROR_NETWORK;
    return fail(category, where, native, net);
}

int ensureModule(OrbisSysModuleInternal module, int where) {
    stage(where);
    if (peppySysmoduleIsLoaded(module) == 0) { logCall(where, 0); return 0; }
    int32_t rc = (int32_t)sceSysmoduleLoadModuleInternal(module);
    logCall(where, rc);
    if (rc < 0 && peppySysmoduleIsLoaded(module) != 0)
        return fail(DOWNLOAD_ERROR_NETWORK, where, rc);
    return 0;
}

struct HttpHandles {
    int net, ssl, http, tmpl, conn, req, slot;
    explicit HttpHandles(int requestSlot = 0) : net(-1), ssl(-1), http(-1), tmpl(-1), conn(-1), req(-1), slot(requestSlot) {}
    void closeRequest(bool abort = false) {
        requestLock();
        __atomic_store_n(slot ? &g_parallelRequest : &g_request, -1, __ATOMIC_RELEASE);
        if (abort && req >= 0) sceHttpAbortRequest(req);
        requestUnlock();
        if (req >= 0) { sceHttpDeleteRequest(req); req = -1; }
        if (conn >= 0) { sceHttpDeleteConnection(conn); conn = -1; }
    }
    ~HttpHandles() {
        closeRequest();
        if (tmpl >= 0) sceHttpDeleteTemplate(tmpl);
        if (http >= 0) sceHttpTerm(http);
        if (ssl >= 0) peppySslTerm(ssl);
        if (net >= 0) sceNetPoolDestroy(net);
        // The process may share module/network initialization with future
        // features. Do not tear down global network or unload their modules.
    }
};

int initHttpContext(HttpHandles& handles, const char* name) {
    stage(DOWNLOAD_STAGE_NET_POOL);
    handles.net = sceNetPoolCreate(name, 1024 * 1024, 0);
    if (handles.net < 0) return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_NET_POOL, handles.net, networkErrno());
    logCall(DOWNLOAD_STAGE_NET_POOL, handles.net);
    stage(DOWNLOAD_STAGE_SSL_INIT);
    handles.ssl = sceSslInit(256 * 1024);
    if (handles.ssl < 0) return fail(DOWNLOAD_ERROR_TLS, DOWNLOAD_STAGE_SSL_INIT, handles.ssl);
    stage(DOWNLOAD_STAGE_HTTP_INIT);
    handles.http = sceHttpInit(handles.net, handles.ssl, 1024 * 1024);
    if (handles.http < 0) return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_HTTP_INIT, handles.http);
    stage(DOWNLOAD_STAGE_TEMPLATE);
    handles.tmpl = sceHttpCreateTemplate(handles.http, "PeppyStore/1.0", ORBIS_HTTP_VERSION_1_1, 0);
    if (handles.tmpl < 0) return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_TEMPLATE, handles.tmpl);
    int32_t rc;
    // GitHub security and signed-release redirect headers exceed libSceHttp's
    // small default. Configure a finite cap before connections inherit it;
    // the redirect parser enforces the same cap independently.
    stage(DOWNLOAD_STAGE_HEADER_LIMIT);
    rc = peppyHttpSetResponseHeaderMaxSize(handles.tmpl, RESPONSE_HEADER_CAP);
    if (rc < 0) return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_HEADER_LIMIT, rc);
    stage(DOWNLOAD_STAGE_TLS_OPTIONS);
    rc = sceHttpsEnableOption(handles.tmpl, TLS_CHECKS);
    if (rc < 0) return fail(DOWNLOAD_ERROR_TLS, DOWNLOAD_STAGE_TLS_OPTIONS, rc);
    stage(DOWNLOAD_STAGE_REDIRECT_OPTION);
    rc = peppyHttpSetAutoRedirect(handles.tmpl, 0);
    if (rc < 0) return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_REDIRECT_OPTION, rc);
    stage(DOWNLOAD_STAGE_RESOLVE_TIMEOUT);
    rc = sceHttpSetResolveTimeOut(handles.tmpl, 10000000);
    if (rc < 0) return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_RESOLVE_TIMEOUT, rc);
    stage(DOWNLOAD_STAGE_CONNECT_TIMEOUT);
    rc = sceHttpSetConnectTimeOut(handles.tmpl, 10000000);
    if (rc < 0) return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_CONNECT_TIMEOUT, rc);
    stage(DOWNLOAD_STAGE_SEND_TIMEOUT);
    rc = sceHttpSetSendTimeOut(handles.tmpl, 15000000);
    if (rc < 0) return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_SEND_TIMEOUT, rc);
    stage(DOWNLOAD_STAGE_RECV_TIMEOUT);
    rc = peppyHttpSetRecvTimeOut(handles.tmpl, 15000000);
    if (rc < 0) return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_RECV_TIMEOUT, rc);

    return 0;
}

int resolveMediafire(int request, char* next) {
    stage(DOWNLOAD_STAGE_SOURCE_READ);
    int32_t lengthType = -1;
    size_t declaredLength = 0;
    int32_t rc = sceHttpGetResponseContentLength(request, &lengthType, &declaredLength);
    if (rc < 0) return transferError(request, DOWNLOAD_STAGE_SOURCE_READ, rc);
    bool knownLength = rc == 0 && lengthType == ORBIS_HTTP_CONTENTLEN_EXIST;
    if (knownLength && declaredLength > peppyMediafire::HTML_CAP)
        return fail(DOWNLOAD_ERROR_SOURCE, DOWNLOAD_STAGE_SOURCE_READ, 0);
    size_t capacity = knownLength ? declaredLength : peppyMediafire::HTML_CAP;
    struct HtmlBuffer {
        char* text;
        explicit HtmlBuffer(size_t size) : text(static_cast<char*>(malloc(size + 1))) {}
        ~HtmlBuffer() { free(text); }
    } html(capacity);
    if (!html.text) return fail(DOWNLOAD_ERROR_SOURCE, DOWNLOAD_STAGE_SOURCE_READ, ENOMEM);
    size_t used = 0;
    unsigned char chunk[16384];
    while (!cancelled()) {
        // Content-Length frames the body independently of connection closure.
        if (knownLength && used == declaredLength) break;
        size_t remaining = capacity - used;
        size_t requested = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        // An unknown-length response still needs EOF, including at the cap.
        if (!requested) requested = 1;
        rc = sceHttpReadData(request, chunk, requested);
        if (rc < 0) return transferError(request, DOWNLOAD_STAGE_SOURCE_READ, rc);
        if (!rc) break;
        if (static_cast<size_t>(rc) > requested || static_cast<size_t>(rc) > remaining)
            return fail(DOWNLOAD_ERROR_SOURCE, DOWNLOAD_STAGE_SOURCE_READ, 0);
        memcpy(html.text + used, chunk, static_cast<size_t>(rc));
        used += static_cast<size_t>(rc);
    }
    if (cancelled()) return 0;
    if (knownLength && used != declaredLength)
        return fail(DOWNLOAD_ERROR_SOURCE, DOWNLOAD_STAGE_SOURCE_READ, 0);
    html.text[used] = 0;
    stage(DOWNLOAD_STAGE_SOURCE_PARSE);
    if (!peppyMediafire::extractUrl(html.text, used, next, URL_CAP) || !mediafireCdn(next))
        return fail(DOWNLOAD_ERROR_SOURCE, DOWNLOAD_STAGE_SOURCE_PARSE, 0);
    return 0;
}

enum ParallelAttempt { PARALLEL_KEEP_RESPONSE, PARALLEL_RESTART_SINGLE, PARALLEL_FINISHED };
void clearAttemptDiagnostics() {
    __atomic_store_n(&g_native, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_network, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ssl, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_sslDetails, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_received, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_transferMode, DOWNLOAD_MODE_CONNECTING, __ATOMIC_RELEASE);
    __atomic_store_n(&g_connections, 0, __ATOMIC_RELEASE);
}
bool openRange(HttpHandles& handles, const char* url, uint64_t first, uint64_t last,
               uint64_t total, const peppyHttpRange::Metadata& seed) {
    stage(DOWNLOAD_STAGE_CONNECTION);
    handles.conn = sceHttpCreateConnectionWithURL(handles.tmpl, url, false);
    if (handles.conn < 0) { fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_CONNECTION, handles.conn); return false; }
    stage(DOWNLOAD_STAGE_REQUEST);
    handles.req = sceHttpCreateRequestWithURL(handles.conn, ORBIS_METHOD_GET, url, 0);
    if (handles.req < 0) { fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_REQUEST, handles.req); return false; }
    publishRequest(handles.req, handles.slot);
    if (cancelled()) return false;
    char range[80];
    int n = snprintf(range, sizeof(range), "bytes=%llu-%llu", (unsigned long long)first, (unsigned long long)last);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(range)) return false;
    stage(DOWNLOAD_STAGE_REQUEST_HEADER);
    int32_t rc = sceHttpAddRequestHeader(handles.req, "Accept-Encoding", "identity", 0);
    if (!rc) rc = sceHttpAddRequestHeader(handles.req, "Range", range, 0);
    if (!rc) rc = sceHttpAddRequestHeader(handles.req, "If-Range", seed.etag, 0);
    if (rc < 0) { fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_REQUEST_HEADER, rc); return false; }
    stage(DOWNLOAD_STAGE_SEND);
    rc = sceHttpSendRequest(handles.req, 0, 0);
    if (rc < 0) { transferError(handles.req, DOWNLOAD_STAGE_SEND, rc); return false; }
    if (cancelled()) return false;
    int32_t status = 0;
    rc = sceHttpGetStatusCode(handles.req, &status);
    if (rc < 0) { fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_STATUS, rc); return false; }
    if (status != 206) { logCall(DOWNLOAD_STAGE_STATUS, status); return false; }
    char* headers = 0; size_t length = 0;
    rc = sceHttpGetAllResponseHeaders(handles.req, &headers, &length);
    if (rc < 0) { fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_HEADERS, rc); return false; }
    peppyHttpRange::Metadata metadata;
    if (!peppyHttpRange::parseHeaders(headers, length, &metadata) ||
        !peppyHttpRange::matchesRange(metadata, first, last, total) ||
        !peppyHttpRange::sameStrongEtag(seed, metadata)) return false;
    int32_t type = -1; size_t bytes = 0;
    rc = sceHttpGetResponseContentLength(handles.req, &type, &bytes);
    if (rc < 0 || type != ORBIS_HTTP_CONTENTLEN_EXIST || bytes != last - first + 1) {
        logCall(DOWNLOAD_STAGE_CONTENT_LENGTH, rc); return false;
    }
    return !cancelled();
}
int32_t parallelRead(void* value, void* buffer, size_t bytes) {
    return sceHttpReadData(static_cast<HttpHandles*>(value)->req, buffer, static_cast<uint32_t>(bytes));
}
void parallelReadFailure(void* value, int32_t native, peppyParallelDownload::Failure* failure) {
    int request = static_cast<HttpHandles*>(value)->req;
    int32_t ssl = 0; uint32_t details = 0;
    int32_t sslRc = peppyHttpsGetSslError(request, &ssl, &details);
    int32_t network = 0;
    if (sceHttpGetLastErrno(request, &network) < 0) network = networkErrno();
    failure->category = static_cast<uint32_t>(native) == 0x80431073U ? DOWNLOAD_ERROR_RESPONSE_HEADERS :
        sslRc == 0 && (ssl || details) ? DOWNLOAD_ERROR_TLS : DOWNLOAD_ERROR_NETWORK;
    failure->where = DOWNLOAD_STAGE_READ; failure->native = native;
    failure->network = network; failure->ssl = sslRc == 0 ? ssl : sslRc; failure->sslDetails = details;
}
bool parallelCancelled(void*) { return cancelled(); }
void parallelAbort(void*) { abortRequests(); }
void parallelProgress(void*, uint64_t bytes) { __atomic_add_fetch(&g_received, bytes, __ATOMIC_ACQ_REL); }
struct StreamContext {
    HttpHandles* handles;
    FILE* file;
    uint64_t required;
    uint8_t magic[4], packageHeader[PACKAGE_HEADER_BYTES];
    size_t magicCount, packageHeaderCount;
    Sha256 hash;
    StreamContext(HttpHandles& h, FILE* f, uint64_t length)
        : handles(&h), file(f), required(length), magic{0,0,0,0}, magicCount(0), packageHeaderCount(0) {}
};
int32_t streamRead(void* value, void* buffer, size_t bytes) {
    StreamContext& context = *static_cast<StreamContext*>(value);
    stage(DOWNLOAD_STAGE_READ);
    return sceHttpReadData(context.handles->req, buffer, static_cast<uint32_t>(bytes));
}
void streamReadFailure(void* value, int32_t native, peppyStreamDownload::Failure* failure) {
    parallelReadFailure(static_cast<StreamContext*>(value)->handles, native, failure);
}
bool streamValidate(void* value, const void* data, size_t bytes, peppyStreamDownload::Failure* failure) {
    StreamContext& context = *static_cast<StreamContext*>(value);
    const uint8_t* buffer = static_cast<const uint8_t*>(data);
    if (!bytes && g_contentId[0] && context.packageHeaderCount != PACKAGE_HEADER_BYTES) {
        *failure = peppyStreamDownload::detail::simpleFailure(DOWNLOAD_ERROR_PACKAGE, DOWNLOAD_STAGE_PACKAGE, 0);
        return false;
    }
    for (size_t i = 0; i < bytes && context.magicCount < 4; ++i)
        context.magic[context.magicCount++] = buffer[i];
    if (context.magicCount == 4 && (context.magic[0] != 0x7f || context.magic[1] != 0x43 ||
        context.magic[2] != 0x4e || context.magic[3] != 0x54)) {
        *failure = peppyStreamDownload::detail::simpleFailure(DOWNLOAD_ERROR_PACKAGE, DOWNLOAD_STAGE_PACKAGE, 0);
        return false;
    }
    if (g_contentId[0] && context.packageHeaderCount < PACKAGE_HEADER_BYTES) {
        size_t take = PACKAGE_HEADER_BYTES - context.packageHeaderCount;
        if (take > bytes) take = bytes;
        memcpy(context.packageHeader + context.packageHeaderCount, buffer, take);
        context.packageHeaderCount += take;
        if (context.packageHeaderCount == PACKAGE_HEADER_BYTES && !matchingExpectedHeader(context.packageHeader, context.required)) {
            *failure = peppyStreamDownload::detail::simpleFailure(DOWNLOAD_ERROR_PACKAGE, DOWNLOAD_STAGE_PACKAGE, 0);
            return false;
        }
    }
    return true;
}
bool streamConsume(void* value, const void* data, size_t bytes, peppyStreamDownload::Failure* failure) {
    StreamContext& context = *static_cast<StreamContext*>(value);
    errno = 0;
    if (fwrite(data, 1, bytes, context.file) != bytes) {
        *failure = peppyStreamDownload::detail::simpleFailure(DOWNLOAD_ERROR_FILESYSTEM,
            DOWNLOAD_STAGE_FILE_WRITE, errno ? errno : EIO);
        return false;
    }
    if (g_digest[0]) context.hash.update(static_cast<const uint8_t*>(data), bytes);
    return true;
}
void streamMode(void*, bool threaded) {
    __atomic_store_n(&g_transferMode, threaded ? DOWNLOAD_MODE_PIPELINED : DOWNLOAD_MODE_SINGLE, __ATOMIC_RELEASE);
    __atomic_store_n(&g_connections, 1, __ATOMIC_RELEASE);
}
int reportParallelFailure(const peppyParallelDownload::Failure& value) {
    __atomic_store_n(&g_ssl, value.ssl, __ATOMIC_RELEASE);
    __atomic_store_n(&g_sslDetails, value.sslDetails, __ATOMIC_RELEASE);
    return fail(value.category, value.where, value.native, value.network);
}
ParallelAttempt tryParallel(HttpHandles& primary, const char* url, uint64_t required,
                            const char* partPath, const char* finalPath, int& result) {
    result = 0;
    char* headers = 0; size_t headerBytes = 0;
    int32_t rc = sceHttpGetAllResponseHeaders(primary.req, &headers, &headerBytes);
    peppyHttpRange::Metadata seed;
    // A normal GET with no usable validator stays on its existing body stream.
    if (rc < 0 || !peppyHttpRange::parseHeaders(headers, headerBytes, &seed) || !seed.hasStrongEtag)
        return PARALLEL_KEEP_RESPONSE;
    uint8_t packageHeader[PACKAGE_HEADER_BYTES]; size_t received = 0;
    while (!cancelled() && received < PACKAGE_HEADER_BYTES) {
        size_t requested = PACKAGE_HEADER_BYTES - received;
        stage(DOWNLOAD_STAGE_READ);
        rc = sceHttpReadData(primary.req, packageHeader + received, static_cast<uint32_t>(requested));
        if (rc < 0) { result = transferError(primary.req, DOWNLOAD_STAGE_READ, rc); return PARALLEL_FINISHED; }
        if (!rc || static_cast<size_t>(rc) > requested) {
            result = fail(DOWNLOAD_ERROR_PACKAGE, DOWNLOAD_STAGE_PACKAGE, rc); return PARALLEL_FINISHED;
        }
        received += static_cast<size_t>(rc);
    }
    if (cancelled()) return PARALLEL_FINISHED;
    if (!matchingExpectedHeader(packageHeader, required)) {
        result = fail(DOWNLOAD_ERROR_PACKAGE, DOWNLOAD_STAGE_PACKAGE, 0); return PARALLEL_FINISHED;
    }
    primary.closeRequest(true);
    HttpHandles secondary(1);
    if (initHttpContext(secondary, "peppy-range")) return PARALLEL_RESTART_SINGLE;
    const uint64_t middle = required / 2;
    if (!openRange(primary, url, 0, middle - 1, required, seed) ||
        !openRange(secondary, url, middle, required - 1, required, seed)) {
        primary.closeRequest(true); secondary.closeRequest(true);
        return cancelled() ? PARALLEL_FINISHED : PARALLEL_RESTART_SINGLE;
    }
    stage(DOWNLOAD_STAGE_FILE_OPEN);
    int fd = open(partPath, O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_OPEN, errno);
        primary.closeRequest(true); secondary.closeRequest(true); return PARALLEL_FINISHED;
    }
    peppyParallelDownload::Plan plan = {};
    plan.fd = fd; plan.context = 0; plan.cancelled = parallelCancelled;
    plan.abort = parallelAbort; plan.progress = parallelProgress;
    plan.lanes[0] = {&primary, parallelRead, parallelReadFailure, 0, middle};
    plan.lanes[1] = {&secondary, parallelRead, parallelReadFailure, middle, required - middle};
    stage(DOWNLOAD_STAGE_READ);
    __atomic_store_n(&g_transferMode, DOWNLOAD_MODE_RANGED, __ATOMIC_RELEASE);
    __atomic_store_n(&g_connections, 2, __ATOMIC_RELEASE);
    peppyParallelDownload::Outcome outcome = peppyParallelDownload::run(plan);
    primary.closeRequest(true); secondary.closeRequest(true);
    // The helper returns only once every worker's last access has finished.
    // Before any data thread started, setup failures can safely use one stream.
    if (!outcome.started && !cancelled()) {
        reportParallelFailure(outcome.failure);
        if (close(fd) != 0) {
            result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_CLOSE, errno);
            removePartial(partPath); return PARALLEL_FINISHED;
        }
        if (!removePartial(partPath)) {
            result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_CLEANUP, errno); return PARALLEL_FINISHED;
        }
        return PARALLEL_RESTART_SINGLE;
    }
    if (outcome.failure.category) result = reportParallelFailure(outcome.failure);
    if (!result && !cancelled()) {
        PeppyParallelFileInfo info = {};
        int32_t statRc = peppyParallelFstat(fd, &info);
        if (statRc) result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_PACKAGE, statRc);
        else if (info.size < 0 || static_cast<uint64_t>(info.size) != required ||
                 (info.mode & 0170000) != 0100000 ||
                 __atomic_load_n(&g_received, __ATOMIC_ACQUIRE) != required)
            result = fail(DOWNLOAD_ERROR_LENGTH, DOWNLOAD_STAGE_PACKAGE, 0);
    }
    if (!result && !cancelled()) {
        // Verify the header actually assembled by the range responses too.
        // The initial validator probe never substitutes for the published PKG.
        stage(DOWNLOAD_STAGE_PACKAGE);
        size_t offset = 0; unsigned interruptions = 0;
        while (!result && !cancelled() && offset < PACKAGE_HEADER_BYTES) {
            size_t count = PACKAGE_HEADER_BYTES - offset;
            int64_t bytes = peppyParallelPread(fd, packageHeader + offset, count, static_cast<int64_t>(offset));
            if (static_cast<uint32_t>(bytes) == 0x80020004U && interruptions++ < 8) continue;
            if (bytes <= 0 || static_cast<uint64_t>(bytes) > count) {
                result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_PACKAGE,
                    bytes ? static_cast<int32_t>(bytes) : EIO); break;
            }
            interruptions = 0; offset += static_cast<size_t>(bytes);
        }
        if (!result && !cancelled() && !matchingExpectedHeader(packageHeader, required))
            result = fail(DOWNLOAD_ERROR_PACKAGE, DOWNLOAD_STAGE_PACKAGE, 0);
    }
    if (!result && !cancelled() && g_digest[0]) {
        stage(DOWNLOAD_STAGE_HASH);
        peppyDownloadBuffers::Buffers buffers(false);
        uint8_t* buffer = buffers.first;
        if (!buffers.ready()) result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_HASH, ENOMEM);
        Sha256 hash; uint64_t offset = 0; unsigned interruptions = 0;
        while (buffer && !result && !cancelled() && offset < required) {
            size_t count = required - offset < buffers.capacity ? static_cast<size_t>(required - offset) : buffers.capacity;
            int64_t bytes = peppyParallelPread(fd, buffer, count, static_cast<int64_t>(offset));
            if (static_cast<uint32_t>(bytes) == 0x80020004U && interruptions++ < 8) continue;
            if (bytes <= 0 || static_cast<uint64_t>(bytes) > count) {
                result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_HASH, bytes ? static_cast<int32_t>(bytes) : EIO); break;
            }
            interruptions = 0;
            hash.update(buffer, static_cast<size_t>(bytes)); offset += static_cast<uint64_t>(bytes);
        }
        if (!result && !cancelled()) {
            uint8_t digest[32]; hash.finish(digest);
            for (int i = 0; i < 32; ++i)
                if (digest[i] != static_cast<uint8_t>((hex(g_digest[2*i]) << 4) | hex(g_digest[2*i+1])))
                    result = fail(DOWNLOAD_ERROR_HASH, DOWNLOAD_STAGE_HASH, 0);
        }
    }
    if (!result && !cancelled() && fsync(fd) != 0)
        result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_FLUSH, errno);
    if (close(fd) != 0 && !result)
        result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_CLOSE, errno);
    if (!result) {
        requestLock();
        if (!cancelled()) {
            if (rename(partPath, finalPath) != 0) result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_RENAME, errno);
            else __atomic_store_n(&g_committed, 1, __ATOMIC_RELEASE);
        }
        requestUnlock();
    }
    if ((result || cancelled()) && !removePartial(partPath) && !result)
        result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_CLEANUP, errno);
    return PARALLEL_FINISHED;
}

int runTransfer() {
    if (cancelled()) return 0;
    stage(DOWNLOAD_STAGE_DIRECTORY);
    if (!ensureDownloadDirectory()) return fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_DIRECTORY, errno);
    char logPath[256];
    int logLength = snprintf(logPath, sizeof(logPath), "%s/download.log", PEPPY_DOWNLOAD_DIRECTORY);
    if (logLength > 0 && (size_t)logLength < sizeof(logPath)) g_log = fopen(logPath, "w");
    char finalPath[256], partPath[264];
    int a = snprintf(finalPath, sizeof(finalPath), "%s/%s", PEPPY_DOWNLOAD_DIRECTORY, g_filename);
    int b = snprintf(partPath, sizeof(partPath), "%s.part", finalPath);
    if (a < 0 || (size_t)a >= sizeof(finalPath) || b < 0 || (size_t)b >= sizeof(partPath))
        return fail(DOWNLOAD_ERROR_SPEC, DOWNLOAD_STAGE_SPEC, 0);
    if (!removePartial(partPath)) return fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_CLEANUP, errno);

    HttpHandles handles;
    const OrbisSysModuleInternal modules[] = {ORBIS_SYSMODULE_INTERNAL_NET,
        ORBIS_SYSMODULE_INTERNAL_NETCTL, ORBIS_SYSMODULE_INTERNAL_SSL, ORBIS_SYSMODULE_INTERNAL_HTTP};
    const int moduleStages[] = {DOWNLOAD_STAGE_MODULE_NET, DOWNLOAD_STAGE_MODULE_NETCTL,
        DOWNLOAD_STAGE_MODULE_SSL, DOWNLOAD_STAGE_MODULE_HTTP};
    for (size_t i = 0; i < sizeof(modules)/sizeof(modules[0]); ++i) {
        int rc = ensureModule(modules[i], moduleStages[i]);
        if (rc) return rc;
    }
    stage(DOWNLOAD_STAGE_NETCTL_INIT);
    int32_t ctlRc = sceNetCtlInit();
    logCall(DOWNLOAD_STAGE_NETCTL_INIT, ctlRc);
    int32_t networkState = -1;
    int32_t stateRc = peppyNetCtlGetState(&networkState);
    __atomic_store_n(&g_networkState, networkState, __ATOMIC_RELEASE);
    // A usable state query confirms a preexisting NetCtl context. Do not
    // whitelist guessed "already initialized" error constants.
    if (ctlRc != 0 && stateRc != 0)
        return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_NETCTL_INIT, ctlRc);
    stage(DOWNLOAD_STAGE_NETCTL_STATE);
    if (stateRc != 0) return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_NETCTL_STATE, stateRc);
    // State 3 is IP_OBTAINED. This checks local readiness and never runs a PSN
    // connection test, logs the local address, or changes connection settings.
    for (int attempt = 0; networkState != 3 && attempt < 100; ++attempt) {
        if (cancelled()) return 0;
        sceKernelUsleep(100000);
        stateRc = peppyNetCtlGetState(&networkState);
        __atomic_store_n(&g_networkState, networkState, __ATOMIC_RELEASE);
        if (stateRc != 0) return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_NETCTL_STATE, stateRc);
    }
    logCall(DOWNLOAD_STAGE_NETCTL_STATE, stateRc);
    if (networkState != 3) return fail(DOWNLOAD_ERROR_NOT_READY, DOWNLOAD_STAGE_NETCTL_STATE, stateRc);

    stage(DOWNLOAD_STAGE_NET_INIT);
    int32_t netRc = sceNetInit();
    int32_t netErr = netRc != 0 ? networkErrno() : 0;
    logCall(DOWNLOAD_STAGE_NET_INIT, netRc, netErr);
    // Official OpenOrbis/Apollo and SSPI validate usable pool creation after
    // sceNetInit. Its original return is retained in the diagnostic log even
    // when a preexisting global network lets pool creation succeed.
    int32_t rc = initHttpContext(handles, "peppy-download");
    if (rc) return rc;

    char current[URL_CAP], next[URL_CAP];
    memcpy(current, g_url, strlen(g_url) + 1);
    int status = 0;
    bool sourceResolved = false;
    bool parallelAttempted = false;
    int redirects = 0;
openResponse:
    for (int hop = redirects;; ++hop) {
        if (cancelled()) return 0;
        stage(DOWNLOAD_STAGE_CONNECTION);
        handles.conn = sceHttpCreateConnectionWithURL(handles.tmpl, current, false);
        if (handles.conn < 0) return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_CONNECTION, handles.conn);
        stage(DOWNLOAD_STAGE_REQUEST);
        handles.req = sceHttpCreateRequestWithURL(handles.conn, ORBIS_METHOD_GET, current, 0);
        if (handles.req < 0) return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_REQUEST, handles.req);
        publishRequest(handles.req, handles.slot);
        if (cancelled()) return 0;
        stage(DOWNLOAD_STAGE_REQUEST_HEADER);
        rc = sceHttpAddRequestHeader(handles.req, "Accept-Encoding", "identity", 0);
        if (rc < 0) return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_REQUEST_HEADER, rc);
        stage(DOWNLOAD_STAGE_SEND);
        rc = sceHttpSendRequest(handles.req, 0, 0);
        if (rc < 0) return transferError(handles.req, DOWNLOAD_STAGE_SEND, rc);
        if (cancelled()) return 0;
        stage(DOWNLOAD_STAGE_STATUS);
        rc = sceHttpGetStatusCode(handles.req, &status);
        if (rc < 0) return fail(DOWNLOAD_ERROR_NETWORK, DOWNLOAD_STAGE_STATUS, rc);
        logCall(DOWNLOAD_STAGE_STATUS, status);
        if (status == 200) {
            if (!mediafirePage(current)) break;
            // Landing-page resolution consumes the same bounded hop budget
            // as HTTP redirects. It never changes package progress or writes
            // the HTML body to the PKG's partial file.
            if (sourceResolved || hop >= MAX_REDIRECTS)
                return fail(DOWNLOAD_ERROR_SOURCE, DOWNLOAD_STAGE_SOURCE_PARSE, 0);
            int sourceResult = resolveMediafire(handles.req, next);
            if (sourceResult || cancelled()) return sourceResult;
            sourceResolved = true;
            ++redirects;
            handles.closeRequest();
            memcpy(current, next, strlen(next) + 1);
            continue;
        }
        if (!(status == 301 || status == 302 || status == 303 || status == 307 || status == 308))
            return fail(DOWNLOAD_ERROR_HTTP, DOWNLOAD_STAGE_STATUS, status);
        if (hop >= MAX_REDIRECTS) return fail(DOWNLOAD_ERROR_REDIRECT, DOWNLOAD_STAGE_HEADERS, 0);
        char* responseHeaders = 0;
        size_t responseHeaderLength = 0;
        stage(DOWNLOAD_STAGE_HEADERS);
        rc = sceHttpGetAllResponseHeaders(handles.req, &responseHeaders, &responseHeaderLength);
        if (rc < 0) return fail(DOWNLOAD_ERROR_REDIRECT, DOWNLOAD_STAGE_HEADERS, rc);
        if (responseHeaderLength > RESPONSE_HEADER_CAP)
            return fail(DOWNLOAD_ERROR_RESPONSE_HEADERS, DOWNLOAD_STAGE_HEADERS, rc);
        if (!redirectUrl(current, responseHeaders, responseHeaderLength, next))
            return fail(DOWNLOAD_ERROR_REDIRECT, DOWNLOAD_STAGE_HEADERS, 0);
        handles.closeRequest();
        ++redirects;
        memcpy(current, next, strlen(next) + 1);
    }

    int32_t lengthType = -1;
    size_t responseLength = 0;
    stage(DOWNLOAD_STAGE_CONTENT_LENGTH);
    int lengthRc = sceHttpGetResponseContentLength(handles.req, &lengthType, &responseLength);
    logCall(DOWNLOAD_STAGE_CONTENT_LENGTH, lengthRc);
    bool knownLength = lengthRc == 0 && lengthType == ORBIS_HTTP_CONTENTLEN_EXIST;
    if (knownLength && (responseLength < 4 || responseLength > PEPPY_MAX_PACKAGE_BYTES ||
        (g_expected && responseLength != g_expected))) return fail(DOWNLOAD_ERROR_LENGTH, DOWNLOAD_STAGE_CONTENT_LENGTH, 0);
    if (!g_expected && !knownLength) return fail(DOWNLOAD_ERROR_LENGTH, DOWNLOAD_STAGE_CONTENT_LENGTH, lengthRc);
    const uint64_t required = g_expected ? g_expected : responseLength;
    __atomic_store_n(&g_total, required, __ATOMIC_RELEASE);
    if (cancelled()) return 0;
    if (!parallelAttempted && knownLength && required >= 32ULL * 1024 * 1024 &&
        g_contentId[0] && (archiveUrl(current) || githubUrl(current) || mediafireCdn(current))) {
        parallelAttempted = true;
        int parallelResult = 0;
        ParallelAttempt attempt = tryParallel(handles, current, required, partPath, finalPath, parallelResult);
        if (attempt == PARALLEL_FINISHED) return parallelResult;
        if (attempt == PARALLEL_RESTART_SINGLE) {
            handles.closeRequest(true);
            if (cancelled()) return 0;
            clearAttemptDiagnostics();
            // Restart the already validated final URL, keeping the original
            // redirect budget and never reusing a partially consumed body.
            goto openResponse;
        }
    }
    stage(DOWNLOAD_STAGE_FILE_OPEN);
    peppyDownloadBuffers::Buffers buffers;
    if (!buffers.ready())
        return fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_OPEN, ENOMEM);
    FILE* file = fopen(partPath, "wb");
    if (!file) { int code = errno; removePartial(partPath); return fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_OPEN, code); }
    int result = 0;
    // Full queued blocks bypass most stdio buffering. A small caller-owned
    // buffer handles tails; it remains alive through both fflush and fclose.
    // The two larger buffers can now alternate network reception and writes.
    char stdioBuffer[16 * 1024];
    errno = 0;
    if (setvbuf(file, stdioBuffer, _IOFBF, sizeof(stdioBuffer)) != 0)
        result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_OPEN, errno ? errno : EIO);
    StreamContext stream(handles, file, required);
    if (!result && !cancelled()) {
        peppyStreamDownload::Plan plan = {};
        plan.buffers = &buffers; plan.context = &stream;
        plan.read = streamRead; plan.captureReadError = streamReadFailure;
        plan.validate = streamValidate; plan.consume = streamConsume;
        plan.cancelled = parallelCancelled; plan.abort = parallelAbort; plan.mode = streamMode;
        plan.progress = parallelProgress;
        plan.length = required; plan.framed = knownLength;
        const peppyStreamDownload::Outcome outcome = peppyStreamDownload::run(plan);
        if (outcome.failure.category) result = reportParallelFailure(outcome.failure);
    }
    const uint64_t received = __atomic_load_n(&g_received, __ATOMIC_ACQUIRE);
    if (!result && !cancelled() && g_contentId[0] && stream.packageHeaderCount != PACKAGE_HEADER_BYTES)
        result = fail(DOWNLOAD_ERROR_PACKAGE, DOWNLOAD_STAGE_PACKAGE, 0);
    if (!result && !cancelled() && (received != required || stream.magicCount != 4 ||
        (knownLength && received != responseLength))) result = fail(DOWNLOAD_ERROR_LENGTH, DOWNLOAD_STAGE_READ, 0);
    if (!result && !cancelled() && g_digest[0]) {
        uint8_t digest[32];
        stream.hash.finish(digest);
        for (int i = 0; i < 32; ++i)
            if (digest[i] != (uint8_t)((hex(g_digest[2*i]) << 4) | hex(g_digest[2*i+1])))
                result = fail(DOWNLOAD_ERROR_HASH, DOWNLOAD_STAGE_HASH, 0);
    }
    if (fflush(file) != 0 || ferror(file)) {
        if (!result) result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_FLUSH, errno);
    }
    if (fclose(file) != 0) {
        if (!result) result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_CLOSE, errno);
    }
    if (!result) {
        // Cancellation and the final rename use the same lock. A cancellation
        // cannot turn an already committed, complete download into CANCELLED.
        requestLock();
        if (!cancelled()) {
            if (rename(partPath, finalPath) != 0) result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_RENAME, errno);
            else __atomic_store_n(&g_committed, 1, __ATOMIC_RELEASE);
        }
        requestUnlock();
    }
    if (result || cancelled()) {
        if (!removePartial(partPath) && !result)
            result = fail(DOWNLOAD_ERROR_FILESYSTEM, DOWNLOAD_STAGE_FILE_CLEANUP, errno);
    }
    return result;
}

void* downloadWorker(void*) {
    int result = runTransfer();
    if (!result && !cancelled()) stage(DOWNLOAD_STAGE_FINISHED);
    logCall(__atomic_load_n(&g_stage, __ATOMIC_ACQUIRE),
            __atomic_load_n(&g_native, __ATOMIC_ACQUIRE),
            __atomic_load_n(&g_network, __ATOMIC_ACQUIRE));
    if (g_log) { fflush(g_log); fclose(g_log); g_log = 0; }
    __atomic_store_n(&g_error, result, __ATOMIC_RELEASE);
    __atomic_store_n(&g_state, cancelled() ? CANCELLED : result ? FAILED : DONE, __ATOMIC_RELEASE);
    __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
    return 0;
}
} // namespace

bool startDownload(const DownloadSpec& spec, const char* expectedContentId, int expectedKind) {
    int expected = 0;
    if (!__atomic_compare_exchange_n(&g_busy, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return false;
    __atomic_store_n(&g_cancel, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_committed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_received, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_total, spec.expectedBytes, __ATOMIC_RELEASE);
    __atomic_store_n(&g_error, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_stage, DOWNLOAD_STAGE_NONE, __ATOMIC_RELEASE);
    __atomic_store_n(&g_native, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_network, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ssl, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_sslDetails, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_networkState, -1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_transferMode, DOWNLOAD_MODE_CONNECTING, __ATOMIC_RELEASE);
    __atomic_store_n(&g_connections, 0, __ATOMIC_RELEASE);
    g_contentId[0] = 0;
    bool verifyContentId = expectedContentId && expectedContentId[0];
    bool digestOkay = !spec.sha256 || !spec.sha256[0];
    if (!digestOkay && boundedLength(spec.sha256, 65) == 64) {
        digestOkay = true;
        for (int i = 0; i < 64; ++i) if (hex(spec.sha256[i]) < 0) digestOkay = false;
    }
    if (!safeUrl(spec.url) || !safeFilename(spec.filename) || !digestOkay ||
        expectedKind < USER_PACKAGE_BASE || expectedKind > USER_PACKAGE_DLC ||
        (expectedKind != USER_PACKAGE_BASE && !verifyContentId) ||
        (spec.expectedBytes && spec.expectedBytes < 4) || spec.expectedBytes > PEPPY_MAX_PACKAGE_BYTES ||
        (verifyContentId && (!canonicalContentId(expectedContentId) || spec.expectedBytes < PACKAGE_HEADER_BYTES))) {
        __atomic_store_n(&g_error, DOWNLOAD_ERROR_SPEC, __ATOMIC_RELEASE);
        stage(DOWNLOAD_STAGE_SPEC);
        __atomic_store_n(&g_state, FAILED, __ATOMIC_RELEASE);
        __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
        return false;
    }
    memcpy(g_url, spec.url, strlen(spec.url) + 1);
    memcpy(g_filename, spec.filename, strlen(spec.filename) + 1);
    g_digest[0] = 0;
    if (spec.sha256 && spec.sha256[0]) memcpy(g_digest, spec.sha256, 65);
    if (verifyContentId) memcpy(g_contentId, expectedContentId, CONTENT_ID_BYTES + 1);
    g_expected = spec.expectedBytes;
    g_expectedKind = expectedKind;
    __atomic_store_n(&g_state, RUNNING, __ATOMIC_RELEASE);

    OrbisPthreadAttr attr;
    int32_t attrRc = scePthreadAttrInit(&attr);
    bool attrReady = attrRc == 0;
    OrbisPthread thread;
    // Native pthread detach state: 1 = detached, 0 = joinable.
    int32_t rc = attrReady ? scePthreadAttrSetdetachstate(&attr, 1) : attrRc;
    if (rc == 0) rc = scePthreadCreate(&thread, &attr, downloadWorker, 0, "peppy-download");
    if (attrReady) scePthreadAttrDestroy(&attr);
    if (rc != 0) {
        fail(DOWNLOAD_ERROR_THREAD, DOWNLOAD_STAGE_THREAD, rc);
        __atomic_store_n(&g_error, DOWNLOAD_ERROR_THREAD, __ATOMIC_RELEASE);
        __atomic_store_n(&g_state, FAILED, __ATOMIC_RELEASE);
        __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
        return false;
    }
    return true;
}

void cancelDownload() {
    if (!__atomic_load_n(&g_busy, __ATOMIC_ACQUIRE)) return;
    requestLock();
    if (__atomic_load_n(&g_busy, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&g_committed, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&g_cancel, 1, __ATOMIC_RELEASE);
        int request = __atomic_load_n(&g_request, __ATOMIC_ACQUIRE);
        if (request >= 0) sceHttpAbortRequest(request);
        request = __atomic_load_n(&g_parallelRequest, __ATOMIC_ACQUIRE);
        if (request >= 0) sceHttpAbortRequest(request);
    }
    requestUnlock();
}

DownloadSnapshot downloadSnapshot() {
    DownloadSnapshot snapshot;
    snapshot.state = __atomic_load_n(&g_state, __ATOMIC_ACQUIRE);
    snapshot.received = __atomic_load_n(&g_received, __ATOMIC_ACQUIRE);
    snapshot.total = __atomic_load_n(&g_total, __ATOMIC_ACQUIRE);
    snapshot.errorCode = __atomic_load_n(&g_error, __ATOMIC_ACQUIRE);
    snapshot.stage = __atomic_load_n(&g_stage, __ATOMIC_ACQUIRE);
    snapshot.nativeCode = __atomic_load_n(&g_native, __ATOMIC_ACQUIRE);
    snapshot.networkCode = __atomic_load_n(&g_network, __ATOMIC_ACQUIRE);
    snapshot.sslCode = __atomic_load_n(&g_ssl, __ATOMIC_ACQUIRE);
    snapshot.sslDetails = __atomic_load_n(&g_sslDetails, __ATOMIC_ACQUIRE);
    snapshot.networkState = __atomic_load_n(&g_networkState, __ATOMIC_ACQUIRE);
    snapshot.transferMode = __atomic_load_n(&g_transferMode, __ATOMIC_ACQUIRE);
    snapshot.connections = __atomic_load_n(&g_connections, __ATOMIC_ACQUIRE);
    return snapshot;
}

const char* downloadStageName(int value) {
    static const char* names[] = {"Aguardando", "Pasta de downloads", "Módulo de rede",
        "Módulo SSL", "Módulo HTTP", "Módulo NetCtl", "Inicializar NetCtl", "Estado da rede",
        "Inicializar rede", "Memória de rede", "Inicializar SSL", "Inicializar HTTP",
        "Configurar HTTP", "Validação TLS", "Redirecionamentos", "Prazo de DNS",
        "Prazo de conexão", "Prazo de envio", "Prazo de leitura", "Criar conexão",
        "Criar requisição", "Cabeçalho HTTP", "Envio HTTPS", "Resposta HTTP",
        "Destino HTTPS", "Tamanho do arquivo", "Abrir arquivo", "Receber arquivo",
        "Validar PKG", "Validar SHA-256", "Gravar arquivo", "Finalizar gravação",
        "Fechar arquivo", "Salvar PKG", "Limpar parcial", "Concluído", "Iniciar tarefa", "Dados do download", "Limite de cabeçalhos",
        "Página da fonte", "Resolver link do PKG"};
    return value >= 0 && (size_t)value < sizeof(names)/sizeof(names[0]) ? names[value] : "Download";
}
