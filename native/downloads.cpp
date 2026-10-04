#include "downloads.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
#include <orbis/Http.h>

// OpenOrbis 0.5.x leaves these functions declared void(). The aliases retain
// their native symbols while supplying the completed public PS4 signatures.
extern "C" int32_t peppyHttpSetAutoRedirect(int32_t, int32_t)
    __asm__("sceHttpSetAutoRedirect");
extern "C" int32_t peppyHttpSetRecvTimeOut(int32_t, uint32_t)
    __asm__("sceHttpSetRecvTimeOut");
extern "C" int32_t peppyHttpsGetSslError(int32_t, int32_t*, uint32_t*)
    __asm__("sceHttpsGetSslError");
extern "C" int32_t peppySslTerm(int32_t) __asm__("sceSslTerm");

#ifndef PEPPY_DOWNLOAD_DIRECTORY
#define PEPPY_DOWNLOAD_DIRECTORY "/data/peppy-store/downloads"
#endif

namespace {
const size_t URL_CAP = 4096;
const size_t NAME_CAP = 96;
const uint64_t MAX_PACKAGE_BYTES = 4ULL * 1024 * 1024 * 1024;
const uint32_t TLS_CHECKS = 0x01 | 0x04 | 0x08 | 0x10 | 0x20 | 0x80;
const int MAX_REDIRECTS = 5;

int g_state = IDLE, g_busy = 0, g_cancel = 0, g_error = 0, g_committed = 0;
uint64_t g_received = 0, g_total = 0;
int g_reqLock = 0, g_request = -1;
char g_url[URL_CAP], g_filename[NAME_CAP], g_digest[65];
uint64_t g_expected = 0;

void requestLock() {
    while (__atomic_exchange_n(&g_reqLock, 1, __ATOMIC_ACQUIRE))
        sceKernelUsleep(1000);
}
void requestUnlock() { __atomic_store_n(&g_reqLock, 0, __ATOMIC_RELEASE); }
bool cancelled() { return __atomic_load_n(&g_cancel, __ATOMIC_ACQUIRE) != 0; }
void publishRequest(int request) {
    requestLock();
    __atomic_store_n(&g_request, request, __ATOMIC_RELEASE);
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

// Exact hosts prevent credentials, ports, scheme downgrades and host suffix
// tricks. Release redirects use signed query strings, which are never logged.
bool safeUrl(const char* url, size_t* originLength = 0) {
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

bool redirectUrl(const char* current, const char* headers, size_t headerLength,
                 char* output) {
    if (!headers || !headerLength || headerLength > 32768) return false;
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
    return safeUrl(output);
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
int transferError(int request) {
    int32_t error = 0;
    uint32_t detail = 0;
    if (peppyHttpsGetSslError(request, &error, &detail) == 0 && (error || detail))
        return DOWNLOAD_ERROR_TLS;
    return DOWNLOAD_ERROR_NETWORK;
}

struct HttpHandles {
    int net, ssl, http, tmpl, conn, req;
    HttpHandles() : net(-1), ssl(-1), http(-1), tmpl(-1), conn(-1), req(-1) {}
    void closeRequest() {
        publishRequest(-1);
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

int runTransfer() {
    if (cancelled()) return 0;
    if (!ensureDownloadDirectory()) return DOWNLOAD_ERROR_FILESYSTEM;
    char finalPath[256], partPath[264];
    int a = snprintf(finalPath, sizeof(finalPath), "%s/%s", PEPPY_DOWNLOAD_DIRECTORY, g_filename);
    int b = snprintf(partPath, sizeof(partPath), "%s.part", finalPath);
    if (a < 0 || (size_t)a >= sizeof(finalPath) || b < 0 || (size_t)b >= sizeof(partPath))
        return DOWNLOAD_ERROR_SPEC;
    if (!removePartial(partPath)) return DOWNLOAD_ERROR_FILESYSTEM;

    HttpHandles handles;
    const OrbisSysModuleInternal modules[] = {ORBIS_SYSMODULE_INTERNAL_NET,
        ORBIS_SYSMODULE_INTERNAL_SSL, ORBIS_SYSMODULE_INTERNAL_HTTP};
    for (size_t i = 0; i < sizeof(modules)/sizeof(modules[0]); ++i)
        if ((int32_t)sceSysmoduleLoadModuleInternal(modules[i]) < 0)
            return DOWNLOAD_ERROR_NETWORK;
    // sceNetInit is global; an already initialized network is usable as well.
    // Pool creation below is the definitive check that it is available.
    sceNetInit();
    handles.net = sceNetPoolCreate("peppy-download", 64 * 1024, 0);
    if (handles.net < 0) return DOWNLOAD_ERROR_NETWORK;
    handles.ssl = sceSslInit(256 * 1024);
    if (handles.ssl < 0) return DOWNLOAD_ERROR_TLS;
    handles.http = sceHttpInit(handles.net, handles.ssl, 1024 * 1024);
    if (handles.http < 0) return DOWNLOAD_ERROR_NETWORK;
    handles.tmpl = sceHttpCreateTemplate(handles.http, "PeppyStore/1.0", ORBIS_HTTP_VERSION_1_1, 0);
    if (handles.tmpl < 0) return DOWNLOAD_ERROR_NETWORK;
    if (sceHttpsEnableOption(handles.tmpl, TLS_CHECKS) < 0) return DOWNLOAD_ERROR_TLS;
    if (peppyHttpSetAutoRedirect(handles.tmpl, 0) < 0 ||
        sceHttpSetResolveTimeOut(handles.tmpl, 10000000) < 0 ||
        sceHttpSetConnectTimeOut(handles.tmpl, 10000000) < 0 ||
        sceHttpSetSendTimeOut(handles.tmpl, 15000000) < 0 ||
        peppyHttpSetRecvTimeOut(handles.tmpl, 15000000) < 0) return DOWNLOAD_ERROR_NETWORK;

    char current[URL_CAP], next[URL_CAP];
    memcpy(current, g_url, strlen(g_url) + 1);
    int status = 0;
    for (int hop = 0;; ++hop) {
        if (cancelled()) return 0;
        handles.conn = sceHttpCreateConnectionWithURL(handles.tmpl, current, false);
        if (handles.conn < 0) return DOWNLOAD_ERROR_NETWORK;
        handles.req = sceHttpCreateRequestWithURL(handles.conn, ORBIS_METHOD_GET, current, 0);
        if (handles.req < 0) return DOWNLOAD_ERROR_NETWORK;
        publishRequest(handles.req);
        if (cancelled()) return 0;
        if (sceHttpAddRequestHeader(handles.req, "Accept-Encoding", "identity", 0) < 0)
            return DOWNLOAD_ERROR_NETWORK;
        if (sceHttpSendRequest(handles.req, 0, 0) < 0) return transferError(handles.req);
        if (cancelled()) return 0;
        if (sceHttpGetStatusCode(handles.req, &status) < 0) return DOWNLOAD_ERROR_NETWORK;
        if (status == 200) break;
        if (!(status == 301 || status == 302 || status == 303 || status == 307 || status == 308))
            return DOWNLOAD_ERROR_HTTP;
        if (hop >= MAX_REDIRECTS) return DOWNLOAD_ERROR_REDIRECT;
        char* responseHeaders = 0;
        size_t responseHeaderLength = 0;
        if (sceHttpGetAllResponseHeaders(handles.req, &responseHeaders, &responseHeaderLength) < 0 ||
            !redirectUrl(current, responseHeaders, responseHeaderLength, next)) return DOWNLOAD_ERROR_REDIRECT;
        handles.closeRequest();
        memcpy(current, next, strlen(next) + 1);
    }

    int32_t lengthType = -1;
    size_t responseLength = 0;
    int lengthRc = sceHttpGetResponseContentLength(handles.req, &lengthType, &responseLength);
    bool knownLength = lengthRc == 0 && lengthType == ORBIS_HTTP_CONTENTLEN_EXIST;
    if (knownLength && (responseLength < 4 || responseLength > MAX_PACKAGE_BYTES ||
        (g_expected && responseLength != g_expected))) return DOWNLOAD_ERROR_LENGTH;
    if (!g_expected && !knownLength) return DOWNLOAD_ERROR_LENGTH;
    const uint64_t required = g_expected ? g_expected : responseLength;
    __atomic_store_n(&g_total, required, __ATOMIC_RELEASE);
    if (cancelled()) return 0;
    FILE* file = fopen(partPath, "wb");
    if (!file) { removePartial(partPath); return DOWNLOAD_ERROR_FILESYSTEM; }
    int result = 0;
    uint64_t received = 0;
    uint8_t buffer[65536], magic[4] = {0,0,0,0};
    size_t magicCount = 0;
    Sha256 hash;
    while (!cancelled()) {
        int32_t got = sceHttpReadData(handles.req, buffer, sizeof(buffer));
        if (got < 0) { result = transferError(handles.req); break; }
        if (got == 0) break;
        if ((size_t)got > sizeof(buffer) || (uint64_t)got > required - received) {
            result = DOWNLOAD_ERROR_LENGTH; break;
        }
        for (int32_t i = 0; i < got && magicCount < 4; ++i) magic[magicCount++] = buffer[i];
        if (magicCount == 4 && (magic[0] != 0x7f || magic[1] != 0x43 ||
            magic[2] != 0x4e || magic[3] != 0x54)) { result = DOWNLOAD_ERROR_PACKAGE; break; }
        if (fwrite(buffer, 1, (size_t)got, file) != (size_t)got) {
            result = DOWNLOAD_ERROR_FILESYSTEM; break;
        }
        hash.update(buffer, (size_t)got);
        received += (uint64_t)got;
        __atomic_store_n(&g_received, received, __ATOMIC_RELEASE);
    }
    if (!result && !cancelled() && (received != required || magicCount != 4 ||
        (knownLength && received != responseLength))) result = DOWNLOAD_ERROR_LENGTH;
    if (!result && !cancelled() && g_digest[0]) {
        uint8_t digest[32];
        hash.finish(digest);
        for (int i = 0; i < 32; ++i)
            if (digest[i] != (uint8_t)((hex(g_digest[2*i]) << 4) | hex(g_digest[2*i+1])))
                result = DOWNLOAD_ERROR_HASH;
    }
    if (fflush(file) != 0 || ferror(file)) result = DOWNLOAD_ERROR_FILESYSTEM;
    if (fclose(file) != 0) result = DOWNLOAD_ERROR_FILESYSTEM;
    if (!result) {
        // Cancellation and the final rename use the same lock. A cancellation
        // cannot turn an already committed, complete download into CANCELLED.
        requestLock();
        if (!cancelled()) {
            if (rename(partPath, finalPath) != 0) result = DOWNLOAD_ERROR_FILESYSTEM;
            else __atomic_store_n(&g_committed, 1, __ATOMIC_RELEASE);
        }
        requestUnlock();
    }
    if (result || cancelled()) {
        if (!removePartial(partPath)) result = DOWNLOAD_ERROR_FILESYSTEM;
    }
    return result;
}

void* downloadWorker(void*) {
    int result = runTransfer();
    __atomic_store_n(&g_error, result, __ATOMIC_RELEASE);
    __atomic_store_n(&g_state, cancelled() ? CANCELLED : result ? FAILED : DONE, __ATOMIC_RELEASE);
    __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
    return 0;
}
} // namespace

bool startDownload(const DownloadSpec& spec) {
    int expected = 0;
    if (!__atomic_compare_exchange_n(&g_busy, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return false;
    __atomic_store_n(&g_cancel, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_committed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_received, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_total, spec.expectedBytes, __ATOMIC_RELEASE);
    __atomic_store_n(&g_error, 0, __ATOMIC_RELEASE);
    bool digestOkay = !spec.sha256 || !spec.sha256[0];
    if (!digestOkay && boundedLength(spec.sha256, 65) == 64) {
        digestOkay = true;
        for (int i = 0; i < 64; ++i) if (hex(spec.sha256[i]) < 0) digestOkay = false;
    }
    if (!safeUrl(spec.url) || !safeFilename(spec.filename) || !digestOkay ||
        (spec.expectedBytes && spec.expectedBytes < 4) || spec.expectedBytes > MAX_PACKAGE_BYTES) {
        __atomic_store_n(&g_error, DOWNLOAD_ERROR_SPEC, __ATOMIC_RELEASE);
        __atomic_store_n(&g_state, FAILED, __ATOMIC_RELEASE);
        __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
        return false;
    }
    memcpy(g_url, spec.url, strlen(spec.url) + 1);
    memcpy(g_filename, spec.filename, strlen(spec.filename) + 1);
    g_digest[0] = 0;
    if (spec.sha256 && spec.sha256[0]) memcpy(g_digest, spec.sha256, 65);
    g_expected = spec.expectedBytes;
    __atomic_store_n(&g_state, RUNNING, __ATOMIC_RELEASE);

    OrbisPthreadAttr attr;
    bool attrReady = scePthreadAttrInit(&attr) == 0;
    OrbisPthread thread;
    // Native pthread detach state: 1 = detached, 0 = joinable.
    int32_t rc = attrReady ? scePthreadAttrSetdetachstate(&attr, 1) : -1;
    if (rc == 0) rc = scePthreadCreate(&thread, &attr, downloadWorker, 0, "peppy-download");
    if (attrReady) scePthreadAttrDestroy(&attr);
    if (rc != 0) {
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
    }
    requestUnlock();
}

DownloadSnapshot downloadSnapshot() {
    DownloadSnapshot snapshot;
    snapshot.state = __atomic_load_n(&g_state, __ATOMIC_ACQUIRE);
    snapshot.received = __atomic_load_n(&g_received, __ATOMIC_ACQUIRE);
    snapshot.total = __atomic_load_n(&g_total, __ATOMIC_ACQUIRE);
    snapshot.errorCode = __atomic_load_n(&g_error, __ATOMIC_ACQUIRE);
    return snapshot;
}
