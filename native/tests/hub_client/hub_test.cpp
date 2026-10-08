#include "../../hub_client.cpp"
#include <assert.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

struct Reply {
    std::string url, body, headers, bearer, range;
    int status, method, sendError, readError;
    size_t cursor, failureAfter, contentLength;
    Reply(const std::string& u, const std::string& b, int s = 200, int m = 0)
        : url(u), body(b), status(s), method(m), sendError(0), readError(0), cursor(0),
          failureAfter(SIZE_MAX), contentLength(b.size()) {
        headers = "HTTP/1.1 " + std::to_string(s) + " Response\r\nContent-Length: " + std::to_string(b.size()) + "\r\nContent-Type: application/json\r\n\r\n";
    }
};
static std::vector<Reply> replies;
static size_t opened, reads, connections, requestDeletes, connectionDeletes;
static int contexts, pools, sslContexts, templates, resolverCount, threadFailure;
static bool privateDns, badDns, secure, noRedirect;
static std::string connectionUrl, sentBody, addedBearer, addedRange;
static std::atomic<bool> holdSend(false), insideSend(false), aborted(false);
static const char* ORIGIN = "https://peppy.example.org";
static const char* PKG = "https://github.com/skidgfx/PS4-2048/releases/download/v1.0/game.pkg";
static const std::string TOKEN(64, 'a');
static Reply& reply() { assert(opened && opened <= replies.size()); return replies[opened - 1]; }

extern "C" int32_t sceKernelUsleep(uint32_t micros) { std::this_thread::sleep_for(std::chrono::microseconds(micros)); return 0; }
extern "C" uint64_t mockUptime() __asm__("sceKernelGetProcessTime");
extern "C" uint64_t mockUptime() { return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
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
    assert(secure && noRedirect && !keepAlive); assert(opened < replies.size());
    assert(replies[opened].url == url); connectionUrl = url; ++connections; return 5;
}
extern "C" int32_t sceHttpCreateRequestWithURL(int32_t, int32_t method, const char* url, uint64_t bytes) {
    assert(connectionUrl == url && replies[opened].method == method);
    if (method == 0) assert(!bytes);
    addedBearer.clear(); addedRange.clear(); ++opened; return 10 + int(opened);
}
extern "C" int32_t sceHttpAddRequestHeader(int32_t, const char* key, const char* value, int32_t) {
    if (!strcmp(key, "Authorization")) addedBearer = value;
    if (!strcmp(key, "Range")) addedRange = value;
    if (!strcmp(key, "Accept-Encoding")) assert(!strcmp(value, "identity"));
    return 0;
}
extern "C" int32_t sceHttpSendRequest(int32_t, const void* body, size_t bytes) {
    assert(addedBearer == reply().bearer && addedRange == reply().range);
    sentBody = body ? std::string(static_cast<const char*>(body), bytes) : "";
    insideSend = true;
    while (holdSend && !aborted) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return aborted ? -99 : reply().sendError;
}
extern "C" int32_t sceHttpGetStatusCode(int32_t, int32_t* status) { *status = reply().status; return 0; }
extern "C" int32_t sceHttpGetAllResponseHeaders(int32_t, char** output, size_t* bytes) {
    *output = &reply().headers[0]; *bytes = reply().headers.size(); return 0;
}
extern "C" int32_t sceHttpGetResponseContentLength(int32_t, int32_t* type, size_t* bytes) {
    *type = ORBIS_HTTP_CONTENTLEN_EXIST; *bytes = reply().contentLength; return 0;
}
extern "C" int32_t sceHttpReadData(int32_t, void* output, uint32_t capacity) {
    ++reads; Reply& r = reply();
    if (r.cursor >= r.failureAfter) return r.readError;
    if (r.cursor == r.body.size()) { assert(false && "must not read beyond exact framed response"); return 0; }
    size_t bytes = r.body.size() - r.cursor; if (bytes > capacity) bytes = capacity; if (bytes > 11) bytes = 11;
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
static void reset() {
    assert(!__atomic_load_n(&g_busy, __ATOMIC_ACQUIRE)); cleanHandles();
    lock(); delete g_result.catalog; g_result = {}; g_snapshot = {}; clearSessionLocked(); unlock();
    g_cancel = 0; g_request = -1;
    replies.clear(); opened = reads = connections = requestDeletes = connectionDeletes = 0;
    contexts = pools = sslContexts = templates = resolverCount = threadFailure = 0;
    privateDns = badDns = secure = noRedirect = false; holdSend = insideSend = aborted = false;
    assert(setHubOrigin(ORIGIN));
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

    reset(); // Actual import rejects HTML/non-PKG bodies, not just bad URLs.
    replies.push_back(rangeReply(PKG, std::string(1080, '<'), 0, 35454976)); assert(startHubImportUrls(PKG, strlen(PKG))); r = finished(); assert(r.errorCode && r.imports.rejected == 1 && !r.catalog->count()); freeHubResult(&r);
    reset(); std::string pkgHeader(1080, '\0'); memcpy(&pkgHeader[0], "\x7f" "CNT", 4); memcpy(&pkgHeader[0x40], "IV0000-SKID02048_00-GAME204800000000", 36); pkgHeader[0x77] = 26; pkgHeader[0x78] = 10;
    uint64_t total = 35454976; for (int i = 0; i < 8; ++i) pkgHeader[0x430 + i] = char(total >> (56 - i * 8));
    replies.push_back(rangeReply(PKG, pkgHeader, 0, total)); assert(startHubImportUrls(PKG, strlen(PKG))); r = finished(); assert(!r.errorCode && r.imports.added == 1 && r.catalog->count() == 1 && !r.catalog->at(0)->titleKnown); freeHubResult(&r);

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
    printf("hub client: async session/catalog/admin/import, expiry, TLS/range framing, credential isolation and cancellation checks passed\n");
    return 0;
}
