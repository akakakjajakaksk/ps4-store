// Integration tests use independent native HTTP contexts and real host FDs.
static const char* parallelTestDirectory();
#define PEPPY_DOWNLOAD_DIRECTORY parallelTestDirectory()
#include "../../downloads.cpp"
#include <assert.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mock {
uint64_t BYTES = 32ULL * 1024 * 1024;
const char CID[] = "UP0000-CUSA99998_00-PEPPYTEST0000000";
const char URL[] = "https://archive.org/download/ps4-fpkg-collection-english-h/Parallel%20Fixture.pkg";
const char* activeUrl = URL;
const size_t LARGE_BUFFER_BYTES = 1024 * 1024;
const size_t SMALL_BUFFER_BYTES = 256 * 1024;
const int32_t READ_ERROR = static_cast<int32_t>(0x80431068U);
const int32_t JOIN_ERROR = static_cast<int32_t>(0x80020016U);
const int32_t WRITE_ERROR = static_cast<int32_t>(0x8002001cU);
enum Mode {
    GOOD, NO_ETAG, WEAK_ETAG, IGNORE_RANGE, RANGE_503, RANGE_REDIRECT, WRONG_ETAG,
    WRONG_RANGE, WRONG_RANGE_TOTAL, WRONG_LENGTH, DUPLICATE_ETAG,
    POOL_FAILURE, ALLOCATION_FAILURE, PREFERRED_FIRST_FAILURE, PREFERRED_SECOND_FAILURE,
    HASH_PREFERRED_FAILURE, HASH_ALLOCATION_FAILURE, THREAD_FAILURE, BODY_FAILURE,
    CANCEL_BODY, SHORT_WRITES, WRITE_FAILURE, FSYNC_FAILURE, CLOSE_FAILURE,
    JOIN_FAILURE, HASH_MISMATCH, HEADER_MISMATCH, FRAGMENTED_BODY,
    INTERRUPTED_WRITES, INTERRUPTED_PREAD, PWRITE_EINTR_EXHAUSTED,
    PREAD_EINTR_EXHAUSTED, ZERO_WRITE, OVERSIZED_WRITE, EARLY_EOF,
    READ_OVERSIZED, RANGE_HEADER_CORRUPT, FSTAT_FAILURE, WRONG_FILE_SIZE,
    NOT_REGULAR_FILE
};
struct Resource {
    int parent;
    size_t headerCap;
    uint32_t tls;
    bool redirectDisabled;
    std::string url;
    Resource(int p = -1) : parent(p), headerCap(0), tls(0), redirectDisabled(false) {}
};
struct Request {
    int connection, id;
    std::string url, range, ifRange, headers;
    uint64_t first, last, position, length;
    int status;
    bool sent, ranged, aborted, firstRead, deleted, blockReported;
    Request(int c, int n, const char* u)
        : connection(c), id(n), url(u), first(0), last(BYTES - 1), position(0),
          length(BYTES), status(200), sent(false), ranged(false), aborted(false),
          firstRead(true), deleted(false), blockReported(false) {}
};
struct Thread {
    std::thread* host;
    void* result;
    bool joinable;
    Thread() : host(0), result(0), joinable(false) {}
};
std::mutex lock;
std::condition_variable wake;
std::map<int, Resource> pools, ssl, http, templates, connections;
std::map<int, std::shared_ptr<Request> > requests;
std::map<int, std::shared_ptr<Thread> > threads;
std::vector<std::shared_ptr<Request> > history;
std::vector<uint8_t> payload;
Mode mode = GOOD;
int nextId = 100, poolCreates = 0, liveJoinable = 0;
int joins = 0, joinErrors = 0, rangeReadCalls = 0, singleReadCalls = 0;
int activeRangeReads = 0, peakRangeReads = 0, enteredRangeRequests = 0;
int blockedRangeRequests = 0;
int pwriteCalls = 0, fsyncCalls = 0, partCloses = 0, partDeletes = 0;
uint64_t pwriteBytes = 0, highestPublished = 0;
std::atomic<int> rangedSent(0), allocatorFailures(0), preferredAllocations(0);
size_t hashReadMax = 0;
std::atomic<int> pwriteInterrupts(0), preadInterrupts(0);
bool joinErrorPublished = false;
bool secondaryCloseError = false;
int netError = 0;

