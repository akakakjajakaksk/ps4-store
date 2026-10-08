#include <assert.h>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>
#include <thread>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

namespace {
std::string inboxRoot;
int hostOpen(const char*, int, ...);
int hostMkdir(const char*, mode_t);
int hostRemove(const char*);
int hostRename(const char*, const char*);
DIR* hostOpendir(const char*);
}
#define open hostOpen
#define mkdir hostMkdir
#define remove hostRemove
#define rename hostRename
#define opendir hostOpendir
#include "../../ftp_receiver.cpp"
#undef open
#undef mkdir
#undef remove
#undef rename
#undef opendir

namespace {
std::atomic<uint64_t> timeOffset(0);
std::atomic<int> sendInterrupt(0), receiveInterrupt(0), threadError(0), optionError(0), bindError(0);
std::atomic<int32_t> poolResult(4);
std::atomic<unsigned> poolsDestroyed(0), bindCalls(0);
thread_local int32_t nativeError = 0;
std::string localPath(const char* path) {
    const char* prefix = "/data/peppy-store";
    assert(!strncmp(path, prefix, strlen(prefix)));
    return inboxRoot + "/peppy-store" + (path + strlen(prefix));
}
int hostOpen(const char* path, int flags, ...) {
    va_list args; va_start(args, flags);
    mode_t mode = flags & O_CREAT ? va_arg(args, int) : 0;
    va_end(args);
    return ::open(localPath(path).c_str(), flags, mode);
}
int hostMkdir(const char* path, mode_t mode) { return ::mkdir(localPath(path).c_str(), mode); }
int hostRemove(const char* path) { return ::remove(localPath(path).c_str()); }
int hostRename(const char* oldPath, const char* newPath) {
    return ::rename(localPath(oldPath).c_str(), localPath(newPath).c_str());
}
DIR* hostOpendir(const char* path) { return ::opendir(localPath(path).c_str()); }
int convertError(int error) {
    if (error == EAGAIN || error == EWOULDBLOCK) return 35;
    if (error == EADDRINUSE) return 48;
    if (error == ECONNRESET) return 54;
    if (error == ETIMEDOUT) return 60;
    return error;
}
int32_t result(int32_t rc) { if (rc < 0) nativeError = convertError(errno); return rc; }
void socketTimeout(int fd) {
    timeval timeout = {3, 0};
    assert(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    assert(!setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
}
int reservePort(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
    int enabled = 1;
    assert(!setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET; addr.sin_port = htons(port); addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(!bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
    assert(!listen(fd, 4)); return fd;
}
int connectTo(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
    socketTimeout(fd);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET; addr.sin_port = htons(port); addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(!connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))); return fd;
}
void clientSend(int fd, const std::string& text) {
    size_t done = 0;
    while (done < text.size()) {
        ssize_t sent = send(fd, text.data() + done, text.size() - done, MSG_NOSIGNAL);
        assert(sent > 0); done += size_t(sent);
    }
}
std::string response(int control) {
    std::string line;
    for (;;) {
        char c = 0;
        assert(recv(control, &c, 1, 0) == 1);
        line.push_back(c);
        if (c == '\n') break;
        assert(line.size() < 2048);
    }
    return line;
}
void expect(int control, int code) {
    std::string line = response(control);
    if (atoi(line.c_str()) != code) fprintf(stderr, "Expected %d, received %s", code, line.c_str());
    assert(atoi(line.c_str()) == code);
}
void command(int control, const std::string& text, int code) {
    clientSend(control, text + "\r\n"); expect(control, code);
}
int login(uint16_t port) {
    int control = connectTo(port); expect(control, 220);
    command(control, "USER anonymous", 331); command(control, "PASS peppy", 230);
    return control;
}
int dataChannel(int control, bool extended = true) {
    clientSend(control, extended ? "EPSV\r\n" : "PASV\r\n");
    std::string line = response(control);
    unsigned port = 0;
    if (extended) {
        assert(atoi(line.c_str()) == 229);
        const char* begin = strstr(line.c_str(), "(|||"); assert(begin);
        assert(sscanf(begin, "(|||%u|)", &port) == 1);
    } else {
        assert(atoi(line.c_str()) == 227);
        const char* begin = strchr(line.c_str(), '('); assert(begin);
        unsigned a, b, c, d, high, low;
        assert(sscanf(begin, "(%u,%u,%u,%u,%u,%u)", &a, &b, &c, &d, &high, &low) == 6);
        assert(a == 192 && b == 168 && c == 1 && d == 7);
        port = high * 256 + low;
    }
    assert(port >= PASSIVE_FIRST && port <= PASSIVE_LAST);
    return connectTo(uint16_t(port));
}
std::string dataBody(int data) {
    std::string body;
    char bytes[1024];
    for (;;) {
        ssize_t n = recv(data, bytes, sizeof(bytes), 0); assert(n >= 0);
        if (!n) break;
        body.append(bytes, size_t(n));
    }
    close(data); return body;
}
std::string listing(int control, const std::string& cmd, bool extended = true) {
    int data = dataChannel(control, extended);
    command(control, cmd, 150);
    std::string body = dataBody(data);
    expect(control, 226); return body;
}
std::string package(size_t bytes) {
    assert(bytes >= USER_PACKAGE_HEADER_BYTES);
    std::string body(bytes, '\x5a');
    memcpy(&body[0], "\x7f" "CNT", 4);
    for (int i = 0; i < 8; ++i) body[0x430 + i] = char(uint64_t(bytes) >> (56 - i * 8));
    return body;
}
void upload(int control, const std::string& target, const std::string& body, int completion = 226) {
    int data = dataChannel(control);
    command(control, "STOR " + target, 150);
    clientSend(data, body); assert(!shutdown(data, SHUT_WR));
    dataBody(data); expect(control, completion);
}
std::string fileContents(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb"); assert(file);
    std::string value;
    char buffer[1024];
    for (;;) {
        size_t bytes = fread(buffer, 1, sizeof(buffer), file);
        if (!bytes) break;
        value.append(buffer, bytes);
    }
    assert(!ferror(file)); fclose(file); return value;
}
void eraseTree(const std::string& path) {
    DIR* dir = ::opendir(path.c_str());
    if (!dir) { ::remove(path.c_str()); return; }
    for (dirent* entry = readdir(dir); entry; entry = readdir(dir)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        eraseTree(path + "/" + entry->d_name);
    }
    closedir(dir); assert(!rmdir(path.c_str()));
}
} // namespace

extern "C" int32_t sceKernelFstat(int32_t fd, PeppyFileInfo* info) {
    struct stat metadata = {};
    if (::fstat(fd, &metadata)) return -1;
    info->st_mode = metadata.st_mode; info->st_size = metadata.st_size;
    return 0;
}
extern "C" int32_t* sceNetErrnoLoc() { return &nativeError; }
extern "C" uint32_t sceNetHtonl(uint32_t v) { return htonl(v); }
extern "C" uint32_t sceNetNtohl(uint32_t v) { return ntohl(v); }
extern "C" uint16_t sceNetHtons(uint16_t v) { return htons(v); }
extern "C" int32_t sceNetInit() { return int32_t(0x80410111u); }
extern "C" int32_t sceNetCtlInit() { return int32_t(0x80410111u); }
extern "C" int32_t sceNetPoolCreate(const char*, int32_t bytes, int32_t flags) {
    assert(bytes == 1024 * 1024 && flags == 0); return poolResult;
}
extern "C" void sceNetPoolDestroy(int32_t id) { assert(id == 4); ++poolsDestroyed; }
extern "C" uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal) { return 0; }
extern "C" int32_t sceNetCtlGetInfo(int selector, OrbisNetCtlInfo* info) {
    assert(selector == ORBIS_NET_CTL_INFO_IP_ADDRESS); strcpy(info->ip_address, "192.168.1.7"); return 0;
}
extern "C" OrbisNetId sceNetSocket(const char*, int32_t domain, int32_t type, int protocol) {
    assert(domain == 2 && type == 1 && protocol == 0); return result(socket(AF_INET, SOCK_STREAM, 0));
}
extern "C" int32_t sceNetSetsockopt(OrbisNetId fd, int32_t level, int32_t option,
                                   const void* value, OrbisNetSocklen_t bytes) {
    assert(level == 0xffff && bytes == sizeof(int) && *static_cast<const int*>(value) == 1);
    if (optionError) { nativeError = optionError; return -1; }
    if (option == 0x0004) return result(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, value, bytes));
    assert(option == 0x1200);
    return result(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK));
}
extern "C" int32_t sceNetBind(OrbisNetId fd, const OrbisNetSockaddr* raw, OrbisNetSocklen_t bytes) {
    ++bindCalls;
    assert(bytes == 16);
    const SocketAddress* source = reinterpret_cast<const SocketAddress*>(raw);
    assert(source->length == 16 && source->family == 2 && source->address == 0);
    if (bindError) { nativeError = bindError; return -1; }
    sockaddr_in target = {};
    target.sin_family = AF_INET; target.sin_port = source->port; target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return result(bind(fd, reinterpret_cast<sockaddr*>(&target), sizeof(target)));
}
extern "C" int32_t sceNetListen(OrbisNetId fd, int count) { return result(listen(fd, count)); }
extern "C" int32_t sceNetGetsockname(OrbisNetId fd, OrbisNetSockaddr* raw, OrbisNetSocklen_t* bytes) {
    sockaddr_in host = {}; socklen_t length = sizeof(host);
    int rc = getsockname(fd, reinterpret_cast<sockaddr*>(&host), &length);
    if (rc) return result(rc);
    assert(*bytes == 16); SocketAddress* target = reinterpret_cast<SocketAddress*>(raw);
    target->length = 16; target->family = 2; target->address = htonl(0xc0a80107); target->port = host.sin_port;
    *bytes = 16; return 0;
}
extern "C" OrbisNetId sceNetAccept(OrbisNetId fd, OrbisNetSockaddr* raw, OrbisNetSocklen_t* bytes) {
    sockaddr_in host = {}; socklen_t length = sizeof(host);
    int accepted = accept(fd, reinterpret_cast<sockaddr*>(&host), &length);
    if (accepted < 0) return result(accepted);
    assert(*bytes == 16); SocketAddress* target = reinterpret_cast<SocketAddress*>(raw);
    target->length = 16; target->family = 2; target->address = htonl(0xc0a80163); target->port = host.sin_port;
    *bytes = 16; return accepted;
}
extern "C" int32_t sceNetRecv(OrbisNetId fd, void* data, size_t bytes, int flags) {
    assert(flags == 0);
    if (receiveInterrupt.exchange(0)) { nativeError = 4; return -1; }
    return result(int32_t(recv(fd, data, bytes, flags)));
}
extern "C" int32_t sceNetSend(OrbisNetId fd, const void* data, size_t bytes, int flags) {
    assert(flags == 0);
    if (sendInterrupt.exchange(0)) { nativeError = 4; return -1; }
    if (bytes > 17) bytes = 17; // Exercise partial responses/listing writes.
    return result(int32_t(send(fd, data, bytes, MSG_NOSIGNAL)));
}
extern "C" int32_t sceNetSocketClose(OrbisNetId fd) { return result(close(fd)); }
extern "C" uint64_t sceKernelGetProcessTime() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) + timeOffset.load();
}
extern "C" int32_t sceKernelUsleep(uint32_t micros) {
    std::this_thread::sleep_for(std::chrono::microseconds(micros)); return 0;
}
extern "C" int32_t scePthreadCreate(OrbisPthread* t, const OrbisPthreadAttr* a,
                                    void* (*entry)(void*), void* arg, const char*) {
    return threadError ? threadError.load() : pthread_create(t, a, entry, arg);
}
extern "C" int32_t scePthreadJoin(OrbisPthread t, void** value) { return pthread_join(t, value); }

