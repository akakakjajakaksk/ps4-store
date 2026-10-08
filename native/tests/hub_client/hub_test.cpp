#include "../../hub_client.cpp"
#include <assert.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <mutex>

struct Reply {
    std::string url, body, headers, bearer, range;
    int status, method, sendError, readError, lengthType;
    bool eofAllowed, oversizedRead;
    size_t cursor, failureAfter, contentLength, fragment;
    Reply(const std::string& u, const std::string& b, int s = 200, int m = 0)
        : url(u), body(b), status(s), method(m), sendError(0), readError(0), lengthType(ORBIS_HTTP_CONTENTLEN_EXIST),
          eofAllowed(false), oversizedRead(false), cursor(0), failureAfter(SIZE_MAX), contentLength(b.size()), fragment(11) {
        headers = "HTTP/1.1 " + std::to_string(s) + " Response\r\nContent-Length: " + std::to_string(b.size()) + "\r\nContent-Type: application/json\r\n\r\n";
    }
};
static std::vector<Reply> replies;
static std::atomic<size_t> opened(0), reads(0), connections(0), requestDeletes(0), connectionDeletes(0);
static std::atomic<int> contexts(0), pools(0), sslContexts(0), templates(0), resolverCount(0);
static int threadFailure;
static std::atomic<bool> privateDns(false), badDns(false), secure(false), noRedirect(false);
static thread_local std::string connectionUrl, addedBearer, addedRange;
static thread_local size_t activeReplyIndex = SIZE_MAX;
static std::string sentBody;
static std::mutex sentBodyLock;
static std::atomic<size_t> heldReplyIndex(SIZE_MAX);
static std::atomic<bool> holdSend(false), insideSend(false), aborted(false);
static std::atomic<uint64_t> clockAdvanceUs(0);
static const char* ORIGIN = "https://peppy.example.org";
static const char* PKG = "https://github.com/skidgfx/PS4-2048/releases/download/v1.0/game.pkg";
static const std::string TOKEN(64, 'a');
static Reply& reply() { assert(activeReplyIndex < replies.size()); return replies[activeReplyIndex]; }

extern "C" int32_t sceKernelUsleep(uint32_t micros) { std::this_thread::sleep_for(std::chrono::microseconds(micros)); return 0; }
extern "C" uint64_t mockUptime() __asm__("sceKernelGetProcessTime");
extern "C" uint64_t mockUptime() { return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) + clockAdvanceUs.load(); }
extern "C" int32_t scePthreadAttrInit(OrbisPthreadAttr* attr) { *attr = 0; return threadFailure == 1 ? -11 : 0; }
extern "C" int32_t scePthreadAttrSetdetachstate(OrbisPthreadAttr* attr, int state) { assert(state == 1); *attr = 1; return threadFailure == 2 ? -12 : 0; }
extern "C" int32_t scePthreadAttrDestroy(OrbisPthreadAttr*) { return 0; }
extern "C" int32_t scePthreadCreate(OrbisPthread* thread, const OrbisPthreadAttr* attr, void* (*fn)(void*), void* arg, const char*) {
    assert(*attr == 1); if (threadFailure == 3) return -13;
    *thread = 1; std::thread(fn, arg).detach(); return 0;
}
extern "C" uint32_t sceSysmoduleLoadModuleInternal(OrbisSysModuleInternal) { return 0; }
extern "C" int32_t mockModule(OrbisSysModuleInternal) __asm__("sceSysmoduleIsLoadedInternal");
extern "C" int32_t mockModule(OrbisSysModuleInternal) { return 0; }
extern "C" int32_t sceNetCtlInit() { return 0; }
extern "C" int32_t mockState(int32_t*) __asm__("sceNetCtlGetState");
extern "C" int32_t mockState(int32_t* state) { *state = 3; return 0; }
extern "C" int32_t sceNetInit() { return 0; }
extern "C" int32_t sceNetPoolCreate(const char*, int32_t, int32_t) { ++pools; return 1; }
extern "C" void sceNetPoolDestroy(int32_t) { --pools; }
extern "C" int32_t sceSslInit(size_t) { ++sslContexts; return 2; }
extern "C" int32_t mockSsl(int32_t) __asm__("sceSslTerm");
extern "C" int32_t mockSsl(int32_t) { --sslContexts; return 0; }
extern "C" int32_t sceHttpInit(int32_t, int32_t, size_t) { ++contexts; return 3; }
extern "C" int32_t sceHttpTerm(int32_t) { --contexts; return 0; }
extern "C" int32_t sceHttpCreateTemplate(int32_t, const char*, int32_t, int32_t proxy) { assert(!proxy); ++templates; return 4; }
extern "C" int32_t sceHttpDeleteTemplate(int32_t) { --templates; return 0; }
extern "C" int32_t mockHeaderLimit(int32_t, size_t) __asm__("sceHttpSetResponseHeaderMaxSize");
extern "C" int32_t mockHeaderLimit(int32_t, size_t bytes) { assert(bytes == 65536); return 0; }
extern "C" int32_t sceHttpsEnableOption(int32_t, uint32_t flags) { assert(flags == 0xbd); secure = true; return 0; }
extern "C" int32_t mockRedirect(int32_t, int32_t) __asm__("sceHttpSetAutoRedirect");
extern "C" int32_t mockRedirect(int32_t, int32_t enabled) { assert(!enabled); noRedirect = true; return 0; }
extern "C" int32_t mockTimeout(int32_t, uint32_t) __asm__("sceHttpSetRecvTimeOut");
extern "C" int32_t mockTimeout(int32_t, uint32_t value) { assert(value <= 15000000); return 0; }
extern "C" int32_t sceHttpSetResolveTimeOut(int32_t, uint32_t) { return 0; }
extern "C" int32_t sceHttpSetConnectTimeOut(int32_t, uint32_t) { return 0; }
extern "C" int32_t sceHttpSetSendTimeOut(int32_t, uint32_t) { return 0; }
extern "C" int32_t mockResolverCreate(const char*, int32_t, int32_t) __asm__("sceNetResolverCreate");
extern "C" int32_t mockResolverCreate(const char*, int32_t, int32_t) { ++resolverCount; return 9; }
extern "C" int32_t mockResolverDestroy(int32_t) __asm__("sceNetResolverDestroy");
extern "C" int32_t mockResolverDestroy(int32_t) { --resolverCount; return 0; }
extern "C" int32_t mockLookup(int32_t, const char*, PeppyHubAddress*, int32_t, int32_t, int32_t) __asm__("sceNetResolverStartNtoa");
extern "C" int32_t mockLookup(int32_t, const char* hostname, PeppyHubAddress* address, int32_t timeout, int32_t retry, int32_t) {
    assert(hostname && *hostname && timeout == 5000000 && retry == 1);
    unsigned char* bytes = reinterpret_cast<unsigned char*>(&address->address);
    bytes[0] = privateDns ? 10 : 185; bytes[1] = 199; bytes[2] = 108; bytes[3] = 133;
    return badDns ? -21 : 0;
}
extern "C" int32_t sceHttpCreateConnectionWithURL(int32_t, const char* url, bool keepAlive) {
    assert(secure && noRedirect && !keepAlive); activeReplyIndex = connections.fetch_add(1);
    assert(activeReplyIndex < replies.size() && replies[activeReplyIndex].url == url); connectionUrl = url; return 5 + int(activeReplyIndex);
}
extern "C" int32_t sceHttpCreateRequestWithURL(int32_t, int32_t method, const char* url, uint64_t bytes) {
    assert(connectionUrl == url && reply().method == method);
    if (method == 0) assert(!bytes);
    addedBearer.clear(); addedRange.clear(); ++opened; return 11 + int(activeReplyIndex);
}
extern "C" int32_t sceHttpAddRequestHeader(int32_t, const char* key, const char* value, int32_t) {
    if (!strcmp(key, "Authorization")) addedBearer = value;
    if (!strcmp(key, "Range")) addedRange = value;
    if (!strcmp(key, "Accept-Encoding")) assert(!strcmp(value, "identity"));
    return 0;
}
extern "C" int32_t sceHttpSendRequest(int32_t, const void* body, size_t bytes) {
    assert(addedBearer == reply().bearer && addedRange == reply().range);
    { std::lock_guard<std::mutex> guard(sentBodyLock); sentBody = body ? std::string(static_cast<const char*>(body), bytes) : ""; }
    insideSend = true;
    while (holdSend && (heldReplyIndex == SIZE_MAX || heldReplyIndex == activeReplyIndex) && !aborted) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return aborted ? -99 : reply().sendError;
}
extern "C" int32_t sceHttpGetStatusCode(int32_t, int32_t* status) { *status = reply().status; return 0; }
extern "C" int32_t sceHttpGetAllResponseHeaders(int32_t, char** output, size_t* bytes) {
    *output = &reply().headers[0]; *bytes = reply().headers.size(); return 0;
}
extern "C" int32_t sceHttpGetResponseContentLength(int32_t, int32_t* type, size_t* bytes) {
    *type = reply().lengthType; *bytes = reply().contentLength; return 0;
}
extern "C" int32_t sceHttpReadData(int32_t, void* output, uint32_t capacity) {
    ++reads; Reply& r = reply();
    if (r.cursor >= r.failureAfter) return r.readError;
    if (r.cursor == r.body.size()) { assert(r.eofAllowed && "must not read beyond exact framed response"); return 0; }
    if (r.oversizedRead) return int32_t(capacity + 1);
    size_t bytes = r.body.size() - r.cursor; if (bytes > capacity) bytes = capacity; if (bytes > r.fragment) bytes = r.fragment;
    memcpy(output, r.body.data() + r.cursor, bytes); r.cursor += bytes; return int32_t(bytes);
}
extern "C" int32_t sceHttpAbortRequest(int32_t) { aborted = true; return 0; }
extern "C" int32_t sceHttpDeleteRequest(int32_t) { ++requestDeletes; return 0; }
extern "C" int32_t sceHttpDeleteConnection(int32_t) { ++connectionDeletes; return 0; }

