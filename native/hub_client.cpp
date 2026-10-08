#include "hub_client.h"
#include "hub_json.h"
#include "http_range.h"
#include "archive_sources.h"
#include "mediafire_source.h"
#include "peppy_hub_config.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/Net.h>
#include <orbis/NetCtl.h>
#include <orbis/Ssl.h>
#include <orbis/Http.h>

extern "C" int32_t peppyHubHttpRedirect(int32_t, int32_t) __asm__("sceHttpSetAutoRedirect");
extern "C" int32_t peppyHubHttpReceiveTimeout(int32_t, uint32_t) __asm__("sceHttpSetRecvTimeOut");
extern "C" int32_t peppyHubHttpHeaderLimit(int32_t, size_t) __asm__("sceHttpSetResponseHeaderMaxSize");
extern "C" int32_t peppyHubSslTerm(int32_t) __asm__("sceSslTerm");
extern "C" int32_t peppyHubModuleLoaded(OrbisSysModuleInternal) __asm__("sceSysmoduleIsLoadedInternal");
extern "C" int32_t peppyHubNetworkState(int32_t*) __asm__("sceNetCtlGetState");
extern "C" uint64_t peppyHubUptime() __asm__("sceKernelGetProcessTime");
struct PeppyHubAddress { uint32_t address; };
extern "C" int32_t peppyHubResolverCreate(const char*, int32_t, int32_t) __asm__("sceNetResolverCreate");
extern "C" int32_t peppyHubResolverDestroy(int32_t) __asm__("sceNetResolverDestroy");
extern "C" int32_t peppyHubResolverLookup(int32_t, const char*, PeppyHubAddress*, int32_t, int32_t, int32_t) __asm__("sceNetResolverStartNtoa");

#ifndef PEPPY_HUB_URL
#define PEPPY_HUB_URL ""
#endif

namespace {
const size_t BODY_CAP = 4 * 1024 * 1024;
const size_t PASSWORD_CAP = 129;
const size_t TOKEN_CAP = 257;
const size_t ORIGIN_CAP = 1024;
int g_lock = 0, g_busy = 0, g_cancel = 0, g_request = -1;
HubSnapshot g_snapshot = {};
HubSession g_session = {};
HubResult g_result = {};
char g_token[TOKEN_CAP] = {};
char g_origin[ORIGIN_CAP] = PEPPY_HUB_URL;
uint64_t g_sessionDeadline = 0, g_premiumDeadline = 0;
struct Task {
    int operation, days;
    char username[65], password[PASSWORD_CAP], token[TOKEN_CAP], origin[ORIGIN_CAP];
    char* text;
    size_t bytes;
    Task() : operation(0), days(0), text(0), bytes(0) {
        username[0] = password[0] = token[0] = origin[0] = 0;
    }
};
void lock() { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) sceKernelUsleep(1000); }
void unlock() { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }
void wipe(void* data, size_t bytes) { volatile unsigned char* p = static_cast<volatile unsigned char*>(data); while (bytes--) *p++ = 0; }
void destroyTask(Task* task) {
    if (!task) return;
    if (task->text) { wipe(task->text, task->bytes); free(task->text); }
    wipe(task, sizeof(*task)); delete task;
}
size_t length(const char* text, size_t cap) { if (!text) return cap; size_t n = 0; while (n < cap && text[n]) ++n; return n; }
uint64_t now() { time_t value = time(0); return value > 0 ? uint64_t(value) : 0; }
bool cancelled() { return __atomic_load_n(&g_cancel, __ATOMIC_ACQUIRE) != 0; }
void clearSessionLocked() {
    wipe(g_token, sizeof(g_token)); memset(&g_session, 0, sizeof(g_session));
    g_sessionDeadline = g_premiumDeadline = 0;
}
void expireLocked() {
    uint64_t clock = peppyHubUptime();
    if (g_session.authenticated && (!g_sessionDeadline || clock >= g_sessionDeadline)) clearSessionLocked();
    else if (g_session.premium && !g_session.admin && (!g_premiumDeadline || clock >= g_premiumDeadline)) g_session.premium = false;
}
bool usernameValid(const char* value) {
    size_t n = length(value, 65); if (!n || n >= 65) return false;
    for (size_t i = 0; i < n; ++i)
        if (!((value[i] >= 'a' && value[i] <= 'z') || (value[i] >= 'A' && value[i] <= 'Z') ||
              (value[i] >= '0' && value[i] <= '9') || value[i] == '_' || value[i] == '-' || value[i] == '.')) return false;
    return true;
}
bool tokenValid(const char* value) {
    size_t n = length(value, TOKEN_CAP); if (n < 32 || n >= TOKEN_CAP) return false;
    for (size_t i = 0; i < n; ++i)
        if (!((value[i] >= 'a' && value[i] <= 'z') || (value[i] >= 'A' && value[i] <= 'Z') ||
              (value[i] >= '0' && value[i] <= '9') || value[i] == '-' || value[i] == '_' || value[i] == '.')) return false;
    return true;
}
bool originValid(const char* value, char* normalized) {
    size_t n = length(value, ORIGIN_CAP); if (n >= ORIGIN_CAP || n < 9 || strncmp(value, "https://", 8)) return false;
    if (value[n - 1] == '/') --n;
    for (size_t i = 8; i < n; ++i) if (value[i] == '/' || value[i] == '?' || value[i] == '#') return false;
    char test[ORIGIN_CAP + 16]; memcpy(test, value, n); strcpy(test + n, "/api/status");
    if (!userCatalogPublicHttpsUrl(test)) return false;
    if (normalized) { memcpy(normalized, value, n); normalized[n] = 0; }
    return true;
}
int fail(int error, int32_t native = 0, int status = 0) {
    lock(); g_snapshot.errorCode = error; g_snapshot.nativeCode = native; g_snapshot.httpStatus = status; unlock(); return error;
}
void progress(size_t done, size_t total) { lock(); g_snapshot.completed = done; g_snapshot.requested = total; unlock(); }
void activeRequest(int request) { lock(); g_request = request; if (request >= 0 && cancelled()) sceHttpAbortRequest(request); unlock(); }