std::string path(bool partial = false) {
    return std::string(parallelTestDirectory()) + "/parallel.pkg" + (partial ? ".part" : "");
}
bool isPartFd(int fd) {
    char link[64], target[512];
    int n = snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(link)) return false;
    ssize_t got = readlink(link, target, sizeof(target) - 1);
    if (got < 0) return false;
    target[got] = 0;
    return path(true) == target;
}
int allocate() { return ++nextId; }
std::shared_ptr<Request> request(int id) {
    std::map<int, std::shared_ptr<Request> >::iterator it = requests.find(id);
    assert(it != requests.end() && !it->second->deleted);
    return it->second;
}
void put32(size_t offset, uint32_t value) {
    for (int n = 0; n < 4; ++n) payload[offset + n] = static_cast<uint8_t>(value >> (24 - 8 * n));
}
void makePayload() {
    assert(strlen(CID) == 36);
    payload.resize(BYTES);
    for (size_t n = 0; n < payload.size(); ++n)
        payload[n] = static_cast<uint8_t>((n * 29 + (n >> 12) * 7) & 255);
    memset(payload.data(), 0, 0x438);
    payload[0] = 0x7f; payload[1] = 0x43; payload[2] = 0x4e; payload[3] = 0x54;
    memcpy(payload.data() + 0x40, CID, 36);
    put32(0x74, 0x1a); put32(0x78, 0x0a000000);
    for (int n = 0; n < 8; ++n) payload[0x430 + n] = static_cast<uint8_t>(BYTES >> (56 - 8 * n));
}
void reset(Mode next) {
    assert(!__atomic_load_n(&g_busy, __ATOMIC_ACQUIRE));
    std::lock_guard<std::mutex> guard(lock);
    assert(requests.empty() && connections.empty() && templates.empty() && http.empty());
    assert(pools.empty() && ssl.empty() && threads.empty() && !liveJoinable);
    mode = next; nextId = 100; poolCreates = joins = joinErrors = 0;
    rangeReadCalls = singleReadCalls = activeRangeReads = peakRangeReads = enteredRangeRequests = blockedRangeRequests = 0;
    pwriteCalls = fsyncCalls = partCloses = partDeletes = 0;
    pwriteBytes = highestPublished = 0; rangedSent = 0; allocatorFailures = preferredAllocations = 0;
    hashReadMax = 0; activeUrl = URL;
    joinErrorPublished = false; secondaryCloseError = false;
    pwriteInterrupts = preadInterrupts = 0; BYTES = 32ULL * 1024 * 1024;
    netError = 0; history.clear();
    unlink(path().c_str()); unlink(path(true).c_str());
    makePayload();
    if (next == HEADER_MISMATCH) payload[0x40] = 'E';
}
void seedPrevious() {
    FILE* f = fopen(path().c_str(), "wb"); assert(f);
    assert(fwrite("previous", 1, 8, f) == 8); assert(fclose(f) == 0);
}
void checkPrevious() {
    FILE* f = fopen(path().c_str(), "rb"); assert(f);
    char data[9] = {}; assert(fread(data, 1, 8, f) == 8);
    assert(!strcmp(data, "previous")); assert(fclose(f) == 0);
}
DownloadSnapshot finish() {
    uint64_t previous = 0;
    for (int tries = 0; tries < 20000; ++tries) {
        DownloadSnapshot s = downloadSnapshot();
        assert(s.received <= BYTES && s.total <= BYTES);
        assert(s.received >= previous);
        previous = s.received;
        if (s.received > highestPublished) highestPublished = s.received;
        if (!__atomic_load_n(&g_busy, __ATOMIC_ACQUIRE)) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!"download worker failed to finish"); return downloadSnapshot();
}
void clean() {
    std::lock_guard<std::mutex> guard(lock);
    assert(requests.empty() && connections.empty() && templates.empty() && http.empty());
    assert(pools.empty() && ssl.empty() && threads.empty() && !liveJoinable);
    assert(!activeRangeReads && access(path(true).c_str(), F_OK) != 0);
    for (size_t n = 0; n < history.size(); ++n) assert(history[n]->deleted);
}
void exactOutput() {
    FILE* f = fopen(path().c_str(), "rb"); assert(f);
    std::vector<uint8_t> chunk(256 * 1024);
    uint64_t at = 0;
    while (at < BYTES) {
        size_t n = fread(chunk.data(), 1, chunk.size(), f);
        assert(n && n <= BYTES - at);
        assert(!memcmp(chunk.data(), payload.data() + at, n)); at += n;
    }
    assert(fgetc(f) == EOF && !ferror(f)); assert(fclose(f) == 0);
}
std::string digest() {
    Sha256 hash; hash.update(payload.data(), payload.size());
    uint8_t bytes[32]; hash.finish(bytes); char text[65];
    for (int n = 0; n < 32; ++n) snprintf(text + n * 2, 3, "%02x", bytes[n]);
    return text;
}
int rangeAborts() {
    int n = 0; for (size_t i = 0; i < history.size(); ++i)
        if (history[i]->ranged && history[i]->aborted) ++n;
    return n;
}
bool shouldBlock() { return mode == CANCEL_BODY; }
} // namespace mock

static const char* parallelTestDirectory() {
    static char value[] = "/tmp/peppy-parallel-tests-XXXXXX";
    static const char* directory = mkdtemp(value); assert(directory); return directory;
}

extern "C" void* __real_malloc(size_t);
extern "C" int __real_close(int);
extern "C" int __real_fsync(int);
extern "C" int __real_fclose(FILE*);
extern "C" int __real_fflush(FILE*);
extern "C" void* __wrap_malloc(size_t count) {
    if ((count == mock::LARGE_BUFFER_BYTES || count == mock::SMALL_BUFFER_BYTES) && mock::rangedSent.load() == 2) {
        const int stage = downloadSnapshot().stage;
        bool fail = (stage == DOWNLOAD_STAGE_READ && mock::mode == mock::ALLOCATION_FAILURE) ||
                    (stage == DOWNLOAD_STAGE_HASH && mock::mode == mock::HASH_ALLOCATION_FAILURE);
        if (stage == DOWNLOAD_STAGE_READ && count == mock::LARGE_BUFFER_BYTES) {
            int allocation = mock::preferredAllocations.fetch_add(1);
            fail = fail || (mock::mode == mock::PREFERRED_FIRST_FAILURE && allocation == 0) ||
                           (mock::mode == mock::PREFERRED_SECOND_FAILURE && allocation == 1);
        }
        if (stage == DOWNLOAD_STAGE_HASH && count == mock::LARGE_BUFFER_BYTES && mock::mode == mock::HASH_PREFERRED_FAILURE)
            fail = true;
        if (fail) { ++mock::allocatorFailures; errno = ENOMEM; return 0; }
    }
    return __real_malloc(count);
}
extern "C" int __wrap_close(int fd) {
    bool part = mock::isPartFd(fd);
    if (part) {
        std::lock_guard<std::mutex> guard(mock::lock);
        assert(!mock::liveJoinable); ++mock::partCloses;
    }
    int rc = __real_close(fd);
    if (part && (mock::mode == mock::CLOSE_FAILURE || mock::secondaryCloseError)) { errno = EIO; return -1; }
    return rc;
}
extern "C" int __wrap_fsync(int fd) {
    bool part = mock::isPartFd(fd);
    if (part) {
        std::lock_guard<std::mutex> guard(mock::lock);
        assert(!mock::liveJoinable); ++mock::fsyncCalls;
    }
    if (part && mock::mode == mock::FSYNC_FAILURE) { errno = ENOSPC; return -1; }
    return __real_fsync(fd);
}
extern "C" int __wrap_fclose(FILE* f) {
    bool part = mock::isPartFd(fileno(f));
    if (part) { std::lock_guard<std::mutex> guard(mock::lock); assert(!mock::liveJoinable); }
    int rc = __real_fclose(f);
    if (part && (mock::mode == mock::CLOSE_FAILURE || mock::secondaryCloseError)) { errno = EIO; return EOF; }
    return rc;
}
extern "C" int __wrap_fflush(FILE* f) { return __real_fflush(f); }