static void cleanHandles() { assert(!contexts && !pools && !sslContexts && !templates && !resolverCount); assert(opened == requestDeletes && connections == connectionDeletes); }
static HubResult finished() {
    for (int i = 0; i < 10000; ++i) {
        HubResult result = {};
        if (consumeHubResult(&result)) { cleanHandles(); return result; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false); return HubResult{};
}
static void sessionChecked() {
    for (int i = 0; i < 10000 && __atomic_load_n(&g_sessionCheckBusy, __ATOMIC_ACQUIRE); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(!__atomic_load_n(&g_sessionCheckBusy, __ATOMIC_ACQUIRE)); cleanHandles();
}
static void reset() {
    assert(!__atomic_load_n(&g_busy, __ATOMIC_ACQUIRE) && !__atomic_load_n(&g_sessionCheckBusy, __ATOMIC_ACQUIRE)); cleanHandles();
    lock(); delete g_result.catalog; g_result = {}; g_snapshot = {}; clearSessionLocked(); unlock();
    g_cancel = 0; g_request = -1; clockAdvanceUs = 0;
    replies.clear(); opened = reads = connections = requestDeletes = connectionDeletes = 0;
    contexts = pools = sslContexts = templates = resolverCount = threadFailure = 0;
    privateDns = badDns = secure = noRedirect = false; holdSend = insideSend = aborted = false; heldReplyIndex = SIZE_MAX;
    assert(setHubOrigin(ORIGIN));
    assert(configureHubSavedLogin(""));
}
static std::string sessionJson(const char* role = "premium", int seconds = 3600, bool token = false) {
    std::string text = "{\"server_time\":" + std::to_string(now()) + ",\"expires_at\":" + std::to_string(now() + seconds) + ",\"user\":{\"id\":\"a-user-id\",\"username\":\"peppy_test\",\"role\":\"" + role + "\",\"expires_at\":" + std::to_string(now() + seconds) + "}";
    if (token) text += ",\"token\":\"" + TOKEN + "\"";
    return text + "}";
}
static void login(const char* role = "premium") {
    replies.push_back(Reply(std::string(ORIGIN) + "/api/login", sessionJson(role, 3600, true), 200, 1));
    assert(startHubLogin("peppy_test", "some_test_password")); HubResult result = finished();
    assert(!result.errorCode && hubSession().authenticated); assert(hubSession().premium); assert(!strcmp(g_token, TOKEN.c_str()));
    freeHubResult(&result);
}
static void sessionReply(const char* role = "premium") {
    Reply r(std::string(ORIGIN) + "/api/session", sessionJson(role)); r.bearer = "Bearer " + TOKEN; replies.push_back(r);
}
static std::string entryJson(const char* url = PKG, const char* cid = "IV0000-SKID02048_00-GAME204800000000") {
    return std::string("{\"name\":\"2048\",\"version\":\"v1.0\",\"content_id\":\"") + cid + "\",\"url\":\"" + url +
        "\",\"size\":35454976,\"content_type\":26,\"content_flags\":167772160,\"kind\":\"homebrew\",\"platform\":\"ps4\",\"description\":\"Free puzzle\",\"requires_data\":\"\",\"sha256\":null,\"adult\":false}";
}
static void catalogReply(const std::string& entries) {
    Reply r(std::string(ORIGIN) + "/api/catalog", "{\"version\":7,\"updated_at\":123,\"entries\":[" + entries + "]}");
    r.bearer = "Bearer " + TOKEN; replies.push_back(r);
}
static Reply rangeReply(const std::string& url, const std::string& body, uint64_t start, uint64_t total) {
    Reply r(url, body, 206); uint64_t end = start + body.size() - 1;
    r.range = "bytes=" + std::to_string(start) + "-" + std::to_string(end);
    r.headers = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(total) +
        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    return r;
}

static void pkgZoneChecks() {
    const char* urls[] = {
        "https://pkg-zone.com/download/ps4/CUSA01116/latest",
        "https://pkg-zone.com/download/ps4/CUSA00127/latest",
        "https://pkg-zone.com/download/ps4/CUSA02644/latest"
    };
    unsigned char output[1080]; UserCatalogRangeInfo info = {};
    for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); ++i) {
        reset(); assert(hubNativePackageUrl(urls[i])); login();
        replies.push_back(rangeReply(urls[i], std::string(sizeof(output), 'x'), 0, 35454976));
        assert(hubUserCatalogRangeReader(0, urls[i], 0, sizeof(output), output, &info));
        assert(addedBearer.empty() && !strcmp(info.effectiveUrl, urls[i]) && info.totalBytes == 35454976 &&
            info.received == sizeof(output) && hubSession().premium); cleanHandles();
    }
    const char* unsupported[] = {
        "http://pkg-zone.com/download/ps4/CUSA01116/latest",
        "https://pkg-zone.com.evil.example/download/ps4/CUSA01116/latest",
        "https://www.pkg-zone.com/download/ps4/CUSA01116/latest",
        "https://pkg-zone.com:443/download/ps4/CUSA01116/latest",
        "https://pkg-zone.com/download/ps5/CUSA01116/latest",
        "https://pkg-zone.com/download/ps4/CUSA01116/latest?auth=1",
        "https://pkg-zone.com/download/ps4/CUSA01116/latest#file.pkg",
        "https://pkg-zone.com/download/ps4/CUSA01116/latest/file.pkg",
        "https://pkg-zone.com/download/ps4/CUSA01116%2flatest",
        "https://pkg-zone.com/download/ps4/CUSA0111/latest",
        "https://pkg-zone.com/details/CUSA01116",
        "https://pkg-zone.com/download/ps4/CUSA01116/game.pkg"
    };
    reset();
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
        assert(!hubNativePackageUrl(unsupported[i]));
        assert(!hubUserCatalogRangeReader(0, unsupported[i], 0, sizeof(output), output, &info));
    }
    assert(!opened && !reads); cleanHandles();
    reset(); Reply html(urls[0], "<html>login or unsupported</html>"); html.range = "bytes=0-1079"; replies.push_back(html);
    assert(!hubUserCatalogRangeReader(0, urls[0], 0, sizeof(output), output, &info) && !reads); cleanHandles();
    reset(); Reply compressed = rangeReply(urls[0], std::string(sizeof(output), 'x'), 0, 35454976);
    compressed.headers.insert(compressed.headers.size() - 2, "Content-Encoding: gzip\r\n"); replies.push_back(compressed);
    assert(!hubUserCatalogRangeReader(0, urls[0], 0, sizeof(output), output, &info) && !reads); cleanHandles();
    reset(); Reply wrongRange = rangeReply(urls[0], std::string(sizeof(output), 'x'), 1, 35454976);
    wrongRange.range = "bytes=0-1079"; replies.push_back(wrongRange);
    assert(!hubUserCatalogRangeReader(0, urls[0], 0, sizeof(output), output, &info) && !reads); cleanHandles();
    const char* badTargets[] = {urls[1], PKG, "https://pkg-zone.com/download/ps4/CUSA01116/latest?token=a"};
    for (size_t i = 0; i < sizeof(badTargets) / sizeof(badTargets[0]); ++i) {
        reset(); Reply redirect(urls[0], "", 302); redirect.range = "bytes=0-1079";
        redirect.headers = std::string("HTTP/1.1 302 Found\r\nLocation: ") + badTargets[i] + "\r\n\r\n";
        replies.push_back(redirect);
        assert(!hubUserCatalogRangeReader(0, urls[0], 0, sizeof(output), output, &info) && opened == 1 && !reads); cleanHandles();
    }
    // A 206-framed HTML body is still rejected by the real PKG header importer.
    reset(); replies.push_back(rangeReply(urls[0], std::string(1080, '<'), 0, 35454976));
    assert(startHubImportUrls(urls[0], strlen(urls[0]))); HubResult r = finished();
    assert(r.errorCode && r.imports.rejected == 1 && !r.catalog->count()); freeHubResult(&r);
}

