#include <assert.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "../../pkg_server.cpp"

namespace {
struct Client {
    int id;
    std::string incoming, outgoing;
    size_t consumed = 0;
    bool closed = false, configured = false;
    bool stalled = false;
    int recvRetry = 0, sendRetry = 0;
    size_t recvLimit = 0, sendLimit = 0, failSendAfter = 0;
    unsigned recvCalls = 0;
};
std::mutex lock;
std::map<int, std::shared_ptr<Client>> clients;
std::deque<int> pending;
std::map<int, int> closeCounts;
int nextId = 100;
std::atomic<int32_t> socketResult(9), optionResult(0), bindResult(0), listenResult(0), nameResult(0);
std::atomic<int32_t> poolResult(4), threadResult(0), acceptFailure(0), joinFailure(0);
std::atomic<int> poolsCreated(0), poolsDestroyed(0), joins(0), acceptRetry(0);
std::atomic<uint64_t> clockOffset(0);
bool listenerConfigured = false;
uint32_t bindAddress = 0;
uint16_t bindPort = 1;
thread_local int32_t mockErrno = 0;

template<class Predicate> void await(Predicate ready) {
    for (int n = 0; n < 3000; ++n) {
        if (ready()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!"Timed out waiting for mock server");
}
std::shared_ptr<Client> enqueue(const std::string& request, size_t recvLimit = 0, size_t sendLimit = 0,
                                int recvRetry = 0, int sendRetry = 0, bool stalled = false,
                                size_t failSendAfter = 0) {
    std::shared_ptr<Client> client(new Client);
    std::lock_guard<std::mutex> guard(lock);
    client->id = nextId++;
    client->incoming = request;
    client->recvLimit = recvLimit;
    client->sendLimit = sendLimit;
    client->recvRetry = recvRetry;
    client->sendRetry = sendRetry;
    client->stalled = stalled;
    client->failSendAfter = failSendAfter;
    clients[client->id] = client;
    pending.push_back(client->id);
    return client;
}
bool closed(const std::shared_ptr<Client>& client) {
    std::lock_guard<std::mutex> guard(lock);
    return client->closed;
}
std::string exchange(PkgServer& server, const std::string& method = "GET", const std::string& headers = "",
                     const char* alternatePath = 0, size_t recvLimit = 0, size_t sendLimit = 0,
                     int recvRetry = 0, int sendRetry = 0) {
    const char* path = alternatePath ? alternatePath : strchr(server.url() + 7, '/');
    assert(path);
    auto client = enqueue(method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" + headers + "\r\n",
                          recvLimit, sendLimit, recvRetry, sendRetry);
    await([client] { return closed(client); });
    return client->outgoing;
}
std::string body(const std::string& response) {
    size_t offset = response.find("\r\n\r\n");
    assert(offset != std::string::npos);
    return response.substr(offset + 4);
}
void status(const std::string& response, int expected) {
    assert(response.find("HTTP/1.1 " + std::to_string(expected) + " ") == 0);
}
void reset() {
    std::lock_guard<std::mutex> guard(lock);
    clients.clear(); pending.clear(); closeCounts.clear(); nextId = 100;
    socketResult = 9; optionResult = 0; bindResult = 0; listenResult = 0; nameResult = 0;
    poolResult = 4; threadResult = 0; acceptFailure = 0; joinFailure = 0;
    poolsCreated = 0; poolsDestroyed = 0; joins = 0; acceptRetry = 0;
    clockOffset = 0;
    listenerConfigured = false; bindAddress = 0; bindPort = 1;
}
Request parsed(const std::string& request, uint64_t bytes = 10) {
    assert(request.size() <= HEADER_CAP);
    char header[HEADER_CAP + 1] = {};
    memcpy(header, request.data(), request.size());
    return parse(header, "/token.pkg", bytes, 43210);
}
Request requestedRange(const std::string& value, uint64_t bytes = 10) {
    return parsed("GET /token.pkg HTTP/1.1\r\nRange: " + value + "\r\n\r\n", bytes);
}
} // namespace

extern "C" int32_t sceNetInit() { return int32_t(0x80410111u); } // Reused global network is usable.
extern "C" int32_t sceNetPoolCreate(const char*, int32_t bytes, int32_t flags) {
    assert(bytes == 1024 * 1024 && flags == 0); ++poolsCreated; return poolResult;
}
extern "C" void sceNetPoolDestroy(int32_t id) { assert(id == 4); ++poolsDestroyed; }
extern "C" OrbisNetId sceNetSocket(const char*, int32_t domain, int32_t type, int protocol) {
    assert(domain == 2 && type == 1 && protocol == 0); return socketResult;
}
extern "C" int32_t sceNetSetsockopt(OrbisNetId id, int32_t level, int32_t option, const void* value, OrbisNetSocklen_t bytes) {
    assert(level == 0xffff && option == 0x1200 && bytes == 4 && *static_cast<const int*>(value) == 1);
    if (optionResult) return optionResult;
    std::lock_guard<std::mutex> guard(lock);
    if (id == 9) listenerConfigured = true;
    else { assert(clients.count(id)); clients[id]->configured = true; }
    return 0;
}
extern "C" int32_t sceNetBind(OrbisNetId id, const OrbisNetSockaddr* raw, OrbisNetSocklen_t bytes) {
    assert(id == 9 && listenerConfigured && bytes == 16);
    const SocketAddress* address = reinterpret_cast<const SocketAddress*>(raw);
    assert(address->length == 16 && address->family == 2);
    bindAddress = address->address; bindPort = address->port;
    assert(bindAddress == sceNetHtonl(0x7f000001) && bindPort == 0);
    return bindResult;
}
extern "C" int32_t sceNetListen(OrbisNetId id, int count) { assert(id == 9 && count == 4); return listenResult; }
extern "C" int32_t sceNetGetsockname(OrbisNetId id, OrbisNetSockaddr* raw, OrbisNetSocklen_t* bytes) {
    assert(id == 9 && *bytes == 16);
    SocketAddress* address = reinterpret_cast<SocketAddress*>(raw);
    address->length = 16; address->family = 2;
    address->address = sceNetHtonl(0x7f000001); address->port = sceNetNtohs(43210);
    return nameResult;
}
extern "C" OrbisNetId sceNetAccept(OrbisNetId id, OrbisNetSockaddr* raw, OrbisNetSocklen_t* bytes) {
    assert(id == 9 && listenerConfigured);
    if (acceptFailure) return acceptFailure;
    if (acceptRetry > 0) { --acceptRetry; mockErrno = NET_EINTR; return -1; }
    std::lock_guard<std::mutex> guard(lock);
    if (pending.empty()) { mockErrno = NET_EWOULDBLOCK; return -1; }
    int client = pending.front(); pending.pop_front();
    SocketAddress* peer = reinterpret_cast<SocketAddress*>(raw);
    peer->length = 16; peer->family = 2; peer->address = sceNetHtonl(0x7f000001); *bytes = 16;
    return client;
}
extern "C" int32_t sceNetRecv(OrbisNetId id, void* output, size_t capacity, int flags) {
    assert(flags == 0);
    std::lock_guard<std::mutex> guard(lock);
    auto client = clients[id]; assert(client && client->configured && !client->closed);
    ++client->recvCalls;
    if (client->recvRetry > 0) {
        mockErrno = client->recvRetry-- & 1 ? NET_EINTR : NET_EWOULDBLOCK;
        return -1;
    }
    if (client->stalled) { mockErrno = NET_EWOULDBLOCK; return -1; }
    size_t available = client->incoming.size() - client->consumed;
    size_t amount = available < capacity ? available : capacity;
    if (client->recvLimit && amount > client->recvLimit) amount = client->recvLimit;
    memcpy(output, client->incoming.data() + client->consumed, amount);
    client->consumed += amount;
    return int32_t(amount);
}
extern "C" int32_t sceNetSend(OrbisNetId id, const void* data, size_t bytes, int flags) {
    assert(flags == 0);
    std::lock_guard<std::mutex> guard(lock);
    auto client = clients[id]; assert(client && client->configured && !client->closed);
    if (client->sendRetry > 0) {
        mockErrno = client->sendRetry-- & 1 ? NET_EINTR : NET_EWOULDBLOCK;
        return -1;
    }
    if (client->failSendAfter) {
        if (client->outgoing.size() >= client->failSendAfter) { mockErrno = 54; return -1; }
        size_t untilFailure = client->failSendAfter - client->outgoing.size();
        if (bytes > untilFailure) bytes = untilFailure;
    }
    if (client->sendLimit && bytes > client->sendLimit) bytes = client->sendLimit;
    client->outgoing.append(static_cast<const char*>(data), bytes);
    return int32_t(bytes);
}
extern "C" int32_t sceNetSocketClose(OrbisNetId id) {
    std::lock_guard<std::mutex> guard(lock);
    assert(++closeCounts[id] == 1); // Catch every repeated close/reused descriptor hazard.
    if (id != 9) { assert(clients.count(id)); clients[id]->closed = true; }
    return 0;
}
extern "C" int32_t* sceNetErrnoLoc() { return &mockErrno; }
extern "C" uint32_t sceNetHtonl(uint32_t value) { return __builtin_bswap32(value); }
extern "C" uint16_t sceNetNtohs(uint16_t value) { return __builtin_bswap16(value); }
extern "C" int32_t sceSysmoduleIsLoadedInternal(OrbisSysModuleInternal) { return 0; }
extern "C" uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal) { assert(false); return 0; }
extern "C" uint64_t sceKernelGetProcessTime() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() + clockOffset.load();
}
extern "C" int32_t sceKernelUsleep(uint32_t microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds)); return 0;
}
extern "C" int32_t scePthreadCreate(OrbisPthread* thread, const OrbisPthreadAttr* attr,
    void* (*entry)(void*), void* argument, const char*) {
    return threadResult ? threadResult.load() : pthread_create(thread, attr, entry, argument);
}
extern "C" int32_t scePthreadJoin(OrbisPthread thread, void** result) {
    ++joins;
    int32_t injected = joinFailure.exchange(0);
    return injected ? injected : pthread_join(thread, result);
}