int main() {
    signal(SIGPIPE, SIG_IGN);
    char folder[] = "/tmp/peppy-ftp-inbox-XXXXXX"; assert(mkdtemp(folder)); inboxRoot = folder;
    assert(safeName("Ação e Teste (PS4) [v1].pkg"));
    assert(safeName("Game..name.pkg"));
    assert(!safeName("../outside.pkg") && !safeName("a\\b.pkg") && !safeName("\xc0\xaf.pkg"));
    assert(!safeName("newline\n.pkg") && !safeName((std::string(192, 'x') + ".pkg").c_str()));

    // Real occupied listeners reproduce the user's native EADDRINUSE 0x80410130.
    int goldhen = reservePort(2121), firstFallback = reservePort(2150);
    assert(ftpReceiverStart());
    FtpReceiverSnapshot snapshot = ftpReceiverSnapshot();
    assert(snapshot.running && snapshot.port == 2151 && !snapshot.lastError);
    sendInterrupt = 1; receiveInterrupt = 1;
    int control = login(snapshot.port);
    assert(listing(control, "NLST") == "base\r\nupdate\r\ndlc\r\n");
    std::string facts = listing(control, "MLSD", false);
    assert(facts.find("type=dir;size=0;perm=el; base\r\n") != std::string::npos);
    assert(facts.find("drwx") == std::string::npos);
    command(control, "CWD /base/", 250); command(control, "CWD .", 250);
    std::string pwd;
    clientSend(control, "PWD\r\n"); pwd = response(control); assert(pwd == "257 \"/base\"\r\n");
    const std::string payload = package(150000);
    const std::string name = "Ação e Teste (PS4) [v1].pkg";
    upload(control, name, payload);
    assert(fileContents(inboxRoot + "/peppy-store/inbox/base/" + name) == payload);
    assert(listing(control, "NLST") == name + "\r\n");
    facts = listing(control, "MLSD /base");
    assert(facts == "type=file;size=150000;perm=w; " + name + "\r\n");
    clientSend(control, "SIZE " + name + "\r\n"); assert(response(control) == "213 150000\r\n");
    std::string longName = std::string(170, 'x') + ".pkg";
    upload(control, "/update/" + longName, payload);
    upload(control, "/dlc/Teste.pkg", payload);
    command(control, "STOR ../outside/hack.pkg", 553);
    command(control, "STORAGE sneaky.pkg", 502);

    // EOF at any point must not publish a partial .pkg or destroy an old one.
    upload(control, "truncated.pkg", payload.substr(0, 2000), 451);
    assert(access((inboxRoot + "/peppy-store/inbox/base/truncated.pkg").c_str(), F_OK));
    upload(control, name, payload.substr(0, 5000), 451);
    assert(fileContents(inboxRoot + "/peppy-store/inbox/base/" + name) == payload);
    upload(control, "oversized.pkg", package(1080) + "extra", 451);
    upload(control, "empty.pkg", "", 451);
    FtpInboxItem items[128]; int count = ftpInboxList(items, 128);
    assert(count == 3 && items[0].kind == FTP_BASE && items[1].kind == FTP_UPDATE && items[2].kind == FTP_DLC);
    assert(std::string(items[1].name) == longName);
    snapshot = ftpReceiverSnapshot();
    assert(snapshot.filesReceived == 3 && snapshot.bytesReceived == 450000 && snapshot.lastError == ERROR_PACKAGE);

    // Listing covers more than the old 64-item cap, with no .part exposure.
    for (int i = 0; i < 80; ++i) {
        std::string path = inboxRoot + "/peppy-store/inbox/base/extra" + std::to_string(i) + ".pkg";
        FILE* file = fopen(path.c_str(), "wb"); assert(file);
        const std::string bytes = package(1080);
        assert(fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size()); assert(!fclose(file));
    }
    std::string names = listing(control, "NLST /base");
    assert(names.find("extra79.pkg\r\n") != std::string::npos);
    assert(names.find(".part") == std::string::npos);

    // An abandoned PASV listener and a stalled STOR cannot freeze the server.
    clientSend(control, "EPSV\r\n"); expect(control, 229);
    command(control, "STOR wait.pkg", 150);
    timeOffset.fetch_add(DATA_TIMEOUT + 1); expect(control, 425);
    command(control, "NOOP", 200);
    int data = dataChannel(control); command(control, "STOR stalled.pkg", 150);
    clientSend(data, payload.substr(0, 100));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    timeOffset.fetch_add(DATA_TIMEOUT + 1); expect(control, 451); close(data);
    command(control, "NOOP", 200);
    assert(ftpReceiverSnapshot().lastError == ERROR_TIMEOUT);

    // Stop while an uploader keeps its socket open; the worker owns cleanup.
    data = dataChannel(control); command(control, "STOR cancelled.pkg", 150);
    clientSend(data, payload.substr(0, 100));
    auto began = std::chrono::steady_clock::now();
    ftpReceiverStop();
    assert(std::chrono::steady_clock::now() - began < std::chrono::seconds(1));
    close(data); close(control);
    assert(!ftpReceiverSnapshot().running);
    assert(access((inboxRoot + "/peppy-store/inbox/base/.cancelled.pkg.part").c_str(), F_OK));

    // Reused libnet, absent optional pool, and restart remain supported.
    poolResult = -1; assert(ftpReceiverStart());
    control = login(ftpReceiverSnapshot().port); command(control, "QUIT", 221); close(control);
    ftpReceiverStop(); poolResult = 4;
    threadError = 12; assert(!ftpReceiverStart()); assert(ftpReceiverSnapshot().lastError == 12);
    threadError = 0; assert(ftpReceiverStart()); ftpReceiverStop();
    unsigned previousBinds = bindCalls;
    bindError = EACCES; assert(!ftpReceiverStart());
    assert(uint32_t(ftpReceiverSnapshot().lastError) == 0x8041010du && bindCalls == previousBinds + 1);
    bindError = 0; optionError = EINVAL; assert(!ftpReceiverStart());
    assert(uint32_t(ftpReceiverSnapshot().lastError) == 0x80410116u); optionError = 0;
    close(goldhen); close(firstFallback);
    assert(ftpReceiverStart() && ftpReceiverSnapshot().port == 2121); ftpReceiverStop();
    eraseTree(inboxRoot);
    puts("FTP receiver integration tests passed");
}