static void savedLoginChecks() {
    char folder[] = "/tmp/peppy-login-test-XXXXXX"; assert(mkdtemp(folder));
    // Existing builds created the outer FTP/app directory as 0777. Credentials
    // use a dedicated private child, so those upgrades still save successfully.
    assert(!chmod(folder, 0777)); const std::string privateFolder = std::string(folder) + "/private";
    assert(!mkdir(privateFolder.c_str(), 0700));
    const std::string path = privateFolder + "/login.dat";
    reset(); assert(configureHubSavedLogin(path.c_str()));
    assert(hubSavedLoginStatus().configured && !hubSavedLoginStatus().hasUsername);
    assert(!startHubSavedLogin() && !hubSession().authenticated);
    login();
    HubSavedLoginStatus status = hubSavedLoginStatus();
    assert(status.hasUsername && status.hasPassword && status.hasToken && !status.storageError && !status.restoring);
    struct stat st; assert(!stat(path.c_str(), &st) && (st.st_mode & 0777) == 0600);
    peppyHubSavedLogin::Record record = {};
    assert(peppyHubSavedLogin::load(path.c_str(), record) == 1);
    assert(!strcmp(record.username, "peppy_test") && !strcmp(record.password, "some_test_password") &&
        !strcmp(record.token, TOKEN.c_str()) && !strcmp(record.origin, ORIGIN));
    const uint64_t expiry = record.expiresAt;

    // Restart has no authorization until the actual token's server validation
    // completes. No saved role/flag can expose premium or administrator actions.
    reset(); assert(configureHubSavedLogin(path.c_str()));
    char username[65], password[129];
    assert(hubSavedLoginCredentials(username, sizeof(username), password, sizeof(password)));
    assert(!strcmp(username, "peppy_test") && !strcmp(password, "some_test_password") && !hubSession().authenticated);
    sessionReply(); holdSend = true; heldReplyIndex = 0;
    assert(startHubSavedLogin());
    for (int i = 0; i < 10000 && !insideSend; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(insideSend && hubSavedLoginStatus().restoring && !hubSession().authenticated && sentBody.empty());
    holdSend = false; HubResult r = finished();
    assert(!r.errorCode && r.operation == HUB_LOGIN && hubSession().premium && !hubSession().admin);
    freeHubResult(&r);

    // The same remembered login can become admin/user only by a server reply.
    reset(); assert(configureHubSavedLogin(path.c_str())); sessionReply("admin");
    assert(startHubSavedLogin()); r = finished(); assert(!r.errorCode && hubSession().admin); freeHubResult(&r);
    reset(); assert(configureHubSavedLogin(path.c_str())); sessionReply("user");
    assert(startHubSavedLogin()); r = finished(); assert(!r.errorCode && hubSession().authenticated && !hubSession().premium && !hubSession().admin); freeHubResult(&r);

    // A mistyped replacement login cannot corrupt the previous successful one.
    replies.push_back(Reply(std::string(ORIGIN) + "/api/login", "{}", 401, 1));
    assert(startHubLogin("other_user", "wrong_password")); r = finished();
    assert(r.errorCode == HUB_ERROR_AUTH && !hubSession().authenticated); freeHubResult(&r);
    assert(peppyHubSavedLogin::load(path.c_str(), record) == 1 && !strcmp(record.username, "peppy_test") &&
        !strcmp(record.password, "some_test_password") && !strcmp(record.token, TOKEN.c_str()));

    // A non-expired token denied at startup never retries a saved password.
    reset(); assert(configureHubSavedLogin(path.c_str()));
    Reply denied(std::string(ORIGIN) + "/api/session", "{}", 401); denied.bearer = "Bearer " + TOKEN; replies.push_back(denied);
    assert(startHubSavedLogin()); r = finished(); assert(r.errorCode == HUB_ERROR_AUTH && opened == 1); freeHubResult(&r);
    assert(peppyHubSavedLogin::load(path.c_str(), record) == 1 && record.username[0] && !record.password[0] && !record.token[0]);
    reset(); assert(configureHubSavedLogin(path.c_str())); assert(!startHubSavedLogin());

    // A proven expired saved token permits exactly one anonymous password login.
    login(); assert(peppyHubSavedLogin::load(path.c_str(), record) == 1);
    record.expiresAt = now() - 1; assert(peppyHubSavedLogin::save(path.c_str(), record));
    reset(); assert(configureHubSavedLogin(path.c_str())); replies.push_back(denied);
    replies.push_back(Reply(std::string(ORIGIN) + "/api/login", sessionJson("premium", 3600, true), 200, 1));
    assert(startHubSavedLogin()); r = finished();
    assert(!r.errorCode && opened == 2 && hubSession().premium && sentBody.find("some_test_password") != std::string::npos); freeHubResult(&r);
    assert(peppyHubSavedLogin::load(path.c_str(), record) == 1 && record.expiresAt >= expiry && record.password[0]);
    record.expiresAt = now() - 1; assert(peppyHubSavedLogin::save(path.c_str(), record));
    reset(); assert(configureHubSavedLogin(path.c_str())); replies.push_back(denied);
    replies.push_back(Reply(std::string(ORIGIN) + "/api/login", "{}", 401, 1));
    assert(startHubSavedLogin()); r = finished(); assert(r.errorCode == HUB_ERROR_AUTH && opened == 2 && !hubSession().authenticated); freeHubResult(&r);
    assert(!hubSavedLoginStatus().hasPassword && !hubSavedLoginStatus().hasToken);

    // A heartbeat denial clears remembered authorization and cannot relogin.
    reset(); assert(configureHubSavedLogin(path.c_str())); login(); replies.push_back(denied);
    clockAdvanceUs = 6000000; pollHubSession(); sessionChecked();
    assert(!hubSession().authenticated && !hubSavedLoginStatus().hasPassword && !hubSavedLoginStatus().hasToken && opened == 2);
    pollHubSession(); assert(opened == 2);

    // Offline verification remains fail-closed while preserving the saved login
    // for a later restart/retry. A temporary network outage is not revocation.
    reset(); assert(configureHubSavedLogin(path.c_str())); login(); sessionReply(); replies.back().sendError = -72;
    clockAdvanceUs = 6000000; pollHubSession(); sessionChecked();
    clockAdvanceUs = 31000000; assert(!hubSession().authenticated);
    assert(hubSavedLoginStatus().hasPassword && hubSavedLoginStatus().hasToken);
    assert(startHubLogout()); r = finished(); assert(!r.errorCode && opened == 2); freeHubResult(&r);
    assert(!hubSavedLoginStatus().hasPassword && !hubSavedLoginStatus().hasToken);
    // Prepare another remembered login for the restart/admin and online-logout
    // checks after the unauthenticated local logout.
    reset(); assert(configureHubSavedLogin(path.c_str())); login();
    reset(); assert(configureHubSavedLogin(path.c_str())); sessionReply("admin");
    assert(startHubSavedLogin()); r = finished(); assert(!r.errorCode && hubSession().admin); freeHubResult(&r);

    // Even an offline logout forgets the local password/token before its request.
    Reply logout(std::string(ORIGIN) + "/api/logout", "{}", 200, 1); logout.bearer = "Bearer " + TOKEN; logout.sendError = -72; replies.push_back(logout);
    assert(startHubLogout()); r = finished(); assert(r.errorCode == HUB_ERROR_NETWORK && !hubSession().authenticated); freeHubResult(&r);
    assert(peppyHubSavedLogin::load(path.c_str(), record) == 1 && record.username[0] && !record.password[0] && !record.token[0]);

    // Cancelled restore cannot authorize or overwrite the previous good login.
    reset(); assert(configureHubSavedLogin(path.c_str())); login();
    reset(); assert(configureHubSavedLogin(path.c_str())); sessionReply("admin"); holdSend = true;
    assert(startHubSavedLogin());
    for (int i = 0; i < 10000 && !insideSend; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(insideSend); cancelHubOperation(); r = finished();
    assert(r.errorCode == HUB_ERROR_CANCELLED && !hubSession().authenticated && hubSavedLoginStatus().hasPassword); freeHubResult(&r);

    // A delayed success/401 for an old token must not erase the newer saved
    // password/token or rewrite its role. Both requests run on real host threads.
    for (int staleStatus = 200; staleStatus <= 401; staleStatus += 201) {
        reset(); assert(configureHubSavedLogin(path.c_str())); login();
        Reply stale(std::string(ORIGIN) + "/api/session", sessionJson(), staleStatus);
        stale.bearer = "Bearer " + TOKEN; replies.push_back(stale);
        std::string newLogin = sessionJson("admin", 3600, true);
        size_t tokenAt = newLogin.find(TOKEN); assert(tokenAt != std::string::npos);
        const std::string newToken(64, 'b'); newLogin.replace(tokenAt, TOKEN.size(), newToken);
        replies.push_back(Reply(std::string(ORIGIN) + "/api/login", newLogin, 200, 1));
        holdSend = true; heldReplyIndex = 1; insideSend = false; clockAdvanceUs = 6000000; pollHubSession();
        for (int i = 0; i < 10000 && !insideSend; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        assert(insideSend && startHubLogin("peppy_test", "new_saved_password"));
        // finished() normally checks all HTTP handles, including this intentionally
        // held heartbeat; wait for just the main result before releasing it.
        bool ready = false;
        for (int i = 0; i < 10000 && !ready; ++i) { ready = consumeHubResult(&r); if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        assert(ready && !r.errorCode && hubSession().admin); freeHubResult(&r);
        holdSend = false; sessionChecked();
        assert(hubSession().admin && !strcmp(g_token, newToken.c_str()));
        assert(peppyHubSavedLogin::load(path.c_str(), record) == 1 && !strcmp(record.token, newToken.c_str()) &&
            !strcmp(record.password, "new_saved_password"));
    }

    // Credentials are origin-bound; a changed origin cannot reuse the record.
    reset(); assert(setHubOrigin("https://other.example.org")); assert(configureHubSavedLogin(path.c_str()));
    assert(!hubSavedLoginStatus().hasUsername && !startHubSavedLogin() && !hubSession().authenticated);

    // Reject files that disclose credentials, symlinks, hardlinks and malformed
    // records rather than granting even a temporary offline session.
    assert(!chmod(path.c_str(), 0644)); reset(); assert(!configureHubSavedLogin(path.c_str()));
    assert(hubSavedLoginStatus().storageError && !startHubSavedLogin()); assert(!chmod(path.c_str(), 0600));
    const std::string linkPath = privateFolder + "/symlink.dat", hardPath = privateFolder + "/hard.dat";
    assert(!symlink(path.c_str(), linkPath.c_str())); assert(!configureHubSavedLogin(linkPath.c_str()));
    assert(!peppyHubSavedLogin::save(linkPath.c_str(), record));
    assert(peppyHubSavedLogin::forget(linkPath.c_str()) && !stat(path.c_str(), &st));
    assert(!link(path.c_str(), hardPath.c_str())); assert(!configureHubSavedLogin(path.c_str())); assert(!unlink(hardPath.c_str()));
    int fd = open(path.c_str(), O_WRONLY); assert(fd >= 0); const char broken = '!'; assert(write(fd, &broken, 1) == 1); assert(!close(fd));
    assert(!configureHubSavedLogin(path.c_str()) && !startHubSavedLogin());
    assert(!chmod(privateFolder.c_str(), 0755)); assert(!peppyHubSavedLogin::save(path.c_str(), record)); assert(!chmod(privateFolder.c_str(), 0700));
    assert(!peppyHubSavedLogin::safePath("/tmp/../login.dat") && !peppyHubSavedLogin::safePath("/tmp/login.dat/"));
    reset(); assert(!unlink(path.c_str()) && !rmdir(privateFolder.c_str()) && !rmdir(folder));
    peppyHubSavedLogin::wipe(&record, sizeof(record)); wipe(password, sizeof(password));
}

int main() {
    reset();
    const char* badOrigins[] = {"http://peppy.example.org", "https://localhost", "https://127.0.0.1", "https://10.0.0.1", "https://user:pass@peppy.example.org", "https://peppy.example.org/api", "https://peppy.example.org?query=x", "https://peppy.example.org:444", "https://peppy.example.org/#x"};
    for (size_t i = 0; i < sizeof(badOrigins) / sizeof(badOrigins[0]); ++i) assert(!setHubOrigin(badOrigins[i]));
    assert(setHubOrigin("https://peppy.example.org/")); char origin[128]; assert(hubOrigin(origin, sizeof(origin)) && !strcmp(origin, ORIGIN)); assert(hubConfigured());
    assert(setHubOrigin("")); assert(!hubConfigured()); assert(!startHubLogin("peppy_test", "password")); HubResult r = finished(); assert(r.errorCode == HUB_ERROR_CONFIG); freeHubResult(&r);

    reset(); login();
    sessionReply(); catalogReply(entryJson()); assert(startHubSync()); r = finished();
    assert(!r.errorCode && r.catalogVersion == 7 && r.catalog && r.catalog->count() == 1);
    assert(!strcmp(r.catalog->at(0)->version, "v1.0") && r.catalog->at(0)->kind == USER_PACKAGE_BASE); assert(userCatalogEntryValid(*r.catalog->at(0))); freeHubResult(&r);
    assert(!strcmp(g_token, TOKEN.c_str()));
    Reply logout(std::string(ORIGIN) + "/api/logout", "{\"ok\":true}", 200, 1); logout.bearer = "Bearer " + TOKEN; replies.push_back(logout);
    assert(startHubLogout()); assert(!hubSession().authenticated && !g_token[0]); r = finished(); assert(!r.errorCode); freeHubResult(&r);

    reset(); replies.push_back(Reply(std::string(ORIGIN) + "/api/login", "{\"error\":\"denied\"}", 401, 1));
    assert(startHubLogin("peppy_test", "bad_password")); r = finished(); assert(r.errorCode == HUB_ERROR_AUTH && !hubSession().authenticated && !g_token[0]); freeHubResult(&r);
    reset(); replies.push_back(Reply(std::string(ORIGIN) + "/api/login", sessionJson("premium", -60, true), 200, 1));
    assert(startHubLogin("peppy_test", "password")); r = finished(); assert(r.errorCode == HUB_ERROR_EXPIRED && !hubSession().authenticated); freeHubResult(&r);
    reset(); login(); lock(); g_sessionDeadline = peppyHubUptime() - 1; unlock(); assert(!hubSession().authenticated && !g_token[0]); assert(!startHubSync()); r = finished(); assert(r.errorCode == HUB_ERROR_AUTH); freeHubResult(&r);
    reset(); login(); lock(); g_premiumDeadline = peppyHubUptime() - 1; unlock(); assert(hubSession().authenticated && !hubSession().premium);
    // A valid server TTL survives an incorrect console calendar; after login,
    // entitlement expiry follows a monotonic deadline, never a changed date.
    reset(); std::string serverClock = "{\"server_time\":1000,\"expires_at\":4600,\"token\":\"" + TOKEN +
        "\",\"user\":{\"id\":\"test\",\"username\":\"peppy_test\",\"role\":\"premium\",\"expires_at\":4600}}";
    replies.push_back(Reply(std::string(ORIGIN) + "/api/login", serverClock, 200, 1));
    assert(startHubLogin("peppy_test", "password")); r = finished(); assert(!r.errorCode && hubSession().premium && hubSession().expiresAt < now()); freeHubResult(&r);

    for (int mode = 1; mode <= 3; ++mode) {
        reset(); threadFailure = mode; assert(!startHubLogin("peppy_test", "password")); r = finished(); assert(r.errorCode == HUB_ERROR_THREAD && !g_token[0]); freeHubResult(&r);
    }
    reset(); privateDns = true; assert(startHubLogin("peppy_test", "password")); r = finished(); assert(r.errorCode == HUB_ERROR_SOURCE && !opened); freeHubResult(&r);
    reset(); badDns = true; assert(startHubLogin("peppy_test", "password")); r = finished(); assert(r.errorCode == HUB_ERROR_NETWORK && !opened); freeHubResult(&r);

    // Login redirects are never followed and cannot forward credentials.
    reset(); Reply redirectLogin(std::string(ORIGIN) + "/api/login", "", 302, 1);
    redirectLogin.headers = "HTTP/1.1 302 Found\r\nLocation: https://other.example.org/api/login\r\nContent-Length: 0\r\n\r\n"; replies.push_back(redirectLogin);
    assert(startHubLogin("peppy_test", "password")); r = finished(); assert(r.errorCode == HUB_ERROR_HTTP && opened == 1); freeHubResult(&r);

    const std::string invalidCatalogs[] = {
        entryJson("https://evil.example.org/game.pkg"), entryJson("https://127.0.0.1/game.pkg"),
        entryJson(PKG, "IV0000-PPSA00001_00-GAME204800000000"), entryJson(PKG, "IV0000-BREW00001_00-GAME204800000000"),
        entryJson() + "," + entryJson(), "{\"name\":\"missing\"}", "{\"name\":\"broken\""
    };
    for (size_t i = 0; i < sizeof(invalidCatalogs) / sizeof(invalidCatalogs[0]); ++i) {
        reset(); login(); sessionReply(); catalogReply(invalidCatalogs[i]); assert(startHubSync()); r = finished(); assert(r.errorCode == HUB_ERROR_JSON && !r.catalog); freeHubResult(&r);
    }
    reset(); login(); sessionReply(); catalogReply(""); assert(startHubSync()); r = finished(); assert(!r.errorCode && r.catalog && !r.catalog->count()); freeHubResult(&r);

    reset(); login("admin"); Reply create(std::string(ORIGIN) + "/api/admin/users", "{\"user\":{\"id\":\"created\"}}", 200, 1); create.bearer = "Bearer " + TOKEN; replies.push_back(create);
    assert(startHubAdminCreateUser("new_user", "new_password", 15)); r = finished(); assert(!r.errorCode && sentBody.find("\"plan\":\"15d\"") != std::string::npos && sentBody.find("new_password") != std::string::npos); freeHubResult(&r);
    Reply publish(std::string(ORIGIN) + "/api/admin/catalog", "{\"version\":8,\"count\":1}", 200, 1); publish.bearer = "Bearer " + TOKEN; replies.push_back(publish);
    std::string publication = "{\"entries\":[" + entryJson() + "],\"replace\":false}";
    assert(startHubAdminPublish(publication.c_str(), publication.size())); r = finished(); assert(!r.errorCode && sentBody == publication); freeHubResult(&r);
    reset(); login(); assert(!startHubAdminCreateUser("new_user", "password", 30)); r = finished(); assert(r.errorCode == HUB_ERROR_AUTH); freeHubResult(&r);

    // The real bootstrap owner's non-hex ID is visible in the admin list.
    reset(); login("admin");
    const std::string ownerRow = "{\"id\":\"owner\",\"username\":\"owner_account\",\"role\":\"admin\",\"plan\":null,\"expires_at\":null,\"premium_active\":true,\"revoked\":false}";
    const std::string memberId = "0123456789abcdef0123456789abcdef";
    const std::string memberRow = "{\"id\":\"" + memberId + "\",\"username\":\"member_account\",\"role\":\"premium\",\"plan\":\"15d\",\"expires_at\":2000000000,\"premium_active\":true,\"revoked\":false}";
    Reply users(std::string(ORIGIN) + "/api/admin/users", "{\"users\":[" + ownerRow + "," + memberRow + "]}");
    users.bearer = "Bearer " + TOKEN; replies.push_back(users);
    assert(startHubAdminListUsers()); r = finished();
    assert(!r.errorCode && r.userCount == 2 && r.users[0].admin && !strcmp(r.users[0].id, "owner") &&
        !r.users[1].revoked && !strcmp(r.users[1].id, memberId.c_str())); freeHubResult(&r);
    assert(!startHubAdminRevokeUser("owner") && !startHubAdminRevokeUser("../user") && !startHubAdminRevokeUser("short"));
    Reply revoke(std::string(ORIGIN) + "/api/admin/users/" + memberId + "/revocation", "{\"user\":{\"revoked\":true}}", 200, 1);
    revoke.bearer = "Bearer " + TOKEN; replies.push_back(revoke);
    assert(startHubAdminRevokeUser(memberId.c_str())); r = finished();
    assert(!r.errorCode && sentBody == "{\"revoked\":true}"); freeHubResult(&r);
    Reply reactivate(std::string(ORIGIN) + "/api/admin/users/" + memberId + "/revocation", "{\"user\":{\"revoked\":false}}", 200, 1);
    reactivate.bearer = "Bearer " + TOKEN; replies.push_back(reactivate);
    assert(startHubAdminRevokeUser(memberId.c_str(), false)); r = finished();
    assert(!r.errorCode && sentBody == "{\"revoked\":false}"); freeHubResult(&r);
    assert(!startHubAdminChangePassword("owner", "new_password") && !startHubAdminChangePassword(memberId.c_str(), "short"));
    Reply passwordChange(std::string(ORIGIN) + "/api/admin/users/" + memberId + "/password", "{\"user\":{\"revoked\":false}}", 200, 1);
    passwordChange.bearer = "Bearer " + TOKEN; replies.push_back(passwordChange);
    assert(startHubAdminChangePassword(memberId.c_str(), "new_\"password")); r = finished();
    assert(!r.errorCode && sentBody == "{\"password\":\"new_\\\"password\"}"); freeHubResult(&r);
    const std::string badUsers[] = {ownerRow + "," + ownerRow, "{\"id\":\"owner\",\"username\":\"attacker\",\"role\":\"premium\",\"expires_at\":2000000000,\"premium_active\":true,\"revoked\":false}",
        "{\"id\":\"../target\",\"username\":\"member\",\"role\":\"premium\",\"expires_at\":2000000000,\"premium_active\":true,\"revoked\":false}", "{\"username\":\"missing_id\"}"};
    for (size_t i = 0; i < sizeof(badUsers) / sizeof(badUsers[0]); ++i) {
        Reply badUsersReply(std::string(ORIGIN) + "/api/admin/users", "{\"users\":[" + badUsers[i] + "]}");
        badUsersReply.bearer = "Bearer " + TOKEN; replies.push_back(badUsersReply);
        assert(startHubAdminListUsers()); r = finished(); assert(r.errorCode == HUB_ERROR_JSON && !r.users && !r.userCount); freeHubResult(&r);
    }
    reset(); login(); assert(!startHubAdminListUsers()); r = finished(); assert(r.errorCode == HUB_ERROR_AUTH); freeHubResult(&r);
    assert(!startHubAdminRevokeUser(memberId.c_str())); r = finished(); assert(r.errorCode == HUB_ERROR_AUTH); freeHubResult(&r);
    assert(!startHubAdminChangePassword(memberId.c_str(), "new_password")); r = finished(); assert(r.errorCode == HUB_ERROR_AUTH); freeHubResult(&r);

    // Heartbeats fetch only the small session endpoint. They are independent of
    // imports/downloads and cannot alter a pending primary result or its abort handle.
    reset(); login(); sessionReply(); size_t beforeCheck = opened;
    lock(); g_nextSessionCheck = peppyHubUptime() + SESSION_CHECK_US; unlock();
    clockAdvanceUs = 4000000; pollHubSession(); assert(!g_sessionCheckBusy && opened == beforeCheck);
    clockAdvanceUs = 6000000; pollHubSession(); sessionChecked();
    assert(opened == beforeCheck + 1 && hubSession().premium && hubSnapshot().state == HUB_IDLE);
    pollHubSession(); assert(!g_sessionCheckBusy && opened == beforeCheck + 1);
    Reply invalidSession(std::string(ORIGIN) + "/api/session", "{\"error\":\"revoked\"}", 401); invalidSession.bearer = "Bearer " + TOKEN; replies.push_back(invalidSession);
    lock(); g_snapshot.state = HUB_RUNNING; g_snapshot.operation = HUB_IMPORT_URLS; g_snapshot.httpStatus = 206;
    g_request = 77; g_busy = 1; g_cancel = 1; unlock();
    clockAdvanceUs = 12000000; pollHubSession(); sessionChecked();
    assert(!hubSession().authenticated && !g_token[0] && g_request == 77 && g_busy == 1 &&
        hubSnapshot().state == HUB_RUNNING && hubSnapshot().operation == HUB_IMPORT_URLS &&
        hubSnapshot().httpStatus == 206 && !hubSnapshot().errorCode);
    lock(); g_busy = 0; g_request = -1; g_snapshot = {}; unlock();

    // Exercise two actual worker requests concurrently: an import owns a held
    // PKG request while a small heartbeat receives revocation. Its completion
    // cannot replace the import's abort handle, state or later cancellation.
    reset(); login();
    replies.push_back(rangeReply(PKG, std::string(1080, '<'), 0, 35454976));
    Reply revokedWhileImport(std::string(ORIGIN) + "/api/session", "{\"error\":\"revoked\"}", 401);
    revokedWhileImport.bearer = "Bearer " + TOKEN; replies.push_back(revokedWhileImport);
    heldReplyIndex = opened.load(); holdSend = true; insideSend = false;
    assert(startHubImportUrls(PKG, strlen(PKG)));
    while (!insideSend) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    int importRequest = g_request; assert(importRequest >= 0 && hubSnapshot().state == HUB_RUNNING);
    clockAdvanceUs = 6000000; pollHubSession();
    for (int i = 0; i < 10000 && g_sessionCheckBusy; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(!g_sessionCheckBusy && !hubSession().authenticated && g_request == importRequest &&
        hubSnapshot().state == HUB_RUNNING && hubSnapshot().operation == HUB_IMPORT_URLS && !hubSnapshot().errorCode);
    cancelHubOperation(); r = finished(); assert(r.errorCode == HUB_ERROR_CANCELLED); freeHubResult(&r);

    // A network failure never extends the last verified entitlement. Once the
    // 30-second grace expires, offline copies of a revoked session fail closed.
    reset(); login(); Reply lostSession(std::string(ORIGIN) + "/api/session", "{}");
    lostSession.bearer = "Bearer " + TOKEN; lostSession.sendError = -22; replies.push_back(lostSession);
    clockAdvanceUs = 6000000; pollHubSession(); sessionChecked(); assert(hubSession().premium && hubSnapshot().state == HUB_IDLE);
    clockAdvanceUs = 31000000; assert(!hubSession().authenticated && !g_token[0]);

    // Both a successful old heartbeat and an old 401 are unable to overwrite or
    // clear a newer login, even when the user logs out while HTTP is in flight.
    for (int staleStatus = 200; staleStatus <= 401; staleStatus += 201) {
        reset(); login(); Reply stale(std::string(ORIGIN) + "/api/session", sessionJson(), staleStatus);
        stale.bearer = "Bearer " + TOKEN; replies.push_back(stale); holdSend = true;
        clockAdvanceUs = 6000000; pollHubSession();
        while (!insideSend) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock(); clearSessionLocked(); unlock();
        std::string newLogin = sessionJson("admin", 3600, true);
        size_t tokenAt = newLogin.find(TOKEN); assert(tokenAt != std::string::npos); newLogin.replace(tokenAt, TOKEN.size(), std::string(64, 'b'));
        assert(!parseSession(newLogin.c_str(), newLogin.size(), true)); holdSend = false; sessionChecked();
        assert(hubSession().authenticated && hubSession().admin && !strcmp(g_token, std::string(64, 'b').c_str()));
    }

    reset(); unsigned char output[1080]; UserCatalogRangeInfo info = {};
    replies.push_back(rangeReply(PKG, std::string(sizeof(output), 'x'), 0, 35454976));
    assert(hubUserCatalogRangeReader(0, PKG, 0, sizeof(output), output, &info)); assert(info.received == sizeof(output) && info.totalBytes == 35454976); cleanHandles();
    assert(!addedBearer.size() && addedRange == "bytes=0-1079");
    reset(); login(); replies.push_back(rangeReply(PKG, std::string(sizeof(output), 'x'), 0, 35454976));
    assert(hubUserCatalogRangeReader(0, PKG, 0, sizeof(output), output, &info) && addedBearer.empty()); cleanHandles();
    reset(); replies.push_back(Reply(PKG, std::string(sizeof(output), 'x'))); replies.back().range = "bytes=0-1079";
    assert(!hubUserCatalogRangeReader(0, PKG, 0, sizeof(output), output, &info) && !reads); cleanHandles();
    reset(); Reply badRange = rangeReply(PKG, std::string(sizeof(output), 'x'), 1, 35454976); badRange.range = "bytes=0-1079"; replies.push_back(badRange);
    assert(!hubUserCatalogRangeReader(0, PKG, 0, sizeof(output), output, &info) && !reads); cleanHandles();
    reset(); Reply compressed = rangeReply(PKG, std::string(sizeof(output), 'x'), 0, 35454976); compressed.headers.insert(compressed.headers.size() - 2, "Content-Encoding: gzip\r\n"); replies.push_back(compressed);
    assert(!hubUserCatalogRangeReader(0, PKG, 0, sizeof(output), output, &info) && !reads); cleanHandles();
    reset(); Reply badRedirect(PKG, "", 302); badRedirect.range = "bytes=0-1079"; badRedirect.headers = "HTTP/1.1 302 Found\r\nLocation: https://10.0.0.1/game.pkg\r\n\r\n"; replies.push_back(badRedirect);
    assert(!hubUserCatalogRangeReader(0, PKG, 0, sizeof(output), output, &info) && opened == 1); cleanHandles();
    reset(); const char* CDN = "https://release-assets.githubusercontent.com/file.pkg?token=ephemeral";
    Reply goodRedirect(PKG, "", 302); goodRedirect.range = "bytes=0-1079"; goodRedirect.headers = std::string("HTTP/1.1 302 Found\r\nLocation: ") + CDN + "\r\n\r\n"; replies.push_back(goodRedirect); replies.push_back(rangeReply(CDN, std::string(sizeof(output), 'x'), 0, 35454976));
    assert(hubUserCatalogRangeReader(0, PKG, 0, sizeof(output), output, &info) && opened == 2 && !strcmp(info.effectiveUrl, CDN)); cleanHandles();
    reset(); const char* ARCHIVE = "https://archive.org/download/ps4-fpkg-collection-english-a/Example.pkg";
    assert(hubNativePackageUrl(ARCHIVE));
    assert(!hubNativePackageUrl("https://archive.org/download/unreviewed-collection/Example.pkg"));
    assert(!hubNativePackageUrl("https://archive.org/download/ps4-fpkg-collection-english-a/%2FExample.pkg"));
    assert(!hubNativePackageUrl("https://archive.org/download/ps4-fpkg-collection-english-a/Example.pkg?auth=1"));
    Reply crossProvider(PKG, "", 302); crossProvider.range = "bytes=0-1079"; crossProvider.headers = std::string("HTTP/1.1 302 Found\r\nLocation: ") + ARCHIVE + "\r\n\r\n"; replies.push_back(crossProvider);
    assert(!hubUserCatalogRangeReader(0, PKG, 0, sizeof(output), output, &info) && opened == 1); cleanHandles();

    const char* MEDIAFIRE_PAGE = "https://www.mediafire.com/file/ABC123/sample.pkg/file";
    const char* MEDIAFIRE_CDN = "https://download2392.mediafire.com/token/sample.pkg?key=a&part=b";
    const std::string mediafireHtml = "<!doctype html><html><body><a id='downloadButton' href='https://download2392.mediafire.com/token/sample.pkg?key=a&amp;part=b'>Download</a></body></html>";
    reset(); assert(hubNativePackageUrl(MEDIAFIRE_PAGE));
    const char* unsupportedPages[] = {"http://www.mediafire.com/file/ABC123/sample.pkg/file", "https://www.mediafire.com/file/ABC123/sample.rar/file",
        "https://www.mediafire.com/file/ABC123/sample.pkg", "https://www.mediafire.com/file/ABC123/sample.pkg/file?auth=1",
        "https://www.mediafire.com/file/ABC123/sample.pkg/file/extra", "https://www.mediafire.com:443/file/ABC123/sample.pkg/file",
        "https://www.mediafire.com.evil.example/file/ABC123/sample.pkg/file", "https://www.mediafire.com/file/ABC123/%2fsample.pkg/file"};
    for (size_t i = 0; i < sizeof(unsupportedPages) / sizeof(unsupportedPages[0]); ++i) assert(!hubNativePackageUrl(unsupportedPages[i]));
    // Even when logged in, both the HTML GET and PKG Range are anonymous.
    reset(); login(); Reply landing(MEDIAFIRE_PAGE, mediafireHtml); landing.fragment = 3;
    landing.failureAfter = mediafireHtml.size(); landing.readError = -22; replies.push_back(landing);
    replies.push_back(rangeReply(MEDIAFIRE_CDN, std::string(sizeof(output), 'x'), 0, 35454976));
    assert(hubUserCatalogRangeReader(0, MEDIAFIRE_PAGE, 0, sizeof(output), output, &info));
    assert(info.received == sizeof(output) && !strcmp(info.effectiveUrl, MEDIAFIRE_CDN) && hubSession().premium); cleanHandles();
    // A single import reuses its resolved CDN for subsequent metadata ranges,
    // while the persisted entry keeps the original stable page URL.
    reset(); replies.push_back(Reply(MEDIAFIRE_PAGE, mediafireHtml));
    replies.push_back(rangeReply(MEDIAFIRE_CDN, std::string(sizeof(output), 'x'), 0, 35454976));
    replies.push_back(rangeReply(MEDIAFIRE_CDN, std::string(32, 'y'), 1080, 35454976));
    { RangeContext shared;
        assert(hubUserCatalogRangeReader(&shared, MEDIAFIRE_PAGE, 0, sizeof(output), output, &info));
        assert(hubUserCatalogRangeReader(&shared, MEDIAFIRE_PAGE, 1080, 32, output, &info));
        assert(opened == 3 && info.totalBytes == 35454976 && output[0] == 'y');
    } cleanHandles();
    // Unknown/chunked length requires EOF, including exactly at the HTML cap.
    for (int exactCap = 0; exactCap < 2; ++exactCap) {
        reset(); std::string html = mediafireHtml; if (exactCap) html.resize(peppyMediafire::HTML_CAP, ' ');
        Reply unknown(MEDIAFIRE_PAGE, html); unknown.lengthType = 1; unknown.contentLength = 0; unknown.eofAllowed = true; unknown.fragment = 16384;
        unknown.headers = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/html\r\n\r\n"; replies.push_back(unknown);
        replies.push_back(rangeReply(MEDIAFIRE_CDN, std::string(sizeof(output), 'x'), 0, 35454976));
        assert(hubUserCatalogRangeReader(0, MEDIAFIRE_PAGE, 0, sizeof(output), output, &info)); cleanHandles();
    }
    reset(); Reply hugeLanding(MEDIAFIRE_PAGE, mediafireHtml); hugeLanding.contentLength = peppyMediafire::HTML_CAP + 1;
    hugeLanding.headers = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(hugeLanding.contentLength) + "\r\n\r\n";
    replies.push_back(hugeLanding); assert(!hubUserCatalogRangeReader(0, MEDIAFIRE_PAGE, 0, sizeof(output), output, &info) && !reads); cleanHandles();
    reset(); std::string overflowHtml = mediafireHtml; overflowHtml.resize(peppyMediafire::HTML_CAP + 1, ' ');
    Reply overflowLanding(MEDIAFIRE_PAGE, overflowHtml); overflowLanding.lengthType = 1; overflowLanding.contentLength = 0; overflowLanding.fragment = 16384;
    overflowLanding.headers = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"; replies.push_back(overflowLanding);
    assert(!hubUserCatalogRangeReader(0, MEDIAFIRE_PAGE, 0, sizeof(output), output, &info)); assert(replies[0].cursor == peppyMediafire::HTML_CAP + 1); cleanHandles();
    reset(); Reply earlyEof(MEDIAFIRE_PAGE, mediafireHtml); earlyEof.contentLength = mediafireHtml.size() + 1; earlyEof.eofAllowed = true;
    earlyEof.headers = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(earlyEof.contentLength) + "\r\n\r\n"; replies.push_back(earlyEof);
    assert(!hubUserCatalogRangeReader(0, MEDIAFIRE_PAGE, 0, sizeof(output), output, &info)); cleanHandles();
    reset(); Reply tooMuch(MEDIAFIRE_PAGE, mediafireHtml); tooMuch.oversizedRead = true; replies.push_back(tooMuch);
    assert(!hubUserCatalogRangeReader(0, MEDIAFIRE_PAGE, 0, sizeof(output), output, &info)); cleanHandles();
    const std::string blockedHtml[] = {"<html>Sign in or complete a captcha</html>",
        "<script>var fake=\"<a id='downloadButton' href='https://download2392.mediafire.com/token/sample.pkg'>Download</a>\";</script>",
        "<a id='downloadButton' href='https://download2392.mediafire.com.evil.example/token/sample.pkg'>Download</a>",
        mediafireHtml + mediafireHtml, mediafireHtml + std::string(1, '\0'), "<a id='downloadButton' href='http://download2392.mediafire.com/token/sample.pkg'>Download</a>"};
    for (size_t i = 0; i < sizeof(blockedHtml) / sizeof(blockedHtml[0]); ++i) {
        reset(); replies.push_back(Reply(MEDIAFIRE_PAGE, blockedHtml[i]));
        assert(!hubUserCatalogRangeReader(0, MEDIAFIRE_PAGE, 0, sizeof(output), output, &info) && opened == 1); cleanHandles();
    }
    reset(); Reply compressedHtml(MEDIAFIRE_PAGE, mediafireHtml);
    compressedHtml.headers.insert(compressedHtml.headers.size() - 2, "Content-Encoding: gzip\r\n"); replies.push_back(compressedHtml);
    assert(!hubUserCatalogRangeReader(0, MEDIAFIRE_PAGE, 0, sizeof(output), output, &info) && !reads); cleanHandles();
    reset(); Reply sourceRedirect(MEDIAFIRE_PAGE, "", 302);
    sourceRedirect.headers = std::string("HTTP/1.1 302 Found\r\nLocation: ") + MEDIAFIRE_CDN + "\r\nContent-Length: 0\r\n\r\n";
    replies.push_back(sourceRedirect); replies.push_back(rangeReply(MEDIAFIRE_CDN, std::string(sizeof(output), 'x'), 0, 35454976));
    assert(hubUserCatalogRangeReader(0, MEDIAFIRE_PAGE, 0, sizeof(output), output, &info)); cleanHandles();
    reset(); Reply returnToPage(MEDIAFIRE_CDN, "", 302); returnToPage.range = "bytes=0-1079";
    returnToPage.headers = std::string("HTTP/1.1 302 Found\r\nLocation: ") + MEDIAFIRE_PAGE + "\r\nContent-Length: 0\r\n\r\n"; replies.push_back(returnToPage);
    assert(!hubUserCatalogRangeReader(0, MEDIAFIRE_CDN, 0, sizeof(output), output, &info) && opened == 1); cleanHandles();
    reset(); Reply escapePage(MEDIAFIRE_PAGE, "", 302); escapePage.headers = std::string("HTTP/1.1 302 Found\r\nLocation: ") + PKG + "\r\nContent-Length: 0\r\n\r\n"; replies.push_back(escapePage);
    assert(!hubUserCatalogRangeReader(0, MEDIAFIRE_PAGE, 0, sizeof(output), output, &info) && opened == 1); cleanHandles();

    reset(); // Actual import rejects HTML/non-PKG bodies, not just bad URLs.
    replies.push_back(rangeReply(PKG, std::string(1080, '<'), 0, 35454976)); assert(startHubImportUrls(PKG, strlen(PKG))); r = finished(); assert(r.errorCode && r.imports.rejected == 1 && !r.catalog->count()); freeHubResult(&r);
    reset(); std::string pkgHeader(1080, '\0'); memcpy(&pkgHeader[0], "\x7f" "CNT", 4); memcpy(&pkgHeader[0x40], "IV0000-SKID02048_00-GAME204800000000", 36); pkgHeader[0x77] = 26; pkgHeader[0x78] = 10;
    uint64_t total = 35454976; for (int i = 0; i < 8; ++i) pkgHeader[0x430 + i] = char(total >> (56 - i * 8));
    replies.push_back(rangeReply(PKG, pkgHeader, 0, total)); assert(startHubImportUrls(PKG, strlen(PKG))); r = finished(); assert(!r.errorCode && r.imports.added == 1 && r.catalog->count() == 1 && !r.catalog->at(0)->titleKnown); freeHubResult(&r);

    reset(); replies.push_back(Reply(MEDIAFIRE_PAGE, mediafireHtml)); replies.push_back(rangeReply(MEDIAFIRE_CDN, pkgHeader, 0, total));
    assert(startHubImportUrls(MEDIAFIRE_PAGE, strlen(MEDIAFIRE_PAGE))); r = finished();
    assert(!r.errorCode && r.imports.added == 1 && !strcmp(r.catalog->at(0)->url, MEDIAFIRE_PAGE)); freeHubResult(&r);

    reset(); replies.push_back(Reply(std::string(ORIGIN) + "/api/login", sessionJson("premium", 3600, true), 200, 1)); holdSend = true;
    assert(startHubLogin("peppy_test", "password"));
    while (!insideSend) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(!startHubLogin("other_user", "password")); assert(!setHubOrigin("https://other.example.org"));
    cancelHubOperation(); r = finished(); assert(r.errorCode == HUB_ERROR_CANCELLED && !hubSession().authenticated && !g_token[0]); freeHubResult(&r);
    reset();

    // Exact-size framing rejects oversized, inconsistent and truncated API data.
    reset(); Reply tooLarge(std::string(ORIGIN) + "/api/login", "{}", 200, 1); tooLarge.contentLength = BODY_CAP + 1; tooLarge.headers = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(BODY_CAP + 1) + "\r\n\r\n"; replies.push_back(tooLarge);
    assert(startHubLogin("peppy_test", "password")); r = finished(); assert(r.errorCode == HUB_ERROR_LIMIT && !reads); freeHubResult(&r);
    reset(); Reply truncated(std::string(ORIGIN) + "/api/login", sessionJson("premium", 3600, true), 200, 1); truncated.failureAfter = 11; truncated.readError = -22; replies.push_back(truncated);
    assert(startHubLogin("peppy_test", "password")); r = finished(); assert(r.errorCode == HUB_ERROR_NETWORK && !hubSession().authenticated); freeHubResult(&r);
    reset();

    const char* malformed[] = {"\"\\u0000\"", "\"\\ud800\"", "\"\\udc00\"", "01", "{\"a\":1,}", "[1,]", "true false", "\"\xc0\xaf\""};
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) { peppyHubJson::Cursor json(malformed[i], strlen(malformed[i])); assert(!json.skip() || !json.done()); }
    char utf[32]; peppyHubJson::Cursor unicode("\"\\ud83d\\ude00\"", 14); assert(unicode.string(utf, sizeof(utf)) && unicode.done() && strlen(utf) == 4);
    pkgZoneChecks(); savedLoginChecks();
    printf("hub client: async session/catalog/admin/import, private saved login/restart/admin/expiry/logout, user invalidation, independent 5-second verification, stale-session race guards, expiry, TLS/range framing, credential isolation, bounded MediaFire landing resolution and cancellation checks passed\n");
    return 0;
}