int main() {
    // Parser boundaries and uint64 range normalization, including >4 GiB.
    Request r = requestedRange("bytes=0-0"); assert(r.status == 206 && r.first == 0 && r.last == 0);
    r = requestedRange("bytes=9-"); assert(r.first == 9 && r.last == 9);
    r = requestedRange("bytes=-1"); assert(r.first == 9 && r.last == 9);
    r = requestedRange("bytes=-11"); assert(r.first == 0 && r.last == 9);
    r = requestedRange("bytes=0-18446744073709551615"); assert(r.last == 9);
    r = requestedRange("bytes=4294967296-4294967297", 5000000000ULL);
    assert(r.status == 206 && r.first == 4294967296ULL && r.last == 4294967297ULL);
    const char* invalid[] = {"bytes=10-", "bytes=4-3", "bytes=-0", "bytes=0-1,4-5", "bytes=1--3",
        "bytes=+1-3", "bytes=1- 3", "bytes=18446744073709551616-", "bytes=0-18446744073709551616", "bytes=-"};
    for (const char* value : invalid) assert(requestedRange(value).status == 416);
    assert(parsed("GET /token.pkg HTTP/1.1\r\nRange: bytes=0-1\r\nrAnGe: bytes=2-3\r\n\r\n").status == 400);
    assert(parsed("GET /token.pkg HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n").status == 400);
    assert(parsed("GET /token.pkg HTTP/1.1\r\nContent-Length: 1\r\n\r\n").status == 400);
    assert(parsed("GET /token.pkg HTTP/1.1\r\nContent-Length: 0\r\n\r\n").status == 200);
    assert(parsed("GET /token.pkg HTTP/1.1\r\n folded: yes\r\n\r\n").status == 400);
    assert(parsed("GET /token.pkg HTTP/1.1\n\n").status == 400);
    const char nulBytes[] = "GET /token.pkg HTTP/1.1\r\nX: a\0b\r\n\r\n";
    std::string nul(nulBytes, sizeof(nulBytes) - 1);
    assert(parsed(nul).status == 400);
    // A BGFT query is opaque and optional; the selected token path remains exact.
    assert(parsed("GET /token.pkg?download=1&x=%2f%ff HTTP/1.1\r\n\r\n").status == 200);
    assert(parsed("HEAD /token.pkg? HTTP/1.1\r\n\r\n").status == 200);
    assert(parsed("GET /token.pkg?unusual=%Z?more HTTP/1.1\r\n\r\n").status == 200);
    assert(parsed("HEAD /token.pkg?download=1 HTTP/1.1\r\nHost: localhost\r\n\r\n").status == 200);
    assert(parsed("GET http://127.0.0.1:43210/token.pkg?download=1 HTTP/1.1\r\n\r\n").status == 200);
    const char* wrongTargets[] = { "/token.pkgx?x=1", "/elsewhere?file=/token.pkg", "/../token.pkg?x=1",
        "/%74oken.pkg?x=1", "/token.pkg#part", "/token.pkg?x=1#part",
        "http://127.0.0.1:43211/token.pkg?x=1", "http://127.0.0.1/token.pkg?x=1",
        "http://example.org:43210/token.pkg?x=1", "http://localhost:43210/token.pkg?x=1",
        "http://127.0.0.1:43210@evil.test/token.pkg?x=1", "http://127.0.0.1:43210x/token.pkg?x=1",
        "http://127.0.0.1:043210/token.pkg?x=1", "https://127.0.0.1:43210/token.pkg?x=1" };
    for (const char* target : wrongTargets)
        assert(parsed(std::string("GET ") + target + " HTTP/1.1\r\n\r\n").status == 404);
    assert(parsed("GET /token.pkg?x=\tbad HTTP/1.1\r\n\r\n").status == 404);

    FILE* file = tmpfile(); assert(file);
    std::string package;
    for (int i = 0; i < 150000; ++i) package.push_back(char(i % 251));
    assert(fwrite(package.data(), 1, package.size(), file) == package.size() && fflush(file) == 0);
    reset();
    {
        PkgServer server;
        assert(server.start(file, package.size()) == 0);
        assert(std::string(server.url()).find("http://127.0.0.1:43210/peppy-") == 0);
        assert(server.start(file, package.size()) == ERROR_BUSY);
        acceptRetry = 2;
        std::string reply = exchange(server, "GET", "", 0, 3, 97, 2, 3);
        status(reply, 200); assert(body(reply) == package);
        assert(reply.find("Content-Length: 150000\r\n") != std::string::npos);
        reply = exchange(server, "HEAD"); status(reply, 200); assert(body(reply).empty());
        reply = exchange(server, "GET", "Range: bytes=65535-65537\r\n");
        status(reply, 206); assert(body(reply) == package.substr(65535, 3));
        assert(reply.find("Content-Range: bytes 65535-65537/150000\r\n") != std::string::npos);
        reply = exchange(server, "GET", "Range: bytes=-7\r\n"); status(reply, 206); assert(body(reply) == package.substr(package.size()-7));
        reply = exchange(server, "GET", "Range: bytes=149999-\r\n"); status(reply, 206); assert(body(reply) == package.substr(149999));
        reply = exchange(server, "HEAD", "Range: bytes=1-2\r\n"); status(reply, 200); assert(body(reply).empty());
        assert(reply.find("Content-Length: 150000\r\n") != std::string::npos && reply.find("Content-Range:") == std::string::npos);
        reply = exchange(server, "GET", "Range: bytes=150000-\r\n"); status(reply, 416);
        assert(reply.find("Content-Range: bytes */150000\r\n") != std::string::npos);
        status(exchange(server, "POST"), 405);
        status(exchange(server, "GET", "", "/../etc/passwd"), 404);
        status(exchange(server, "GET", "", "/token.pkg?extra"), 404);
        status(exchange(server, "GET", "", "http://127.0.0.1/token.pkg"), 404);
        status(exchange(server, "GET", "Range: bytes=0-1\r\nRange: bytes=3-4\r\n"), 400);
        std::string exactPath = strchr(server.url() + 7, '/');
        reply = exchange(server, "GET", "", (exactPath + "?extra=1").c_str());
        status(reply, 200); assert(body(reply) == package);
        reply = exchange(server, "HEAD", "", (std::string(server.url()) + "?download=1").c_str());
        status(reply, 200); assert(body(reply).empty());
        reply = exchange(server, "GET", "Range: bytes=65535-65537\r\n",
                         (std::string(server.url()) + "?download=1&x=%2f").c_str());
        status(reply, 206); assert(body(reply) == package.substr(65535, 3));
        status(exchange(server, "GET", "", (exactPath + "x").c_str()), 404);
        std::string atCap = "GET " + exactPath + " HTTP/1.1\r\nX-Fill: ";
        atCap.append(HEADER_CAP - atCap.size() - 4, 'A');
        atCap += "\r\n\r\n";
        assert(atCap.size() == HEADER_CAP);
        auto exactCap = enqueue(atCap, 101);
        await([exactCap] { return closed(exactCap); });
        status(exactCap->outgoing, 200); assert(body(exactCap->outgoing) == package);
        auto huge = enqueue(std::string(HEADER_CAP, 'A'));
        await([huge] { return closed(huge); }); status(huge->outgoing, 431);
        assert(server.errorCode() == 0); // Malformed clients do not poison the source.
        auto stalled = enqueue("", 0, 0, 0, 0, true);
        await([stalled] { std::lock_guard<std::mutex> guard(lock); return stalled->configured; });
        uint64_t began = sceKernelGetProcessTime();
        joinFailure = -22;
        assert(server.stop() == -22 && sceKernelGetProcessTime() - began < 1000000);
        assert(closed(stalled) && joins == 2 && poolsDestroyed == 1);
        assert(fileno(file) >= 0 && fseeko(file, 0, SEEK_SET) == 0 && fgetc(file) == 0);
        assert(server.stop() == -22 && poolsDestroyed == 1);
    }
    // Native-shaped targets work in both forms; telemetry counts only actual
    // package bytes, including a client disconnect halfway through an output.
    reset();
    {
        PkgServer server;
        assert(server.start(file, package.size()) == 0);
        PkgServerSnapshot snapshot = server.snapshot();
        assert(snapshot.requests == 0 && snapshot.status == 0 && snapshot.sentBytes == 0);
        std::string tokenPath = strchr(server.url() + 7, '/');
        std::string reply = exchange(server, "GET", "Range: bytes=65530-65536\r\n",
            (tokenPath + "?download=1&native=%Z?opaque").c_str(), 1, 3, 2, 3);
        status(reply, 206); assert(body(reply) == package.substr(65530, 7));
        snapshot = server.snapshot();
        assert(snapshot.requests == 1 && snapshot.status == 206 && snapshot.sentBytes == 7);
        assert(server.start(file, package.size()) == ERROR_BUSY);
        assert(server.snapshot().requests == 1 && server.snapshot().sentBytes == 7);
        reply = exchange(server, "HEAD", "Range: bytes=1-2\r\n", (std::string(server.url()) + "?download=1").c_str());
        status(reply, 200); assert(body(reply).empty());
        snapshot = server.snapshot();
        assert(snapshot.requests == 2 && snapshot.status == 200 && snapshot.sentBytes == 7);
        status(exchange(server, "GET", "", (tokenPath + "x?download=1").c_str()), 404);
        snapshot = server.snapshot();
        assert(snapshot.requests == 3 && snapshot.status == 404 && snapshot.sentBytes == 7);
        status(exchange(server, "GET", "Range: bytes=150000-\r\n", (tokenPath + "?download=1").c_str()), 416);
        assert(server.snapshot().requests == 4 && server.snapshot().status == 416 && server.snapshot().sentBytes == 7);
        Request full = {false, false, 0, uint64_t(package.size() - 1), 200};
        char fullHeader[512];
        size_t fullHeaderSize = response(fullHeader, sizeof(fullHeader), full, package.size());
        assert(fullHeaderSize);
        auto partial = enqueue("GET " + tokenPath + "?download=1 HTTP/1.1\r\n\r\n", 0, 5, 0, 1, false, fullHeaderSize + 13);
        await([partial] { return closed(partial); });
        status(partial->outgoing, 200);
        assert(body(partial->outgoing) == package.substr(0, 13));
        snapshot = server.snapshot();
        assert(snapshot.requests == 5 && snapshot.status == 200 && snapshot.sentBytes == 20);
        auto timeout = enqueue("", 0, 0, 0, 0, true);
        await([timeout] { std::lock_guard<std::mutex> guard(lock); return timeout->recvCalls != 0; });
        clockOffset.fetch_add(IO_TIMEOUT + 1);
        await([timeout] { return closed(timeout); });
        status(timeout->outgoing, 408);
        snapshot = server.snapshot();
        assert(snapshot.requests == 6 && snapshot.status == 408 && snapshot.sentBytes == 20);
        assert(server.errorCode() == 0 && server.stop() == 0);
        snapshot = server.snapshot();
        assert(snapshot.requests == 6 && snapshot.status == 408 && snapshot.sentBytes == 20);
        assert(server.stop() == 0 && server.snapshot().sentBytes == 20);
        { std::lock_guard<std::mutex> guard(lock); closeCounts.erase(9); } // A newly created listener can reuse fd 9.
        assert(server.start(file, package.size()) == 0);
        snapshot = server.snapshot();
        assert(snapshot.requests == 0 && snapshot.status == 0 && snapshot.sentBytes == 0);
        status(exchange(server, "HEAD"), 200);
        assert(server.stop() == 0 && server.snapshot().requests == 1 && server.snapshot().sentBytes == 0);
        assert(fileno(file) >= 0);
    }
    // Startup failures close resources once, never the owner's FILE.
    reset(); optionResult = -71;
    { PkgServer server; assert(server.start(file, package.size()) == -71); assert(server.stop() == 0 && server.errorCode() == -71); }
    assert(closeCounts[9] == 1 && poolsDestroyed == 1 && fileno(file) >= 0);
    reset(); threadResult = -72;
    { PkgServer server; assert(server.start(file, package.size()) == -72); }
    assert(closeCounts[9] == 1 && poolsDestroyed == 1);
    reset(); bindResult = -73;
    { PkgServer server; assert(server.start(file, package.size()) == -73); }
    assert(closeCounts[9] == 1 && poolsDestroyed == 1);
    reset();
    { PkgServer server; assert(server.start(file, package.size()) == 0);
      acceptFailure = -74;
      await([&server] { return server.errorCode() == -74; });
      assert(server.stop() == 0 && server.errorCode() == -74); }
    assert(closeCounts[9] == 1 && poolsDestroyed == 1);
    reset();
    { PkgServer server; assert(server.start(file, package.size() + 1) == 0);
      auto client = enqueue(std::string("GET ") + strchr(server.url()+7, '/') + " HTTP/1.1\r\n\r\n");
      await([client] { return closed(client); }); assert(server.errorCode() == ERROR_FILE_READ);
      assert(server.stop() == 0 && server.errorCode() == ERROR_FILE_READ); }
    assert(fileno(file) >= 0);
    fclose(file);

    // Sparse file confirms seek and range lengths beyond the 32-bit boundary.
    reset(); FILE* large = tmpfile(); assert(large);
    const uint64_t big = 4294967301ULL;
    assert(fseeko(large, off_t(big - 3), SEEK_SET) == 0 && fwrite("XYZ", 1, 3, large) == 3 && fflush(large) == 0);
    { PkgServer server; assert(server.start(large, big) == 0);
      std::string reply = exchange(server, "GET", "Range: bytes=4294967298-4294967300\r\n");
      status(reply, 206); assert(body(reply) == "XYZ");
      assert(reply.find("Content-Range: bytes 4294967298-4294967300/4294967301") != std::string::npos);
      assert(server.stop() == 0); }
    fclose(large);
    puts("PKG source tests passed: loopback ABI, HTTP/ranges, bounded I/O, retries, cancellation and FILE ownership.");
}