struct Handles {
    int net, ssl, http, tmpl, conn, req;
    Handles() : net(-1), ssl(-1), http(-1), tmpl(-1), conn(-1), req(-1) {}
    void closeRequest() {
        lock(); g_request = -1; if (req >= 0) sceHttpDeleteRequest(req); unlock();
        if (conn >= 0) sceHttpDeleteConnection(conn);
        req = conn = -1;
    }
    ~Handles() {
        closeRequest();
        if (tmpl >= 0) sceHttpDeleteTemplate(tmpl);
        if (http >= 0) sceHttpTerm(http);
        if (ssl >= 0) peppyHubSslTerm(ssl);
        if (net >= 0) sceNetPoolDestroy(net);
    }
};
struct RangeContext { Handles handles; bool ready; RangeContext() : ready(false) {} };
int init(Handles& h) {
    const OrbisSysModuleInternal modules[] = {ORBIS_SYSMODULE_INTERNAL_NET, ORBIS_SYSMODULE_INTERNAL_NETCTL,
        ORBIS_SYSMODULE_INTERNAL_SSL, ORBIS_SYSMODULE_INTERNAL_HTTP};
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i) {
        if (peppyHubModuleLoaded(modules[i]) == 0) continue;
        int32_t rc = int32_t(sceSysmoduleLoadModuleInternal(modules[i]));
        if (rc < 0 && peppyHubModuleLoaded(modules[i]) != 0) return fail(HUB_ERROR_NETWORK, rc);
    }
    int32_t state = -1, ctl = sceNetCtlInit(), rc = peppyHubNetworkState(&state);
    if ((ctl != 0 && rc != 0) || rc != 0 || state != 3) return fail(HUB_ERROR_NETWORK, rc ? rc : ctl);
    // Global networking is shared with downloads; own only context handles.
    sceNetInit();
    h.net = sceNetPoolCreate("PeppyHub", 1024 * 1024, 0); if (h.net < 0) return fail(HUB_ERROR_NETWORK, h.net);
    h.ssl = sceSslInit(256 * 1024); if (h.ssl < 0) return fail(HUB_ERROR_NETWORK, h.ssl);
    h.http = sceHttpInit(h.net, h.ssl, 1024 * 1024); if (h.http < 0) return fail(HUB_ERROR_NETWORK, h.http);
    h.tmpl = sceHttpCreateTemplate(h.http, "PeppyStore/Hub", ORBIS_HTTP_VERSION_1_1, 0);
    if (h.tmpl < 0) return fail(HUB_ERROR_NETWORK, h.tmpl);
    rc = peppyHubHttpHeaderLimit(h.tmpl, peppyHttpRange::HEADER_CAP);
    if (!rc) rc = sceHttpsEnableOption(h.tmpl, 0xbd);
    if (!rc) rc = peppyHubHttpRedirect(h.tmpl, 0);
    if (!rc) rc = sceHttpSetResolveTimeOut(h.tmpl, 10000000);
    if (!rc) rc = sceHttpSetConnectTimeOut(h.tmpl, 10000000);
    if (!rc) rc = sceHttpSetSendTimeOut(h.tmpl, 15000000);
    if (!rc) rc = peppyHubHttpReceiveTimeout(h.tmpl, 15000000);
    return rc < 0 ? fail(HUB_ERROR_NETWORK, rc) : 0;
}
bool publicAddress(const unsigned char* ip) {
    return ip[0] != 0 && ip[0] != 10 && ip[0] != 127 && ip[0] < 224 &&
        !(ip[0] == 100 && ip[1] >= 64 && ip[1] <= 127) &&
        !(ip[0] == 169 && ip[1] == 254) && !(ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) &&
        !(ip[0] == 192 && (ip[1] == 168 || (ip[1] == 0 && (ip[2] == 0 || ip[2] == 2)) || (ip[1] == 88 && ip[2] == 99))) &&
        !(ip[0] == 198 && (ip[1] == 18 || ip[1] == 19 || (ip[1] == 51 && ip[2] == 100))) &&
        !(ip[0] == 203 && ip[1] == 0 && ip[2] == 113);
}
int publicDns(Handles& h, const char* url) {
    const char* start = url + 8; const char* slash = strchr(start, '/');
    if (!slash) return fail(HUB_ERROR_SOURCE);
    size_t count = size_t(slash - start);
    if (count > 4 && !memcmp(slash - 4, ":443", 4)) count -= 4;
    if (!count || count > 253) return fail(HUB_ERROR_SOURCE);
    char hostname[254]; memcpy(hostname, start, count); hostname[count] = 0;
    int32_t resolver = peppyHubResolverCreate("PeppyHubDns", h.net, 0);
    if (resolver < 0) return fail(HUB_ERROR_NETWORK, resolver);
    PeppyHubAddress address = {};
    int32_t rc = peppyHubResolverLookup(resolver, hostname, &address, 5000000, 1, 0);
    peppyHubResolverDestroy(resolver);
    if (rc < 0) return fail(HUB_ERROR_NETWORK, rc);
    if (!publicAddress(reinterpret_cast<const unsigned char*>(&address.address))) return fail(HUB_ERROR_SOURCE);
    // libSceHttp resolves again and cannot pin this answer. Package requests
    // additionally require provider-owned names and normal hostname/certificate
    // verification; arbitrary user-controlled rebinding domains stay unsupported.
    return 0;
}
int open(Handles& h, const char* url, int method, const char* body, size_t bytes,
         const char* bearer, const char* range, int& status) {
    h.closeRequest();
    if (cancelled()) return fail(HUB_ERROR_CANCELLED);
    int dns = publicDns(h, url); if (dns) return dns;
    h.conn = sceHttpCreateConnectionWithURL(h.tmpl, url, false);
    if (h.conn < 0) return fail(HUB_ERROR_NETWORK, h.conn);
    h.req = sceHttpCreateRequestWithURL(h.conn, method, url, bytes);
    if (h.req < 0) return fail(HUB_ERROR_NETWORK, h.req);
    activeRequest(h.req);
    int32_t rc = sceHttpAddRequestHeader(h.req, "Accept-Encoding", "identity", 0);
    if (!rc) rc = sceHttpAddRequestHeader(h.req, "Accept", range ? "application/octet-stream" : "application/json", 0);
    if (!rc && body) rc = sceHttpAddRequestHeader(h.req, "Content-Type", "application/json", 0);
    if (!rc && range) rc = sceHttpAddRequestHeader(h.req, "Range", range, 0);
    char authorization[TOKEN_CAP + 8] = {};
    if (!rc && bearer && *bearer) {
        snprintf(authorization, sizeof(authorization), "Bearer %s", bearer);
        rc = sceHttpAddRequestHeader(h.req, "Authorization", authorization, 0);
        wipe(authorization, sizeof(authorization));
    }
    if (!rc) rc = sceHttpSendRequest(h.req, body, bytes);
    if (!rc) rc = sceHttpGetStatusCode(h.req, &status);
    if (rc < 0) return fail(cancelled() ? HUB_ERROR_CANCELLED : HUB_ERROR_NETWORK, rc);
    lock(); g_snapshot.httpStatus = status; unlock();
    return 0;
}
int readExact(Handles& h, unsigned char* output, size_t bytes) {
    size_t done = 0;
    while (done < bytes) {
        if (cancelled()) return fail(HUB_ERROR_CANCELLED);
        size_t remaining = bytes - done;
        uint32_t wanted = uint32_t(remaining > 64 * 1024 ? 64 * 1024 : remaining);
        int32_t got = sceHttpReadData(h.req, output + done, wanted);
        if (got < 0) return fail(HUB_ERROR_NETWORK, got);
        if (!got || uint32_t(got) > wanted) return fail(HUB_ERROR_RANGE);
        done += size_t(got);
    }
    // Content-Length/Content-Range already frame the response. Do not perform
    // an extra EOF read after the final byte (some PS4 transports time out).
    return 0;
}
int apiRequest(Handles& h, const Task& task, const char* path, bool post,
               const char* body, size_t bytes, char*& response, size_t& responseBytes) {
    response = 0; responseBytes = 0;
    char url[ORIGIN_CAP + 96]; int n = snprintf(url, sizeof(url), "%s%s", task.origin, path);
    if (n < 0 || size_t(n) >= sizeof(url)) return fail(HUB_ERROR_CONFIG);
    int status = 0; int rc = open(h, url, post ? 1 : ORBIS_METHOD_GET, body, bytes,
        task.operation == HUB_LOGIN ? 0 : task.token, 0, status);
    if (rc) return rc;
    if (status == 401 || status == 403) { lock(); clearSessionLocked(); unlock(); return fail(HUB_ERROR_AUTH, 0, status); }
    // Never redirect a credential or bearer request to a second origin.
    if (status < 200 || status > 299) return fail(HUB_ERROR_HTTP, 0, status);
    char* headers = 0; size_t headerBytes = 0;
    rc = sceHttpGetAllResponseHeaders(h.req, &headers, &headerBytes);
    peppyHttpRange::Metadata meta = {};
    if (rc < 0) return fail(HUB_ERROR_NETWORK, rc);
    if (!peppyHttpRange::parseHeaders(headers, headerBytes, &meta)) return fail(HUB_ERROR_HTTP);
    int32_t type = -1; size_t count = 0;
    rc = sceHttpGetResponseContentLength(h.req, &type, &count);
    if (rc < 0) return fail(HUB_ERROR_NETWORK, rc);
    if (type != ORBIS_HTTP_CONTENTLEN_EXIST || !meta.hasContentLength || meta.contentLength != count || !count || count > BODY_CAP)
        return fail(HUB_ERROR_LIMIT);
    response = static_cast<char*>(malloc(count + 1)); if (!response) return fail(HUB_ERROR_MEMORY);
    rc = readExact(h, reinterpret_cast<unsigned char*>(response), count);
    if (rc) { wipe(response, count); free(response); response = 0; return rc; }
    response[count] = 0; responseBytes = count; return 0;
}
bool parseUser(peppyHubJson::Cursor& json, HubSession& session) {
    if (!json.take('{')) return false;
    unsigned fields = 0; char key[65], role[17] = {};
    if (json.take('}')) return false;
    do {
        if (!json.string(key, sizeof(key)) || !json.take(':')) return false;
        unsigned bit = !strcmp(key, "id") ? 1 : !strcmp(key, "username") ? 2 : !strcmp(key, "role") ? 4 : !strcmp(key, "expires_at") ? 8 : 0;
        if (bit && (fields & bit)) return false;
        fields |= bit;
        if (bit == 1) { if (!json.string(session.userId, sizeof(session.userId))) return false; }
        else if (bit == 2) { if (!json.string(session.username, sizeof(session.username))) return false; }
        else if (bit == 4) { if (!json.string(role, sizeof(role))) return false; }
        else if (bit == 8) { if (!json.literal("null") && !json.number(session.premiumExpiresAt)) return false; }
        else if (!json.skip()) return false;
        if (json.take('}')) break;
        if (!json.take(',')) return false;
    } while (true);
    if ((fields & 7) != 7 || !session.userId[0] || !usernameValid(session.username)) return false;
    session.admin = !strcmp(role, "admin");
    if (!session.admin && strcmp(role, "premium") && strcmp(role, "user")) return false;
    session.premium = session.admin || !strcmp(role, "premium");
    return true;
}
int parseSession(const char* body, size_t bytes, bool login) {
    peppyHubJson::Cursor json(body, bytes); HubSession session = {}; char token[TOKEN_CAP] = {}, key[65];
    unsigned fields = 0; uint64_t serverTime = 0;
    bool valid = json.take('{') && !json.take('}');
    while (valid) {
        valid = json.string(key, sizeof(key)) && json.take(':'); if (!valid) break;
        unsigned bit = !strcmp(key, "token") ? 1 : !strcmp(key, "expires_at") ? 2 : !strcmp(key, "user") ? 4 : !strcmp(key, "server_time") ? 8 : 0;
        if (bit && (fields & bit)) { valid = false; break; } fields |= bit;
        if (bit == 1) valid = json.string(token, sizeof(token));
        else if (bit == 2) valid = json.number(session.expiresAt);
        else if (bit == 4) valid = parseUser(json, session);
        else if (bit == 8) valid = json.number(serverTime);
        else valid = json.skip();
        if (!valid || json.take('}')) break;
        valid = json.take(',');
    }
    valid = valid && json.done() && (fields & 6) == 6 && (!login || ((fields & 1) && tokenValid(token)));
    uint64_t reference = (fields & 8) ? serverTime : now();
    int result = !valid ? HUB_ERROR_JSON : !reference || session.expiresAt <= reference ? HUB_ERROR_EXPIRED : 0;
    uint64_t ttl = !result ? session.expiresAt - reference : 0;
    if (ttl > 86400) ttl = 86400;
    lock();
    if (!result && !cancelled()) {
        session.authenticated = true; g_session = session;
        uint64_t clock = peppyHubUptime();
        g_sessionDeadline = clock > UINT64_MAX - ttl * 1000000 ? UINT64_MAX : clock + ttl * 1000000;
        uint64_t premiumTtl = session.premiumExpiresAt > reference ? session.premiumExpiresAt - reference : 0;
        if (premiumTtl > ttl) premiumTtl = ttl;
        g_premiumDeadline = !premiumTtl ? 0 : clock > UINT64_MAX - premiumTtl * 1000000 ? UINT64_MAX : clock + premiumTtl * 1000000;
        if (!session.admin && !premiumTtl) g_session.premium = false;
        if (login) { wipe(g_token, sizeof(g_token)); memcpy(g_token, token, strlen(token) + 1); }
    } else clearSessionLocked();
    unlock(); wipe(token, sizeof(token)); return result ? fail(result) : 0;
}
bool parseEntry(peppyHubJson::Cursor& json, UserCatalogEntry& entry) {
    if (!json.take('{') || json.take('}')) return false;
    char key[65]; unsigned fields = 0; uint64_t value = 0; int category = 0;
    do {
        if (!json.string(key, sizeof(key)) || !json.take(':')) return false;
        unsigned bit = !strcmp(key, "url") ? 1 : !strcmp(key, "name") ? 2 : !strcmp(key, "version") ? 4 :
            !strcmp(key, "content_id") ? 8 : (!strcmp(key, "size_bytes") || !strcmp(key, "size")) ? 16 : !strcmp(key, "content_type") ? 32 :
            !strcmp(key, "content_flags") ? 64 : !strcmp(key, "kind") ? 128 : !strcmp(key, "is_theme") ? 256 :
            !strcmp(key, "description") ? 512 : !strcmp(key, "requires_data") ? 1024 : !strcmp(key, "sha256") ? 2048 :
            !strcmp(key, "adult") ? 4096 : !strcmp(key, "iro_tag") ? 8192 : !strcmp(key, "platform") ? 16384 :
            !strcmp(key, "display_category") ? 32768 : 0;
        if (bit && (fields & bit)) return false;
        fields |= bit;
        if (bit == 1) { if (!json.string(entry.url, sizeof(entry.url))) return false; }
        else if (bit == 2) { if (!json.string(entry.name, sizeof(entry.name))) return false; }
        else if (bit == 4) { if (!json.string(entry.version, sizeof(entry.version))) return false; }
        else if (bit == 8) { if (!json.string(entry.contentId, sizeof(entry.contentId))) return false; }
        else if (bit == 256) { if (!json.boolean(entry.isTheme)) return false; }
        else if (bit == 512) { if (!json.string(entry.description, sizeof(entry.description))) return false; }
        else if (bit == 1024) { if (!json.string(entry.requiresData, sizeof(entry.requiresData))) return false; }
        else if (bit == 2048) { if (!json.literal("null") && !json.string(entry.sha256, sizeof(entry.sha256))) return false; }
        else if (bit == 4096) { if (!json.boolean(entry.adult)) return false; }
        else if (bit == 16384) { char platform[17]; if (!json.string(platform, sizeof(platform)) || strcmp(platform, "ps4")) return false; }
        else if (bit == 128) {
            char kind[17]; if (!json.string(kind, sizeof(kind))) return false;
            entry.kind = !strcmp(kind, "base") || !strcmp(kind, "homebrew") || !strcmp(kind, "media") ? USER_PACKAGE_BASE :
                !strcmp(kind, "update") ? USER_PACKAGE_UPDATE : !strcmp(kind, "dlc") || !strcmp(kind, "theme") ? USER_PACKAGE_DLC : USER_PACKAGE_UNKNOWN;
            entry.displayCategory = !strcmp(kind, "homebrew") ? 3 : !strcmp(kind, "media") ? 4 : !strcmp(kind, "theme") ? 7 : 0;
            if (entry.kind < 0) return false;
        } else if (bit) {
            if (!json.number(value) || (bit != 16 && value > UINT32_MAX)) return false;
            if (bit == 16) entry.sizeBytes = value; else if (bit == 32) entry.contentType = uint32_t(value);
            else if (bit == 8192) entry.iroTag = uint32_t(value);
            else if (bit == 32768) { if (value > 7) return false; category = int(value); }
            else entry.contentFlags = uint32_t(value);
        } else if (!json.skip()) return false;
        if (json.take('}')) break;
        if (!json.take(',')) return false;
    } while (true);
    if ((fields & (1 | 2 | 8 | 16 | 32 | 64 | 128)) != (1 | 2 | 8 | 16 | 32 | 64 | 128) ||
        !entry.name[0] || !userPackageCanonicalContentId(entry.contentId) || !hubNativePackageUrl(entry.url)) return false;
    // Do not claim a server-selected link/name proves an official author source.
    entry.titleKnown = true; entry.versionKnown = entry.version[0] != 0;
    if (category) entry.displayCategory = category;
    return userCatalogPrepareEntry(&entry);
}
int parseCatalog(const char* body, size_t bytes, HubResult& output) {
    peppyHubJson::Cursor json(body, bytes); char key[65]; unsigned fields = 0;
    UserCatalog* catalog = new (std::nothrow) UserCatalog(); if (!catalog) return fail(HUB_ERROR_MEMORY);
    bool valid = json.take('{') && !json.take('}');
    while (valid) {
        valid = json.string(key, sizeof(key)) && json.take(':'); if (!valid) break;
        unsigned bit = !strcmp(key, "version") ? 1 : !strcmp(key, "entries") ? 2 : 0;
        if (bit && (fields & bit)) { valid = false; break; } fields |= bit;
        if (bit == 1) valid = json.number(output.catalogVersion);
        else if (bit == 2) {
            valid = json.take('[');
            if (valid && !json.take(']')) {
                do {
                    UserCatalogEntry entry = {};
                    valid = parseEntry(json, entry) && catalog->add(entry) == USER_CATALOG_OK;
                    if (!valid || json.take(']')) break;
                    valid = json.take(',');
                } while (valid);
            }
        } else valid = json.skip();
        if (!valid || json.take('}')) break;
        valid = json.take(',');
    }
    if (!valid || !json.done() || fields != 3) { delete catalog; return fail(HUB_ERROR_JSON); }
    output.catalog = catalog; return 0;
}
char* credentialBody(Task& task, size_t& bytes) {
    size_t capacity = 6 * (strlen(task.username) + strlen(task.password)) + 128;
    char* body = static_cast<char*>(malloc(capacity)); if (!body) return 0;
    const char* prefix = "{\"username\":\""; memcpy(body, prefix, strlen(prefix)); size_t used = strlen(prefix);
    bool ok = peppyHubJson::escape(task.username, strlen(task.username), body, capacity, used);
    const char* middle = "\",\"password\":\"";
    if (ok) { memcpy(body + used, middle, strlen(middle)); used += strlen(middle); ok = peppyHubJson::escape(task.password, strlen(task.password), body, capacity, used); }
    if (ok) {
        const char* plan = task.days == 15 ? "15d" : task.days == 30 ? "1m" : "2m";
        int count = task.operation == HUB_ADMIN_CREATE_USER ? snprintf(body + used, capacity - used, "\",\"plan\":\"%s\"}", plan) : snprintf(body + used, capacity - used, "\"}");
        ok = count > 0 && size_t(count) < capacity - used; used += ok ? size_t(count) : 0;
    }
    wipe(task.password, sizeof(task.password));
    if (!ok) { wipe(body, capacity); free(body); return 0; }
    bytes = used; return body;
}
int execute(Task& task, HubResult& result) {
    if (task.operation == HUB_IMPORT_URLS) {
        result.catalog = new (std::nothrow) UserCatalog(); if (!result.catalog) return fail(HUB_ERROR_MEMORY);
        RangeContext context;
        result.imports = result.catalog->importUrlList(task.text, task.bytes, hubUserCatalogRangeReader, &context);
        progress(result.imports.requested, result.imports.requested);
        if (cancelled()) return fail(HUB_ERROR_CANCELLED);
        HubSnapshot snapshot = hubSnapshot();
        int error = result.imports.firstError == USER_CATALOG_ERROR_NETWORK && snapshot.errorCode ? snapshot.errorCode :
            result.imports.firstError ? result.imports.firstError : HUB_ERROR_INPUT;
        return result.imports.added || result.imports.duplicates ? 0 : fail(error);
    }
    Handles handles; int error = init(handles); if (error) return error;
    char* body = 0; size_t bodyBytes = 0;
    if (task.operation == HUB_LOGIN || task.operation == HUB_ADMIN_CREATE_USER) {
        body = credentialBody(task, bodyBytes); if (!body) return fail(HUB_ERROR_MEMORY);
    }
    char* response = 0; size_t bytes = 0;
    if (task.operation == HUB_LOGIN) error = apiRequest(handles, task, "/api/login", true, body, bodyBytes, response, bytes);
    else if (task.operation == HUB_LOGOUT) error = apiRequest(handles, task, "/api/logout", true, "{}", 2, response, bytes);
    else if (task.operation == HUB_ADMIN_CREATE_USER) error = apiRequest(handles, task, "/api/admin/users", true, body, bodyBytes, response, bytes);
    else if (task.operation == HUB_ADMIN_PUBLISH) error = apiRequest(handles, task, "/api/admin/catalog", true, task.text, task.bytes, response, bytes);
    else {
        error = apiRequest(handles, task, "/api/session", false, 0, 0, response, bytes);
        if (!error) error = parseSession(response, bytes, false);
        if (response) { wipe(response, bytes); free(response); response = 0; }
        HubSession session = hubSession();
        if (!error && !session.premium) error = fail(HUB_ERROR_EXPIRED);
        if (!error) error = apiRequest(handles, task, "/api/catalog", false, 0, 0, response, bytes);
    }
    if (body) { wipe(body, bodyBytes); free(body); }
    wipe(task.password, sizeof(task.password));
    if (!error && task.operation == HUB_LOGIN) error = parseSession(response, bytes, true);
    else if (!error && task.operation == HUB_SYNC) error = parseCatalog(response, bytes, result);
    else if (!error) { peppyHubJson::Cursor json(response, bytes); if (!json.skip() || !json.done()) error = fail(HUB_ERROR_JSON); }
    if (response) { wipe(response, bytes); free(response); }
    return error;
}
void* worker(void* value) {
    Task* task = static_cast<Task*>(value); HubResult result = {}; result.operation = task->operation;
    int error = execute(*task, result);
    if (cancelled()) error = HUB_ERROR_CANCELLED;
    if (error && result.catalog && task->operation != HUB_IMPORT_URLS) { delete result.catalog; result.catalog = 0; }
    result.errorCode = error;
    lock();
    if (error && task->operation == HUB_LOGIN) clearSessionLocked();
    g_result = result; g_snapshot.errorCode = error;
    g_snapshot.state = error == HUB_ERROR_CANCELLED ? HUB_CANCELLED : error ? HUB_FAILED : HUB_DONE;
    unlock();
    destroyTask(task);
    __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE); return 0;
}
bool launch(Task* task, bool requiresSession, bool admin) {
    if (!task) return false;
    int expected = 0;
    if (!__atomic_compare_exchange_n(&g_busy, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { destroyTask(task); return false; }
    lock(); expireLocked();
    bool waiting = g_snapshot.state == HUB_DONE || g_snapshot.state == HUB_FAILED || g_snapshot.state == HUB_CANCELLED;
    int error = waiting ? HUB_ERROR_INPUT : task->operation != HUB_IMPORT_URLS && !originValid(g_origin, 0) ? HUB_ERROR_CONFIG :
        requiresSession && !g_session.authenticated ? HUB_ERROR_AUTH : admin && !g_session.admin ? HUB_ERROR_AUTH : 0;
    if (!error) {
        memcpy(task->origin, g_origin, strlen(g_origin) + 1); memcpy(task->token, g_token, strlen(g_token) + 1);
        if (task->operation == HUB_LOGIN || task->operation == HUB_LOGOUT) clearSessionLocked();
        g_snapshot = {}; g_snapshot.state = HUB_RUNNING; g_snapshot.operation = task->operation;
        g_result = {}; __atomic_store_n(&g_cancel, 0, __ATOMIC_RELEASE);
    }
    unlock();
    if (error) { if (!waiting) { lock(); g_snapshot = {}; g_snapshot.operation = task->operation; g_snapshot.state = HUB_FAILED; g_snapshot.errorCode = error; g_result = {}; g_result.operation = task->operation; g_result.errorCode = error; unlock(); } destroyTask(task); __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE); return false; }
    OrbisPthreadAttr attr; int32_t rc = scePthreadAttrInit(&attr); bool initialized = !rc;
    if (!rc) rc = scePthreadAttrSetdetachstate(&attr, 1);
    OrbisPthread thread;
    if (!rc) rc = scePthreadCreate(&thread, &attr, worker, task, "PeppyHub");
    if (initialized) scePthreadAttrDestroy(&attr);
    if (rc) {
        lock(); g_snapshot.state = HUB_FAILED; g_snapshot.errorCode = HUB_ERROR_THREAD; g_snapshot.nativeCode = rc; g_result = {}; g_result.operation = task->operation; g_result.errorCode = HUB_ERROR_THREAD; unlock();
        destroyTask(task); __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE); return false;
    }
    return true;
}
bool credentials(int operation, const char* username, const char* password, int days) {
    size_t n = length(password, PASSWORD_CAP);
    if (!usernameValid(username) || !n || n >= PASSWORD_CAP || (operation == HUB_ADMIN_CREATE_USER && days != 15 && days != 30 && days != 60)) return false;
    Task* task = new (std::nothrow) Task(); if (!task) return false;
    task->operation = operation; task->days = days; strcpy(task->username, username); memcpy(task->password, password, n + 1);
    return launch(task, operation != HUB_LOGIN, operation == HUB_ADMIN_CREATE_USER);
}
bool textTask(int operation, const char* text, size_t bytes) {
    size_t cap = operation == HUB_IMPORT_URLS ? USER_CATALOG_MAX_LIST_BYTES : BODY_CAP;
    if (!text || !bytes || bytes > cap || memchr(text, 0, bytes)) return false;
    Task* task = new (std::nothrow) Task(); if (!task) return false;
    task->text = static_cast<char*>(malloc(bytes + 1)); if (!task->text) { delete task; return false; }
    memcpy(task->text, text, bytes); task->text[bytes] = 0; task->bytes = bytes; task->operation = operation;
    return launch(task, operation != HUB_IMPORT_URLS, operation == HUB_ADMIN_PUBLISH);
}
bool hostEquals(const char* host, size_t bytes, const char* value) {
    if (strlen(value) != bytes) return false;
    for (size_t i = 0; i < bytes; ++i) if (peppyHttpRange::detail::lower(host[i]) != value[i]) return false;
    return true;
}
bool archivePackageUrl(const char* url) {
    const char* host = url + 8; const char* path = strchr(host, '/'); if (!path) return false;
    size_t hostBytes = size_t(path - host);
    // Match the existing downloader's exact reviewed Archive host/path policy.
    bool origin = hostEquals(host, hostBytes, "archive.org");
    if (!origin && !peppyArchiveSources::approvedCdnHost(host, hostBytes)) return false;
    const char* file = path;
    if (origin) { if (strncmp(file, "/download/", 10)) return false; file += 10; }
    else {
        if (*file++ != '/') return false;
        size_t digits = 0; while (*file >= '0' && *file <= '9') { ++file; ++digits; }
        if (!digits || digits > 10 || strncmp(file, "/items/", 7)) return false;
        file += 7;
    }
    const char* end = strchr(file, '/');
    if (!end || !peppyArchiveSources::approvedCollection(file, size_t(end - file))) return false;
    file = end + 1; size_t bytes = strlen(file);
    if (bytes <= 4 || strcmp(file + bytes - 4, ".pkg")) return false;
    bool first = true; unsigned continuation = 0; uint32_t code = 0, minimum = 0;
    for (size_t i = 0; i < bytes; ++i) {
        unsigned char ch = (unsigned char)file[i];
        if (ch == '%') {
            if (bytes - i < 3 || peppyHubJson::Cursor::hex(file[i + 1]) < 0 || peppyHubJson::Cursor::hex(file[i + 2]) < 0) return false;
            ch = (unsigned char)(peppyHubJson::Cursor::hex(file[i + 1]) * 16 + peppyHubJson::Cursor::hex(file[i + 2])); i += 2;
        } else if (ch <= 32 || ch >= 127) return false;
        if (continuation) {
            if ((ch & 0xc0) != 0x80) return false;
            code = (code << 6) | (ch & 63);
            if (!--continuation && (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff) || (code >= 0x7f && code <= 0x9f))) return false;
            continue;
        }
        if (ch >= 128) {
            if (ch >= 0xc2 && ch <= 0xdf) { continuation = 1; code = ch & 31; minimum = 0x80; }
            else if (ch >= 0xe0 && ch <= 0xef) { continuation = 2; code = ch & 15; minimum = 0x800; }
            else if (ch >= 0xf0 && ch <= 0xf4) { continuation = 3; code = ch & 7; minimum = 0x10000; }
            else return false;
            first = false; continue;
        }
        if (ch < 32 || ch == 127 || ch == '/' || ch == '\\' || ch == '?' || ch == '#' || ch == '%' || ch == '"' || ch == '<' || ch == '>' || ch == '`' || (first && ch == '.')) return false;
        first = false;
    }
    return !continuation;
}
int packageProvider(const char* url) {
    if (!userCatalogPublicHttpsUrl(url)) return 0;
    const char* host = url + 8; const char* end = strchr(host, '/'); if (!end) return 0;
    size_t count = size_t(end - host); if (count > 4 && !memcmp(end - 4, ":443", 4)) count -= 4;
    const char* github[] = {"github.com", "raw.githubusercontent.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com", "github-releases.githubusercontent.com"};
    for (size_t i = 0; i < sizeof(github) / sizeof(github[0]); ++i) if (hostEquals(host, count, github[i])) return 1;
    if (archivePackageUrl(url)) return 2;
    if (peppyMediafire::isCdnUrl(url, strlen(url))) return 3;
    if (!strcmp(url, "https://gamebatoapp.ir/home/app.pkg")) return 4;
    return 0;
}
bool redirect(const char* headers, size_t bytes, char* output) {
    if (!headers || !bytes || bytes > peppyHttpRange::HEADER_CAP) return false;
    if (!headers[bytes - 1]) --bytes;
    bool found = false; size_t position = 0;
    while (position < bytes) {
        size_t begin = position;
        while (position < bytes && headers[position] != '\r' && headers[position] != '\n' && headers[position]) ++position;
        if (bytes - position < 2 || headers[position] != '\r' || headers[position + 1] != '\n') return false;
        size_t count = position - begin; position += 2;
        if (!count) return position == bytes && found;
        const char* colon = static_cast<const char*>(memchr(headers + begin, ':', count));
        if (!colon) { if (begin != 0 || !peppyHttpRange::detail::statusLine(headers, count)) return false; continue; }
        if (!peppyHttpRange::detail::equalNoCase(headers + begin, size_t(colon - headers - begin), "Location")) continue;
        if (found) return false;
        const char* start = colon + 1; const char* end = headers + begin + count;
        while (start < end && (*start == ' ' || *start == '\t')) ++start;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;
        size_t n = size_t(end - start); if (!n || n > USER_CATALOG_MAX_URL_BYTES) return false;
        memcpy(output, start, n); output[n] = 0; found = true;
    }
    return found;
}
} // namespace