extern "C" int64_t testPwrite(int32_t, const void*, size_t, int64_t) __asm__("sceKernelPwrite");
extern "C" int64_t testPwrite(int32_t fd, const void* data, size_t bytes, int64_t offset) {
    assert(offset >= 0 && static_cast<uint64_t>(offset) <= mock::BYTES);
    assert(bytes <= mock::BYTES - static_cast<uint64_t>(offset));
    {
        std::lock_guard<std::mutex> guard(mock::lock);
        ++mock::pwriteCalls;
        if (mock::mode == mock::WRITE_FAILURE) return mock::WRITE_ERROR;
    }
    if (mock::mode == mock::PWRITE_EINTR_EXHAUSTED ||
        (mock::mode == mock::INTERRUPTED_WRITES && mock::pwriteInterrupts.fetch_add(1) < 2))
        return static_cast<int32_t>(0x80020004U);
    if (mock::mode == mock::ZERO_WRITE) return 0;
    if (mock::mode == mock::OVERSIZED_WRITE) return static_cast<int64_t>(bytes + 1);
    if (mock::mode == mock::SHORT_WRITES && bytes > 137) bytes = 137;
    ssize_t rc = pwrite(fd, data, bytes, static_cast<off_t>(offset));
    if (rc > 0) { std::lock_guard<std::mutex> guard(mock::lock); mock::pwriteBytes += rc; }
    return rc < 0 ? -errno : rc;
}
extern "C" int64_t testPread(int32_t, void*, size_t, int64_t) __asm__("sceKernelPread");
extern "C" int64_t testPread(int32_t fd, void* data, size_t bytes, int64_t offset) {
    assert(offset >= 0 && bytes <= mock::BYTES - static_cast<uint64_t>(offset));
    if (downloadSnapshot().stage == DOWNLOAD_STAGE_HASH && bytes > mock::hashReadMax) mock::hashReadMax = bytes;
    if (mock::mode == mock::PREAD_EINTR_EXHAUSTED ||
        (mock::mode == mock::INTERRUPTED_PREAD && mock::preadInterrupts.fetch_add(1) < 2))
        return static_cast<int32_t>(0x80020004U);
    ssize_t rc = pread(fd, data, bytes, static_cast<off_t>(offset));
    return rc < 0 ? -errno : rc;
}
extern "C" int32_t testNativeFstat(int32_t, void*) __asm__("sceKernelFstat");
extern "C" int32_t testNativeFstat(int32_t fd, void* native) {
    if (mock::mode == mock::FSTAT_FAILURE) return static_cast<int32_t>(0x80020005U);
    struct stat info; assert(fstat(fd, &info) == 0);
    assert(mock::isPartFd(fd));
    uint8_t* bytes = static_cast<uint8_t*>(native); memset(bytes, 0, 120);
    uint16_t mode = static_cast<uint16_t>(info.st_mode);
    int64_t size = info.st_size;
    if (mock::mode == mock::WRONG_FILE_SIZE) --size;
    if (mock::mode == mock::NOT_REGULAR_FILE) mode = S_IFIFO | 0600;
    // Independent raw native ABI: mode at8, size at72, total120 bytes.
    memcpy(bytes + 8, &mode, sizeof(mode)); memcpy(bytes + 72, &size, sizeof(size));
    return 0;
}
extern "C" int32_t sceKernelUsleep(uint32_t us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us)); return 0;
}
extern "C" int32_t scePthreadAttrInit(OrbisPthreadAttr* attr) { *attr = 0; return 0; }
extern "C" int32_t scePthreadAttrDestroy(OrbisPthreadAttr*) { return 0; }
extern "C" int32_t scePthreadAttrSetdetachstate(OrbisPthreadAttr* attr, int state) {
    assert(state == 0 || state == 1); *attr = state; return 0;
}
extern "C" int32_t scePthreadCreate(OrbisPthread* id, const OrbisPthreadAttr* attr,
    void* (*entry)(void*), void* value, const char*) {
    assert(*attr == 0 || *attr == 1);
    if (*attr == 0 && mock::mode == mock::THREAD_FAILURE) return mock::JOIN_ERROR;
    std::shared_ptr<mock::Thread> thread(new mock::Thread);
    thread->joinable = *attr == 0;
    if (!thread->joinable) {
        *id = 1; std::thread(entry, value).detach(); return 0;
    }
    {
        std::lock_guard<std::mutex> guard(mock::lock);
        *id = mock::allocate(); mock::threads[*id] = thread; ++mock::liveJoinable;
    }
    thread->host = new std::thread([entry, value, thread]() { thread->result = entry(value); });
    return 0;
}
extern "C" int32_t scePthreadJoin(OrbisPthread id, void** result) {
    std::shared_ptr<mock::Thread> thread;
    {
        std::lock_guard<std::mutex> guard(mock::lock);
        assert(mock::threads.count(id)); thread = mock::threads[id]; ++mock::joins;
        if (mock::mode == mock::JOIN_FAILURE && !mock::joinErrors) {
            ++mock::joinErrors; mock::joinErrorPublished = true; mock::wake.notify_all();
            return mock::JOIN_ERROR;
        }
    }
    thread->host->join(); if (result) *result = thread->result;
    delete thread->host; thread->host = 0;
    {
        std::lock_guard<std::mutex> guard(mock::lock);
        mock::threads.erase(id); --mock::liveJoinable;
    }
    return 0;
}
extern "C" uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal) { return 0; }
extern "C" int32_t testModuleLoaded(OrbisSysModuleInternal) __asm__("sceSysmoduleIsLoadedInternal");
extern "C" int32_t testModuleLoaded(OrbisSysModuleInternal) { return 0; }
extern "C" int32_t* testNetErrno() __asm__("sceNetErrnoLoc");
extern "C" int32_t* testNetErrno() { return &mock::netError; }
extern "C" int32_t sceNetInit() { return 0; }
extern "C" int32_t sceNetCtlInit() { return 0; }
extern "C" int32_t testNetState(int32_t*) __asm__("sceNetCtlGetState");
extern "C" int32_t testNetState(int32_t* state) { *state = 3; return 0; }
extern "C" int32_t sceNetPoolCreate(const char*, int32_t bytes, int32_t) {
    assert(bytes == 1024 * 1024); std::lock_guard<std::mutex> guard(mock::lock);
    if (++mock::poolCreates == 2 && mock::mode == mock::POOL_FAILURE) return -77;
    int id = mock::allocate(); mock::pools[id] = mock::Resource(); return id;
}
extern "C" void sceNetPoolDestroy(int32_t id) {
    std::lock_guard<std::mutex> guard(mock::lock); assert(mock::pools.erase(id) == 1);
}
extern "C" int32_t sceSslInit(size_t bytes) {
    assert(bytes == 256 * 1024); std::lock_guard<std::mutex> guard(mock::lock);
    int id = mock::allocate(); mock::ssl[id] = mock::Resource(); return id;
}
extern "C" int32_t testSslTerm(int32_t) __asm__("sceSslTerm");
extern "C" int32_t testSslTerm(int32_t id) {
    std::lock_guard<std::mutex> guard(mock::lock); assert(mock::ssl.erase(id) == 1); return 0;
}
extern "C" int32_t sceHttpInit(int32_t pool, int32_t ssl, size_t bytes) {
    assert(bytes == 1024 * 1024); std::lock_guard<std::mutex> guard(mock::lock);
    assert(mock::pools.count(pool) && mock::ssl.count(ssl));
    int id = mock::allocate(); mock::http[id] = mock::Resource(pool); return id;
}
extern "C" int32_t sceHttpCreateTemplate(int32_t http, const char*, int32_t version, int32_t proxy) {
    assert(version == ORBIS_HTTP_VERSION_1_1 && !proxy);
    std::lock_guard<std::mutex> guard(mock::lock); assert(mock::http.count(http));
    int id = mock::allocate(); mock::templates[id] = mock::Resource(http); return id;
}
extern "C" int32_t testHeaderLimit(int32_t, size_t) __asm__("sceHttpSetResponseHeaderMaxSize");
extern "C" int32_t testHeaderLimit(int32_t id, size_t bytes) {
    assert(bytes == 65536); std::lock_guard<std::mutex> guard(mock::lock);
    assert(mock::templates.count(id)); mock::templates[id].headerCap = bytes; return 0;
}
extern "C" int32_t sceHttpsEnableOption(int32_t id, uint32_t checks) {
    assert(checks == 0xbd); std::lock_guard<std::mutex> guard(mock::lock);
    assert(mock::templates.count(id)); mock::templates[id].tls = checks; return 0;
}
extern "C" int32_t testAutoRedirect(int32_t, int32_t) __asm__("sceHttpSetAutoRedirect");
extern "C" int32_t testAutoRedirect(int32_t id, int32_t enabled) {
    assert(!enabled); std::lock_guard<std::mutex> guard(mock::lock);
    assert(mock::templates.count(id)); mock::templates[id].redirectDisabled = true; return 0;
}
extern "C" int32_t testRecvTimeout(int32_t, uint32_t) __asm__("sceHttpSetRecvTimeOut");
extern "C" int32_t testRecvTimeout(int32_t, uint32_t us) { assert(us == 15000000); return 0; }
extern "C" int32_t sceHttpSetResolveTimeOut(int32_t, uint32_t us) { assert(us == 10000000); return 0; }
extern "C" int32_t sceHttpSetConnectTimeOut(int32_t, uint32_t us) { assert(us == 10000000); return 0; }
extern "C" int32_t sceHttpSetSendTimeOut(int32_t, uint32_t us) { assert(us == 15000000); return 0; }
extern "C" int32_t testSslError(int32_t, int32_t*, uint32_t*) __asm__("sceHttpsGetSslError");
extern "C" int32_t testSslError(int32_t, int32_t* code, uint32_t* detail) { *code = 0; *detail = 0; return 0; }
extern "C" int32_t sceHttpCreateConnectionWithURL(int32_t tmpl, const char* url, bool keepalive) {
    assert(!keepalive && !strcmp(url, mock::activeUrl));
    std::lock_guard<std::mutex> guard(mock::lock); assert(mock::templates.count(tmpl));
    const mock::Resource& t = mock::templates[tmpl];
    assert(t.headerCap == 65536 && t.tls == 0xbd && t.redirectDisabled);
    int id = mock::allocate(); mock::connections[id] = mock::Resource(tmpl);
    mock::connections[id].url = url; return id;
}
extern "C" int32_t sceHttpCreateRequestWithURL(int32_t conn, int32_t method, const char* url, uint64_t size) {
    assert(method == ORBIS_METHOD_GET && !size);
    std::lock_guard<std::mutex> guard(mock::lock); assert(mock::connections.count(conn));
    assert(mock::connections[conn].url == url); int id = mock::allocate();
    std::shared_ptr<mock::Request> r(new mock::Request(conn, id, url));
    mock::requests[id] = r; mock::history.push_back(r); return id;
}
extern "C" int32_t sceHttpAddRequestHeader(int32_t id, const char* key, const char* value, int32_t) {
    std::lock_guard<std::mutex> guard(mock::lock); std::shared_ptr<mock::Request> r = mock::request(id);
    if (!strcmp(key, "Range")) r->range = value;
    else if (!strcmp(key, "If-Range")) r->ifRange = value;
    else { assert(!strcmp(key, "Accept-Encoding") && !strcmp(value, "identity")); }
    return 0;
}
extern "C" int32_t sceHttpSendRequest(int32_t id, const void* data, size_t size) {
    assert(!data && !size); std::lock_guard<std::mutex> guard(mock::lock);
    std::shared_ptr<mock::Request> r = mock::request(id); r->sent = true;
    if (!r->range.empty()) {
        unsigned long long first = 0, last = 0; char tail = 0;
        assert(sscanf(r->range.c_str(), "bytes=%llu-%llu%c", &first, &last, &tail) == 2);
        assert(r->ifRange == "\"fixture-v1\"" && first <= last && last < mock::BYTES);
        r->ranged = true; r->first = first; r->last = last; r->length = last - first + 1;
        r->status = 206; ++mock::rangedSent;
        if (mock::mode == mock::RANGE_REDIRECT) r->status = 302;
        if (first && mock::mode == mock::IGNORE_RANGE) r->status = 200;
        if (first && mock::mode == mock::RANGE_503) r->status = 503;
        if (first && mock::mode == mock::WRONG_LENGTH) ++r->length;
    }
    uint64_t rangeFirst = r->first, total = mock::BYTES;
    if (r->ranged && r->first && mock::mode == mock::WRONG_RANGE) ++rangeFirst;
    if (r->ranged && r->first && mock::mode == mock::WRONG_RANGE_TOTAL) ++total;
    char header[512];
    snprintf(header, sizeof(header), "HTTP/1.1 %d OK\r\nContent-Length: %llu\r\nAccept-Ranges: bytes\r\n",
        r->status, static_cast<unsigned long long>(r->length)); r->headers = header;
    if (r->ranged) {
        snprintf(header, sizeof(header), "Content-Range: bytes %llu-%llu/%llu\r\n",
            static_cast<unsigned long long>(rangeFirst), static_cast<unsigned long long>(r->last),
            static_cast<unsigned long long>(total)); r->headers += header;
    }
    if (mock::mode != mock::NO_ETAG) {
        if (mock::mode == mock::WEAK_ETAG) r->headers += "ETag: W/\"fixture-v1\"\r\n";
        else if (r->ranged && r->first && mock::mode == mock::WRONG_ETAG) r->headers += "ETag: \"fixture-v2\"\r\n";
        else r->headers += "ETag: \"fixture-v1\"\r\n";
    }
    if (r->ranged && r->first && mock::mode == mock::DUPLICATE_ETAG)
        r->headers += "ETag: \"fixture-v2\"\r\n";
    r->headers += "\r\n"; return 0;
}
extern "C" int32_t sceHttpGetStatusCode(int32_t id, int32_t* status) {
    std::lock_guard<std::mutex> guard(mock::lock); *status = mock::request(id)->status; return 0;
}
extern "C" int32_t sceHttpGetLastErrno(int32_t, int32_t* code) { *code = 0; return 0; }
extern "C" int32_t sceHttpGetAllResponseHeaders(int32_t id, char** headers, size_t* bytes) {
    std::lock_guard<std::mutex> guard(mock::lock); std::shared_ptr<mock::Request> r = mock::request(id);
    assert(r->sent); *headers = &r->headers[0]; *bytes = r->headers.size(); return 0;
}
extern "C" int32_t sceHttpGetResponseContentLength(int32_t id, int32_t* kind, size_t* bytes) {
    std::lock_guard<std::mutex> guard(mock::lock); std::shared_ptr<mock::Request> r = mock::request(id);
    *kind = ORBIS_HTTP_CONTENTLEN_EXIST; *bytes = r->length; return 0;
}
extern "C" int32_t sceHttpReadData(int32_t id, void* out, uint32_t bytes) {
    std::unique_lock<std::mutex> guard(mock::lock); std::shared_ptr<mock::Request> r = mock::request(id);
    assert(r->sent && bytes && bytes <= mock::LARGE_BUFFER_BYTES);
    if (r->aborted) return mock::READ_ERROR;
    if (!r->ranged) {
        ++mock::singleReadCalls;
        if (r->position == r->length) return 0;
        size_t n = static_cast<size_t>(r->length - r->position);
        if (n > bytes) n = bytes;
        memcpy(out, mock::payload.data() + r->position, n); r->position += n; return n;
    }
    ++mock::rangeReadCalls; ++mock::activeRangeReads;
    if (mock::activeRangeReads > mock::peakRangeReads) mock::peakRangeReads = mock::activeRangeReads;
    if (r->firstRead) { r->firstRead = false; ++mock::enteredRangeRequests; mock::wake.notify_all(); }
    const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (mock::enteredRangeRequests < 2 && !r->aborted) {
        if (mock::wake.wait_until(guard, until) == std::cv_status::timeout)
            assert(!"two native range readers never overlapped");
    }
    bool blocking = (mock::shouldBlock() && r->position >= mock::LARGE_BUFFER_BYTES) ||
        (mock::mode == mock::JOIN_FAILURE && r->first && r->position >= mock::LARGE_BUFFER_BYTES && !mock::joinErrorPublished);
    if (blocking && !r->blockReported) { r->blockReported = true; ++mock::blockedRangeRequests; mock::wake.notify_all(); }
    while (!r->aborted && ((mock::shouldBlock() && r->position >= mock::LARGE_BUFFER_BYTES) ||
        (mock::mode == mock::JOIN_FAILURE && r->first && r->position >= mock::LARGE_BUFFER_BYTES && !mock::joinErrorPublished)))
        mock::wake.wait(guard);
    if (r->aborted) { --mock::activeRangeReads; return mock::READ_ERROR; }
    if (mock::mode == mock::BODY_FAILURE && !r->first && r->position >= 1024 * 1024) {
        --mock::activeRangeReads; return mock::READ_ERROR;
    }
    if (mock::mode == mock::EARLY_EOF && !r->first && r->position >= 1024 * 1024) {
        --mock::activeRangeReads; return 0;
    }
    if (mock::mode == mock::READ_OVERSIZED && !r->first && r->position >= 1024 * 1024) {
        --mock::activeRangeReads; return static_cast<int32_t>(bytes + 1);
    }
    if (r->position == r->last - r->first + 1) { --mock::activeRangeReads; return 0; }
    size_t n = static_cast<size_t>(r->last - r->first + 1 - r->position);
    if (n > bytes) n = bytes;
    if (mock::mode == mock::FRAGMENTED_BODY && n > 16 * 1024) n = 16 * 1024;
    uint64_t at = r->first + r->position;
    guard.unlock(); std::this_thread::sleep_for(std::chrono::microseconds(100));
    memcpy(out, mock::payload.data() + at, n);
    if (mock::mode == mock::RANGE_HEADER_CORRUPT && at <= 0x40 && at + n > 0x40)
        static_cast<uint8_t*>(out)[0x40 - at] = 'E';
    guard.lock();
    r->position += n; --mock::activeRangeReads; return n;
}
extern "C" int32_t sceHttpAbortRequest(int32_t id) {
    std::lock_guard<std::mutex> guard(mock::lock); std::shared_ptr<mock::Request> r = mock::request(id);
    r->aborted = true; mock::wake.notify_all(); return 0;
}
extern "C" int32_t sceHttpDeleteRequest(int32_t id) {
    std::lock_guard<std::mutex> guard(mock::lock); std::shared_ptr<mock::Request> r = mock::request(id);
    assert(!mock::activeRangeReads && !mock::liveJoinable); r->deleted = true;
    assert(mock::requests.erase(id) == 1); return 0;
}
extern "C" int32_t sceHttpDeleteConnection(int32_t id) {
    std::lock_guard<std::mutex> guard(mock::lock); assert(!mock::liveJoinable);
    assert(mock::connections.erase(id) == 1); return 0;
}
extern "C" int32_t sceHttpDeleteTemplate(int32_t id) {
    std::lock_guard<std::mutex> guard(mock::lock); assert(mock::templates.erase(id) == 1); return 0;
}
extern "C" int32_t sceHttpTerm(int32_t id) {
    std::lock_guard<std::mutex> guard(mock::lock); assert(mock::http.erase(id) == 1); return 0;
}

