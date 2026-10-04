#include "pkg_server.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <orbis/Net.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

extern "C" int32_t* peppyPkgNetErrno() __asm__("sceNetErrnoLoc");
extern "C" int32_t peppyPkgModuleLoaded(OrbisSysModuleInternal)
    __asm__("sceSysmoduleIsLoadedInternal");

namespace {
const size_t HEADER_CAP = 8192, STREAM_CAP = 65536;
// Native PS4 values, rather than host Linux or Vita constants. OpenOrbis 0.5.4
// omits these socket options; public shadPS4 network/net.h documents their ABI.
const int NET_SOL_SOCKET = 0xffff, NET_SO_NBIO = 0x1200;
const int NET_EINTR = 4, NET_EWOULDBLOCK = 35;
const int32_t ERROR_MEMORY = -2300, ERROR_ARGUMENT = -2301;
const int32_t ERROR_FILE_READ = -2302, ERROR_BUSY = -2303, ERROR_ADDRESS = -2304;
const uint64_t IO_TIMEOUT = 10000000; // microseconds, per header or output block

struct SocketAddress {
    uint8_t length, family;
    uint16_t port;
    uint32_t address;
    uint8_t zero[8];
};
static_assert(sizeof(SocketAddress) == 16 && offsetof(SocketAddress, port) == 2 &&
              offsetof(SocketAddress, address) == 4, "PS4 sockaddr_in ABI");
static_assert(sizeof(off_t) == 8, "64-bit package offsets");

struct Request {
    bool head, partial;
    uint64_t first, last;
    int status;
};
bool cancelled(const int* flag) { return __atomic_load_n(flag, __ATOMIC_ACQUIRE) != 0; }
void publish(int32_t* destination, int32_t code) {
    int32_t empty = 0;
    __atomic_compare_exchange_n(destination, &empty, code, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
int nativeErrno() {
    int32_t* location = peppyPkgNetErrno();
    return location ? *location : 0;
}
bool retryable(int32_t rc, int err) {
    return err == NET_EINTR || err == NET_EWOULDBLOCK ||
           uint32_t(rc) == 0x80410104u || uint32_t(rc) == 0x80410123u;
}
bool expired(uint64_t began) { return sceKernelGetProcessTime() - began >= IO_TIMEOUT; }
int32_t nonblocking(int32_t socket) {
    int enabled = 1;
    return sceNetSetsockopt(socket, NET_SOL_SOCKET, NET_SO_NBIO, &enabled, sizeof(enabled));
}
void closeSocket(int32_t& socket, int32_t* error) {
    if (socket < 0) return;
    int32_t owned = socket;
    socket = -1; // No later cleanup can accidentally close a reused descriptor.
    int32_t rc = sceNetSocketClose(owned);
    if (rc < 0) publish(error, rc);
}
bool sendAll(int32_t socket, const void* data, size_t bytes, const int* stop) {
    const char* cursor = static_cast<const char*>(data);
    uint64_t began = sceKernelGetProcessTime();
    while (bytes && !cancelled(stop) && !expired(began)) {
        int32_t sent = sceNetSend(socket, cursor, bytes, 0);
        int err = sent < 0 ? nativeErrno() : 0; // Capture before any other native call.
        if (sent > 0 && size_t(sent) <= bytes) {
            cursor += sent;
            bytes -= size_t(sent);
        } else if (sent < 0 && retryable(sent, err)) {
            sceKernelUsleep(1000);
        } else return false; // Client disconnects are not server-wide failures.
    }
    return bytes == 0;
}
int readHeader(int32_t socket, char* header, const int* stop) {
    size_t used = 0;
    uint64_t began = sceKernelGetProcessTime();
    while (!cancelled(stop) && !expired(began)) {
        if (used == HEADER_CAP) return 431;
        int32_t count = sceNetRecv(socket, header + used, HEADER_CAP - used, 0);
        int err = count < 0 ? nativeErrno() : 0;
        if (count > 0 && size_t(count) <= HEADER_CAP - used) {
            size_t previous = used;
            used += size_t(count);
            size_t begin = previous > 3 ? previous - 3 : 0;
            for (size_t i = begin; i + 3 < used; ++i) {
                if (!memcmp(header + i, "\r\n\r\n", 4)) {
                    header[i + 4] = 0;
                    return 0;
                }
            }
        } else if (count < 0 && retryable(count, err)) {
            sceKernelUsleep(1000);
        } else return 400;
    }
    return cancelled(stop) ? -1 : 408;
}
bool asciiEqual(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        unsigned char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return !*a && !*b;
}
bool decimal(const char* begin, const char* end, uint64_t& value) {
    if (begin == end) return false;
    value = 0;
    for (const char* p = begin; p != end; ++p) {
        if (*p < '0' || *p > '9') return false;
        unsigned digit = *p - '0';
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}
bool range(const char* value, uint64_t bytes, Request& out) {
    if (strncmp(value, "bytes=", 6) || !bytes) return false;
    const char* begin = value + 6;
    const char* dash = strchr(begin, '-');
    const char* end = begin + strlen(begin);
    if (!dash || strchr(dash + 1, '-') || strchr(begin, ',')) return false;
    uint64_t first = 0, last = 0;
    if (dash == begin) {
        uint64_t suffix = 0;
        if (!decimal(dash + 1, end, suffix) || !suffix) return false;
        first = suffix >= bytes ? 0 : bytes - suffix;
        last = bytes - 1;
    } else {
        if (!decimal(begin, dash, first) || first >= bytes) return false;
        if (dash + 1 == end) last = bytes - 1;
        else if (!decimal(dash + 1, end, last) || last < first) return false;
        if (last >= bytes) last = bytes - 1;
    }
    out.partial = true;
    out.first = first;
    out.last = last;
    return true;
}
bool headerName(const char* name) {
    if (!*name) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) continue;
        if (!strchr("!#$%&'*+-.^_`|~", *p)) return false;
    }
    return true;
}
Request parse(char* header, const char* path, uint64_t bytes) {
    Request out = {false, false, 0, bytes ? bytes - 1 : 0, 400};
    // Validate framing/controls before making any C-string parser assumptions.
    // readHeader guarantees a terminator, and its caller zeroes the full buffer;
    // embedded NUL must be distinguished from that final terminator.
    size_t length = 0;
    for (; length < HEADER_CAP; ++length) {
        unsigned char c = header[length];
        if (length >= 3 && !memcmp(header + length - 3, "\r\n\r\n", 4)) { ++length; break; }
        if (!c || c == 127 || (c < 32 && c != '\r' && c != '\n' && c != '\t')) return out;
        if (c == '\n' && (length == 0 || header[length - 1] != '\r')) return out;
        if (c == '\r' && header[length + 1] != '\n') return out;
    }
    if (length > HEADER_CAP || length < 4 || memcmp(header + length - 4, "\r\n\r\n", 4)) return out;
    char* lineEnd = strstr(header, "\r\n");
    if (!lineEnd) return out;
    *lineEnd = 0;
    char* target = strchr(header, ' ');
    if (!target) return out;
    *target++ = 0;
    char* version = strchr(target, ' ');
    if (!version) return out;
    *version++ = 0;
    if (strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1")) return out;
    out.head = !strcmp(header, "HEAD");
    if (strcmp(header, "GET") && !out.head) { out.status = 405; return out; }
    // BGFT may canonicalize the loopback URL before requesting it (for example\n    // an absolute-form request target). Accept the exact path or an absolute\n    // http://127.0.0.1:<port><path> target, while still rejecting every other path.\n    bool targetOk = !strcmp(target, path);\n    if (!targetOk && !strncmp(target, "http://127.0.0.1:", 17)) {\n        const char* slash = strchr(target + 17, '/');\n        targetOk = slash && !strcmp(slash, path);\n    }\n    if (!targetOk) { out.status = 404; return out; }
    bool hasRange = false, hasLength = false;
    char* cursor = lineEnd + 2;
    while (*cursor) {
        char* next = strstr(cursor, "\r\n");
        if (!next) return out;
        if (next == cursor) break;
        *next = 0;
        char* colon = strchr(cursor, ':');
        if (!colon) return out;
        *colon++ = 0;
        if (!headerName(cursor)) return out;
        while (*colon == ' ' || *colon == '\t') ++colon;
        char* valueEnd = colon + strlen(colon);
        while (valueEnd > colon && (valueEnd[-1] == ' ' || valueEnd[-1] == '\t')) *--valueEnd = 0;
        if (asciiEqual(cursor, "Range")) {
            if (hasRange) return out;
            hasRange = true;
            // RFC 7233: a Range header is applied only to GET. HEAD describes
            // the full representation and sends no body.
            if (!out.head && !range(colon, bytes, out)) { out.status = 416; return out; }
        } else if (asciiEqual(cursor, "Content-Length")) {
            uint64_t amount;
            if (hasLength || !decimal(colon, valueEnd, amount) || amount != 0) return out;
            hasLength = true;
        } else if (asciiEqual(cursor, "Transfer-Encoding")) return out;
        cursor = next + 2;
    }
    out.status = out.partial ? 206 : 200;
    return out;
}
const char* statusName(int status) {
    switch (status) {
    case 200: return "OK";
    case 206: return "Partial Content";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 416: return "Range Not Satisfiable";
    case 431: return "Request Header Fields Too Large";
    default: return "Bad Request";
    }
}
size_t response(char* output, size_t cap, const Request& request, uint64_t bytes) {
    uint64_t length = request.status == 200 ? bytes :
                      request.status == 206 ? request.last - request.first + 1 : 0;
    int n = snprintf(output, cap, "HTTP/1.1 %d %s\r\nContent-Length: %llu\r\n"
                     "Content-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\nConnection: close\r\n",
                     request.status, statusName(request.status), (unsigned long long)length);
    if (n < 0 || size_t(n) >= cap) return 0;
    size_t used = size_t(n);
    if (request.status == 206)
        n = snprintf(output + used, cap - used, "Content-Range: bytes %llu-%llu/%llu\r\n",
                     (unsigned long long)request.first, (unsigned long long)request.last, (unsigned long long)bytes);
    else if (request.status == 416)
        n = snprintf(output + used, cap - used, "Content-Range: bytes */%llu\r\n", (unsigned long long)bytes);
    else if (request.status == 405) n = snprintf(output + used, cap - used, "Allow: GET, HEAD\r\n");
    else n = 0;
    if (n < 0 || size_t(n) >= cap - used) return 0;
    used += size_t(n);
    if (cap - used < 3) return 0;
    memcpy(output + used, "\r\n", 3);
    return used + 2;
}
} // namespace

struct PkgServer::State {
    FILE* file;
    uint64_t bytes;
    int32_t listener, client, pool;
    int32_t error, cleanupError;
    int stop, alive;
    bool joinable;
    OrbisPthread thread;
    char url[192], path[80];
};

PkgServer::PkgServer() : state_(static_cast<State*>(calloc(1, sizeof(State)))) {
    if (state_) state_->listener = state_->client = state_->pool = -1;
}
PkgServer::~PkgServer() { stop(); free(state_); }
int32_t PkgServer::errorCode() const {
    if (!state_) return ERROR_MEMORY;
    int32_t primary = __atomic_load_n(&state_->error, __ATOMIC_ACQUIRE);
    return primary ? primary : __atomic_load_n(&state_->cleanupError, __ATOMIC_ACQUIRE);
}
const char* PkgServer::url() const { return state_ ? state_->url : ""; }

int32_t PkgServer::start(FILE* file, uint64_t bytes) {
    if (!state_) return ERROR_MEMORY;
    State& state = *state_;
    if (__atomic_load_n(&state.alive, __ATOMIC_ACQUIRE)) return ERROR_BUSY;
    stop();
    __atomic_store_n(&state.error, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&state.cleanupError, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&state.stop, 0, __ATOMIC_RELEASE);
    state.url[0] = state.path[0] = 0;
    if (!file || !bytes || bytes > uint64_t(INT64_MAX)) {
        publish(&state.error, ERROR_ARGUMENT);
        return errorCode();
    }
    state.file = file;
    state.bytes = bytes;
    int32_t rc = 0;
    if (peppyPkgModuleLoaded(ORBIS_SYSMODULE_INTERNAL_NET) != 0) {
        rc = int32_t(sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET));
        if (rc < 0 && peppyPkgModuleLoaded(ORBIS_SYSMODULE_INTERNAL_NET) != 0) {
            publish(&state.error, rc);
            return errorCode();
        }
    }
    // An existing global network is expected after a download. Pool/socket
    // creation authoritatively determines usability; no guessed init-error code.
    sceNetInit();
    state.pool = sceNetPoolCreate("peppy-pkg-source", 1024 * 1024, 0);
    if (state.pool < 0) { publish(&state.error, state.pool); stop(); return errorCode(); }
    state.listener = sceNetSocket("peppy-pkg-http", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    if (state.listener < 0) { publish(&state.error, state.listener); stop(); return errorCode(); }
    rc = nonblocking(state.listener);
    if (rc != 0) { publish(&state.error, rc); stop(); return errorCode(); }
    SocketAddress address = {};
    address.length = sizeof(address);
    address.family = ORBIS_NET_AF_INET;
    address.address = sceNetHtonl(0x7f000001); // Never INADDR_ANY or a LAN address.
    rc = sceNetBind(state.listener, reinterpret_cast<OrbisNetSockaddr*>(&address), sizeof(address));
    if (rc != 0) { publish(&state.error, rc); stop(); return errorCode(); }
    rc = sceNetListen(state.listener, 4);
    if (rc != 0) { publish(&state.error, rc); stop(); return errorCode(); }
    OrbisNetSocklen_t length = sizeof(address);
    rc = sceNetGetsockname(state.listener, reinterpret_cast<OrbisNetSockaddr*>(&address), &length);
    if (rc != 0 || length != sizeof(address) || address.family != ORBIS_NET_AF_INET ||
        address.address != sceNetHtonl(0x7f000001) || !address.port) {
        publish(&state.error, rc ? rc : ERROR_ADDRESS); stop(); return errorCode();
    }
    static uint32_t nonceCounter = 0;
    uint32_t nonce = __atomic_add_fetch(&nonceCounter, 1, __ATOMIC_RELAXED);
    snprintf(state.path, sizeof(state.path), "/peppy-%016llx-%08x.pkg",
             (unsigned long long)sceKernelGetProcessTime(), (unsigned)nonce);
    snprintf(state.url, sizeof(state.url), "http://127.0.0.1:%u%s", unsigned(sceNetNtohs(address.port)), state.path);
    __atomic_store_n(&state.alive, 1, __ATOMIC_RELEASE);
    rc = scePthreadCreate(&state.thread, 0, worker, &state, "peppy-pkg-http");
    if (rc != 0) {
        __atomic_store_n(&state.alive, 0, __ATOMIC_RELEASE);
        publish(&state.error, rc); stop(); return errorCode();
    }
    state.joinable = true;
    return 0;
}

void PkgServer::serve(State* state, int32_t client) {
    char header[HEADER_CAP + 1] = {};
    int status = readHeader(client, header, &state->stop);
    if (status < 0) return;
    Request request = status ? Request{false, false, 0, 0, status} : parse(header, state->path, state->bytes);
    if ((request.status == 200 || request.status == 206) && !request.head) {
        if (fseeko(state->file, off_t(request.first), SEEK_SET) != 0) {
            publish(&state->error, ERROR_FILE_READ);
            return;
        }
    }
    char outgoing[512];
    size_t length = response(outgoing, sizeof(outgoing), request, state->bytes);
    if (!length || !sendAll(client, outgoing, length, &state->stop)) return;
    if (request.head || (request.status != 200 && request.status != 206)) return;
    uint64_t remaining = request.last - request.first + 1;
    unsigned char block[STREAM_CAP];
    while (remaining && !cancelled(&state->stop)) {
        size_t amount = remaining > sizeof(block) ? sizeof(block) : size_t(remaining);
        if (fread(block, 1, amount, state->file) != amount) {
            publish(&state->error, ERROR_FILE_READ);
            return;
        }
        if (!sendAll(client, block, amount, &state->stop)) return;
        remaining -= amount;
    }
}

void* PkgServer::worker(void* argument) {
    State* state = static_cast<State*>(argument);
    while (!cancelled(&state->stop) && !__atomic_load_n(&state->error, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&state->cleanupError, __ATOMIC_ACQUIRE)) {
        SocketAddress peer = {};
        OrbisNetSocklen_t length = sizeof(peer);
        int32_t client = sceNetAccept(state->listener, reinterpret_cast<OrbisNetSockaddr*>(&peer), &length);
        int err = client < 0 ? nativeErrno() : 0;
        if (client < 0) {
            if (retryable(client, err)) { sceKernelUsleep(1000); continue; }
            publish(&state->error, client);
            break;
        }
        state->client = client;
        if (length == sizeof(peer) && peer.family == ORBIS_NET_AF_INET &&
            peer.address == sceNetHtonl(0x7f000001)) {
            int32_t rc = nonblocking(client);
            if (rc == 0) serve(state, client);
            else publish(&state->error, rc);
        }
        closeSocket(state->client, &state->cleanupError);
    }
    closeSocket(state->client, &state->cleanupError);
    closeSocket(state->listener, &state->cleanupError);
    __atomic_store_n(&state->alive, 0, __ATOMIC_RELEASE); // Last access by worker.
    return 0;
}

int32_t PkgServer::stop() {
    if (!state_) return 0; // Allocation failed before any worker/resources existed.
    State& state = *state_;
    __atomic_store_n(&state.stop, 1, __ATOMIC_RELEASE);
    if (state.joinable) {
        int32_t rc = scePthreadJoin(state.thread, 0);
        if (rc != 0) {
            // Even an unexpected native join error cannot release borrowed FILE
            // while it is in use. Nonblocking sockets observe stop every retry.
            while (__atomic_load_n(&state.alive, __ATOMIC_ACQUIRE)) sceKernelUsleep(1000);
            int32_t retry = scePthreadJoin(state.thread, 0);
            publish(&state.cleanupError, retry ? retry : rc);
        }
        state.joinable = false;
    }
    // Before worker creation, start owns these resources; after a join the
    // worker already set their descriptors to -1, so cleanup never repeats close.
    closeSocket(state.client, &state.cleanupError);
    closeSocket(state.listener, &state.cleanupError);
    // OpenOrbis 0.5.4 exposes this destructor as void; do not invent a status ABI.
    if (state.pool >= 0) { sceNetPoolDestroy(state.pool); state.pool = -1; }
    state.file = 0;
    return __atomic_load_n(&state.cleanupError, __ATOMIC_ACQUIRE);
}