bool hubNativePackageUrl(const char* url) {
    return packageProvider(url) != 0;
}
bool hubUserCatalogRangeReader(void* context, const char* url, uint64_t offset, size_t requested,
                               unsigned char* output, UserCatalogRangeInfo* info) {
    if (!info || !output || !requested || requested > USER_CATALOG_MAX_METADATA_BYTES || offset > UINT64_MAX - requested || !hubNativePackageUrl(url)) { fail(HUB_ERROR_SOURCE); return false; }
    *info = {};
    RangeContext local;
    RangeContext* rangeContext = context ? static_cast<RangeContext*>(context) : &local;
    Handles& h = rangeContext->handles;
    if (!rangeContext->ready) { if (init(h)) return false; rangeContext->ready = true; }
    char current[USER_CATALOG_MAX_URL_BYTES + 1]; strcpy(current, url);
    char range[96]; snprintf(range, sizeof(range), "bytes=%llu-%llu", (unsigned long long)offset, (unsigned long long)(offset + requested - 1));
    for (int attempt = 0; attempt <= 5; ++attempt) {
        int status = 0; if (open(h, current, ORBIS_METHOD_GET, 0, 0, 0, range, status)) return false;
        char* headers = 0; size_t headerBytes = 0;
        int32_t rc = sceHttpGetAllResponseHeaders(h.req, &headers, &headerBytes);
        if (rc < 0) { fail(HUB_ERROR_NETWORK, rc); return false; }
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            char next[USER_CATALOG_MAX_URL_BYTES + 1];
            if (attempt == 5 || !redirect(headers, headerBytes, next) || packageProvider(next) != packageProvider(current)) { fail(HUB_ERROR_SOURCE); return false; }
            strcpy(current, next); continue;
        }
        peppyHttpRange::Metadata metadata = {};
        if (status != 206 || !peppyHttpRange::parseHeaders(headers, headerBytes, &metadata) || !metadata.hasContentRange ||
            metadata.first != offset || metadata.last != offset + requested - 1 || !metadata.hasContentLength || metadata.contentLength != requested) { fail(HUB_ERROR_RANGE, 0, status); return false; }
        int32_t type = -1; size_t count = 0;
        rc = sceHttpGetResponseContentLength(h.req, &type, &count);
        if (rc < 0 || type != ORBIS_HTTP_CONTENTLEN_EXIST || count != requested) { fail(HUB_ERROR_RANGE, rc, status); return false; }
        if (readExact(h, output, requested)) return false;
        info->received = requested; info->totalBytes = metadata.total; strcpy(info->effectiveUrl, current); return true;
    }
    return false;
}
bool setHubOrigin(const char* origin) {
    char normalized[ORIGIN_CAP] = {};
    if (origin && *origin && !originValid(origin, normalized)) return false;
    if (__atomic_load_n(&g_busy, __ATOMIC_ACQUIRE)) return false;
    lock(); clearSessionLocked(); strcpy(g_origin, normalized); unlock(); return true;
}
bool hubConfigured() { lock(); bool configured = originValid(g_origin, 0); unlock(); return configured; }
bool hubOrigin(char* output, size_t capacity) {
    if (!output || !capacity) return false;
    lock(); size_t n = strlen(g_origin); bool fits = n < capacity;
    if (fits) memcpy(output, g_origin, n + 1); else output[0] = 0;
    unlock(); return fits;
}
bool startHubLogin(const char* username, const char* password) { return credentials(HUB_LOGIN, username, password, 0); }
bool startHubAdminCreateUser(const char* username, const char* password, int days) { return credentials(HUB_ADMIN_CREATE_USER, username, password, days); }
bool startHubImportUrls(const char* urls, size_t bytes) { return textTask(HUB_IMPORT_URLS, urls, bytes); }
bool startHubAdminPublish(const char* json, size_t bytes) { return textTask(HUB_ADMIN_PUBLISH, json, bytes); }
bool startHubLogout() { Task* task = new (std::nothrow) Task(); if (!task) return false; task->operation = HUB_LOGOUT; return launch(task, true, false); }
bool startHubSync() { Task* task = new (std::nothrow) Task(); if (!task) return false; task->operation = HUB_SYNC; return launch(task, true, false); }
void cancelHubOperation() {
    __atomic_store_n(&g_cancel, 1, __ATOMIC_RELEASE);
    lock(); if (g_request >= 0) sceHttpAbortRequest(g_request); unlock();
}
HubSnapshot hubSnapshot() { lock(); HubSnapshot snapshot = g_snapshot; unlock(); return snapshot; }
HubSession hubSession() { lock(); expireLocked(); HubSession session = g_session; unlock(); return session; }
bool consumeHubResult(HubResult* output) {
    if (!output || __atomic_load_n(&g_busy, __ATOMIC_ACQUIRE)) return false;
    lock(); bool ready = g_snapshot.state == HUB_DONE || g_snapshot.state == HUB_FAILED || g_snapshot.state == HUB_CANCELLED;
    if (ready) { *output = g_result; g_result = {}; g_snapshot = {}; }
    unlock(); return ready;
}
void freeHubResult(HubResult* result) { if (!result) return; delete result->catalog; *result = {}; }
const char* hubErrorMessage(int error) {
    switch (error) {
        case HUB_OK: return "Concluido";
        case HUB_ERROR_CONFIG: return "Servidor Peppy ainda nao configurado";
        case HUB_ERROR_INPUT: return "Confira os dados informados";
        case HUB_ERROR_THREAD: return "Nao foi possivel iniciar a tarefa";
        case HUB_ERROR_NETWORK: return "Falha na conexao HTTPS";
        case HUB_ERROR_HTTP: return "Servidor recusou a solicitacao";
        case HUB_ERROR_AUTH: return "Usuario, senha ou acesso indisponivel";
        case HUB_ERROR_EXPIRED: return "Acesso expirado; entre novamente";
        case HUB_ERROR_JSON: return "Resposta do catalogo invalida";
        case HUB_ERROR_LIMIT: return "Resposta excedeu o limite permitido";
        case HUB_ERROR_SOURCE: return "Use um PKG direto de uma fonte compativel";
        case HUB_ERROR_RANGE: return "Fonte nao oferece leitura parcial de PKG";
        case HUB_ERROR_CANCELLED: return "Tarefa cancelada";
        case HUB_ERROR_MEMORY: return "Memoria insuficiente";
        default: return userCatalogErrorMessage(error);
    }
}