static DownloadSpec spec(const char* hash = 0) {
    DownloadSpec value = {mock::activeUrl, "parallel.pkg", mock::BYTES, hash}; return value;
}
static void successTests() {
    for (mock::Mode mode : {mock::GOOD, mock::SHORT_WRITES, mock::FRAGMENTED_BODY,
        mock::INTERRUPTED_WRITES, mock::INTERRUPTED_PREAD}) {
        mock::reset(mode); assert(startDownload(spec(), mock::CID));
        DownloadSnapshot s = mock::finish();
        assert(s.state == DONE && !s.errorCode && s.received == mock::BYTES);
        assert(mock::rangedSent == 2 && mock::peakRangeReads >= 2);
        assert(mock::pwriteBytes == mock::BYTES && mock::joins == 1 && mock::fsyncCalls == 1);
        assert(mock::singleReadCalls == 1); mock::clean(); mock::exactOutput();
        if (mode == mock::FRAGMENTED_BODY) assert(mock::pwriteCalls == 32 && mock::rangeReadCalls == 2048);
        assert(mock::history.size() == 3);
        assert(mock::history[1]->first == 0 && mock::history[1]->last == mock::BYTES / 2 - 1);
        assert(mock::history[2]->first == mock::BYTES / 2 && mock::history[2]->last == mock::BYTES - 1);
    }
    mock::reset(mock::GOOD); std::string hash = mock::digest();
    assert(startDownload(spec(hash.c_str()), mock::CID));
    assert(mock::finish().state == DONE); mock::clean(); mock::exactOutput();
    mock::reset(mock::GOOD); mock::BYTES += 123; mock::makePayload();
    assert(startDownload(spec(), mock::CID));
    assert(mock::finish().state == DONE && mock::pwriteBytes == mock::BYTES);
    mock::clean(); mock::exactOutput();
}
static void fallbackTests() {
    for (mock::Mode mode : {mock::NO_ETAG, mock::WEAK_ETAG, mock::IGNORE_RANGE,
        mock::RANGE_503, mock::RANGE_REDIRECT, mock::WRONG_ETAG, mock::WRONG_RANGE, mock::WRONG_RANGE_TOTAL,
        mock::WRONG_LENGTH, mock::DUPLICATE_ETAG, mock::POOL_FAILURE,
        mock::ALLOCATION_FAILURE, mock::THREAD_FAILURE}) {
        mock::reset(mode); assert(startDownload(spec(), mock::CID));
        DownloadSnapshot s = mock::finish();
        assert(s.state == DONE && !s.errorCode && s.received == mock::BYTES);
        assert(!mock::pwriteCalls && !mock::rangeReadCalls);
        assert(s.connections == 1);
        assert(s.transferMode == (mode == mock::THREAD_FAILURE ? DOWNLOAD_MODE_SINGLE : DOWNLOAD_MODE_PIPELINED));
        assert(mock::joins == (mode == mock::THREAD_FAILURE ? 0 : 1));
        if (mode == mock::NO_ETAG || mode == mock::WEAK_ETAG) assert(!mock::rangedSent);
        else assert(mock::rangedSent > 0 || mode == mock::POOL_FAILURE);
        mock::clean(); mock::exactOutput();
    }
}
static void bufferFallbackTests() {
    for (mock::Mode mode : {mock::PREFERRED_FIRST_FAILURE, mock::PREFERRED_SECOND_FAILURE}) {
        mock::reset(mode); assert(startDownload(spec(), mock::CID));
        DownloadSnapshot s = mock::finish();
        assert(s.state == DONE && s.received == mock::BYTES && !s.errorCode);
        assert(mock::allocatorFailures == 1 && mock::rangedSent == 2 && mock::joins == 1);
        assert(mock::pwriteCalls == 128 && mock::rangeReadCalls == 128);
        mock::clean(); mock::exactOutput();
    }
    mock::reset(mock::HASH_PREFERRED_FAILURE); std::string hash = mock::digest();
    assert(startDownload(spec(hash.c_str()), mock::CID));
    assert(mock::finish().state == DONE && mock::hashReadMax == mock::SMALL_BUFFER_BYTES);
    assert(mock::allocatorFailures == 1 && mock::pwriteCalls == 32);
    mock::clean(); mock::exactOutput();
}
static void approvedProviderTests() {
    const char* urls[] = {
        "https://release-assets.githubusercontent.com/fixture/parallel.pkg?token=abc",
        "https://download2392.mediafire.com/token/parallel.pkg?key=a&part=b"
    };
    for (const char* url : urls) {
        for (mock::Mode mode : {mock::GOOD, mock::NO_ETAG, mock::WEAK_ETAG, mock::RANGE_REDIRECT}) {
            mock::reset(mode); mock::activeUrl = url;
            assert(startDownload(spec(), mock::CID)); DownloadSnapshot s = mock::finish();
            assert(s.state == DONE && s.received == mock::BYTES && !s.errorCode);
            if (mode == mock::GOOD) {
                assert(mock::rangedSent == 2 && mock::peakRangeReads >= 2 && mock::pwriteCalls == 32);
            } else {
                assert(!mock::pwriteCalls && !mock::rangeReadCalls && mock::joins == 1);
                assert(s.connections == 1 && s.transferMode == DOWNLOAD_MODE_PIPELINED);
                if (mode != mock::RANGE_REDIRECT) assert(!mock::rangedSent);
                else assert(mock::history.size() == 3); // Initial body, rejected range, one single restart.
            }
            for (size_t i = 0; i < mock::history.size(); ++i) assert(mock::history[i]->url == url);
            mock::clean(); mock::exactOutput();
        }
    }
}
static void packageKindRangeTests() {
    struct Fixture { uint32_t type, flags; int kind; };
    const Fixture fixtures[] = {
        {0x1A, 0x62300000U, USER_PACKAGE_UPDATE},
        {0x1C, 0x0A000000U, USER_PACKAGE_DLC}
    };
    for (const Fixture& fixture : fixtures) {
        mock::reset(mock::GOOD);
        mock::put32(0x74, fixture.type); mock::put32(0x78, fixture.flags);
        std::string hash = mock::digest();
        assert(startDownload(spec(hash.c_str()), mock::CID, fixture.kind));
        assert(mock::finish().state == DONE && mock::rangedSent == 2);
        mock::clean(); mock::exactOutput();

        mock::reset(mock::GOOD); mock::seedPrevious();
        mock::put32(0x74, fixture.type); mock::put32(0x78, fixture.flags);
        assert(startDownload(spec(), mock::CID)); // Legacy default is still base.
        DownloadSnapshot s = mock::finish();
        assert(s.state == FAILED && s.errorCode == DOWNLOAD_ERROR_PACKAGE && !mock::rangedSent);
        mock::clean(); mock::checkPrevious();
    }
}
static void failureTests() {
    for (mock::Mode mode : {mock::BODY_FAILURE, mock::WRITE_FAILURE, mock::FSYNC_FAILURE,
        mock::CLOSE_FAILURE, mock::JOIN_FAILURE, mock::HASH_MISMATCH, mock::HEADER_MISMATCH,
        mock::PWRITE_EINTR_EXHAUSTED, mock::PREAD_EINTR_EXHAUSTED, mock::ZERO_WRITE,
        mock::OVERSIZED_WRITE, mock::EARLY_EOF, mock::READ_OVERSIZED, mock::RANGE_HEADER_CORRUPT,
        mock::FSTAT_FAILURE, mock::WRONG_FILE_SIZE, mock::NOT_REGULAR_FILE, mock::HASH_ALLOCATION_FAILURE}) {
        mock::reset(mode); mock::seedPrevious();
        if (mode == mock::BODY_FAILURE || mode == mock::WRITE_FAILURE) mock::secondaryCloseError = true;
        std::string goodHash = mock::digest();
        const char* hash = mode == mock::HASH_MISMATCH ?
            "0000000000000000000000000000000000000000000000000000000000000000" :
            mode == mock::HASH_ALLOCATION_FAILURE ? goodHash.c_str() : 0;
        assert(startDownload(spec(hash), mock::CID)); DownloadSnapshot s = mock::finish();
        assert(s.state == FAILED && s.errorCode && s.received <= mock::BYTES);
        mock::clean(); mock::checkPrevious();
        if (mode == mock::HEADER_MISMATCH) {
            assert(s.errorCode == DOWNLOAD_ERROR_PACKAGE && !mock::rangedSent && !mock::pwriteCalls);
        } else {
            assert(mock::history.size() == 3 && mock::rangedSent == 2);
            if (mode == mock::BODY_FAILURE) {
                assert(s.errorCode == DOWNLOAD_ERROR_NETWORK && s.stage == DOWNLOAD_STAGE_READ);
                assert(s.nativeCode == mock::READ_ERROR && mock::rangeAborts() == 2);
            } else if (mode == mock::WRITE_FAILURE) {
                assert(s.errorCode == DOWNLOAD_ERROR_FILESYSTEM && s.stage == DOWNLOAD_STAGE_FILE_WRITE);
                assert(s.nativeCode == mock::WRITE_ERROR && mock::rangeAborts() == 2);
            } else if (mode == mock::FSYNC_FAILURE) {
                assert(s.errorCode == DOWNLOAD_ERROR_FILESYSTEM && s.stage == DOWNLOAD_STAGE_FILE_FLUSH);
                assert(s.nativeCode == ENOSPC);
            } else if (mode == mock::CLOSE_FAILURE) {
                assert(s.errorCode == DOWNLOAD_ERROR_FILESYSTEM && s.stage == DOWNLOAD_STAGE_FILE_CLOSE);
                assert(s.nativeCode == EIO);
            } else if (mode == mock::JOIN_FAILURE) {
                assert(s.errorCode == DOWNLOAD_ERROR_THREAD && s.stage == DOWNLOAD_STAGE_THREAD);
                assert(s.nativeCode == mock::JOIN_ERROR && mock::joins >= 2 && mock::rangeAborts() == 2);
            } else if (mode == mock::HASH_MISMATCH) assert(s.errorCode == DOWNLOAD_ERROR_HASH);
            else if (mode == mock::PWRITE_EINTR_EXHAUSTED || mode == mock::ZERO_WRITE || mode == mock::OVERSIZED_WRITE)
                assert(s.errorCode == DOWNLOAD_ERROR_FILESYSTEM && s.stage == DOWNLOAD_STAGE_FILE_WRITE);
            else if (mode == mock::PREAD_EINTR_EXHAUSTED)
                assert(s.errorCode == DOWNLOAD_ERROR_FILESYSTEM && s.stage == DOWNLOAD_STAGE_PACKAGE);
            else if (mode == mock::EARLY_EOF || mode == mock::READ_OVERSIZED)
                assert(s.errorCode == DOWNLOAD_ERROR_LENGTH && s.stage == DOWNLOAD_STAGE_READ);
            else if (mode == mock::RANGE_HEADER_CORRUPT)
                assert(s.errorCode == DOWNLOAD_ERROR_PACKAGE && s.stage == DOWNLOAD_STAGE_PACKAGE);
            else if (mode == mock::HASH_ALLOCATION_FAILURE)
                assert(s.errorCode == DOWNLOAD_ERROR_FILESYSTEM && s.stage == DOWNLOAD_STAGE_HASH && s.nativeCode == ENOMEM && mock::allocatorFailures == 2);
            else if (mode == mock::FSTAT_FAILURE)
                assert(s.errorCode == DOWNLOAD_ERROR_FILESYSTEM && s.stage == DOWNLOAD_STAGE_PACKAGE &&
                    s.nativeCode == static_cast<int32_t>(0x80020005U));
            else if (mode == mock::WRONG_FILE_SIZE || mode == mock::NOT_REGULAR_FILE)
                assert(s.errorCode == DOWNLOAD_ERROR_LENGTH && s.stage == DOWNLOAD_STAGE_PACKAGE);
        }
    }
}
static void cancellationTest() {
    mock::reset(mock::CANCEL_BODY); mock::seedPrevious();
    assert(startDownload(spec(), mock::CID));
    {
        std::unique_lock<std::mutex> guard(mock::lock);
        assert(mock::wake.wait_for(guard, std::chrono::seconds(5), []() { return mock::blockedRangeRequests == 2; }));
        assert(mock::activeRangeReads == 2);
    }
    assert(downloadSnapshot().received == 2 * mock::LARGE_BUFFER_BYTES);
    cancelDownload(); DownloadSnapshot s = mock::finish();
    assert(s.state == CANCELLED && s.received <= mock::BYTES && mock::rangeAborts() == 2);
    assert(mock::joins == 1); mock::clean(); mock::checkPrevious();
}
struct SparseLane {
    const char* bytes;
    size_t used;
    SparseLane(const char* value) : bytes(value), used(0) {}
};
struct SparseProgress {
    std::atomic<uint64_t> received;
    std::atomic<bool> aborted;
    SparseProgress() : received(0), aborted(false) {}
};
static int32_t sparseRead(void* opaque, void* output, size_t requested) {
    SparseLane& lane = *static_cast<SparseLane*>(opaque);
    size_t bytes = 8 - lane.used;
    if (bytes > requested) bytes = requested;
    memcpy(output, lane.bytes + lane.used, bytes); lane.used += bytes;
    return static_cast<int32_t>(bytes);
}
static void sparseFailure(void*, int32_t code, peppyParallelDownload::Failure* failure) {
    failure->category = DOWNLOAD_ERROR_NETWORK;
    failure->where = DOWNLOAD_STAGE_READ; failure->native = code;
}
static bool sparseCancelled(void*) { return false; }
static void sparseAbort(void* opaque) { static_cast<SparseProgress*>(opaque)->aborted = true; }
static void sparseProgress(void* opaque, uint64_t bytes) {
    static_cast<SparseProgress*>(opaque)->received.fetch_add(bytes);
}
static void sparseOffsetTest() {
    mock::reset(mock::GOOD);
    const int64_t remoteOffset = 5LL * 1024 * 1024 * 1024 + 123;
    mock::BYTES = static_cast<uint64_t>(remoteOffset) + 8;
    int fd = open(mock::path(true).c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600); assert(fd >= 0);
    SparseLane first("offset00"), second("offset05"); SparseProgress progress;
    peppyParallelDownload::Plan plan;
    plan.lanes[0].context = &first; plan.lanes[0].read = sparseRead;
    plan.lanes[0].captureReadError = sparseFailure; plan.lanes[0].first = 0; plan.lanes[0].length = 8;
    plan.lanes[1].context = &second; plan.lanes[1].read = sparseRead;
    plan.lanes[1].captureReadError = sparseFailure;
    plan.lanes[1].first = static_cast<uint64_t>(remoteOffset); plan.lanes[1].length = 8;
    plan.fd = fd; plan.context = &progress; plan.cancelled = sparseCancelled;
    plan.abort = sparseAbort; plan.progress = sparseProgress;
    peppyParallelDownload::Outcome outcome = peppyParallelDownload::run(plan);
    assert(outcome.started && !outcome.failure.category && !progress.aborted && progress.received == 16);
    assert(mock::pwriteBytes == 16 && mock::pwriteCalls == 2 && mock::joins == 1);
    char bytes[8]; assert(testPread(fd, bytes, 8, 0) == 8 && !memcmp(bytes, "offset00", 8));
    assert(testPread(fd, bytes, 8, remoteOffset) == 8 && !memcmp(bytes, "offset05", 8));
    struct stat info; assert(fstat(fd, &info) == 0);
    assert(info.st_size == remoteOffset + 8 && info.st_blocks <= 64);
    assert(close(fd) == 0); assert(unlink(mock::path(true).c_str()) == 0); mock::clean();
}
int main() {
    successTests(); fallbackTests(); bufferFallbackTests(); approvedProviderTests(); packageKindRangeTests();
    failureTests(); cancellationTest(); sparseOffsetTest();
    unlink(mock::path().c_str());
    unlink((std::string(parallelTestDirectory()) + "/download.log").c_str());
    assert(rmdir(parallelTestDirectory()) == 0);
    puts("parallel download tests passed: real concurrent readers, exact output, fallback and failure cleanup");
    return 0;
}
