#ifndef PEPPY_HUB_CLIENT_H
#define PEPPY_HUB_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include "user_catalog.h"

enum HubOperation { HUB_NONE = 0, HUB_LOGIN, HUB_LOGOUT, HUB_SYNC,
                    HUB_IMPORT_URLS, HUB_ADMIN_CREATE_USER, HUB_ADMIN_PUBLISH };
enum HubState { HUB_IDLE = 0, HUB_RUNNING, HUB_DONE, HUB_FAILED, HUB_CANCELLED };
enum HubError { HUB_OK = 0, HUB_ERROR_CONFIG = -4000, HUB_ERROR_INPUT = -4001,
                HUB_ERROR_THREAD = -4002, HUB_ERROR_NETWORK = -4003,
                HUB_ERROR_HTTP = -4004, HUB_ERROR_AUTH = -4005,
                HUB_ERROR_EXPIRED = -4006, HUB_ERROR_JSON = -4007,
                HUB_ERROR_LIMIT = -4008, HUB_ERROR_SOURCE = -4009,
                HUB_ERROR_RANGE = -4010, HUB_ERROR_CANCELLED = -4011,
                HUB_ERROR_MEMORY = -4012 };

struct HubSession {
    bool authenticated, premium, admin;
    uint64_t expiresAt, premiumExpiresAt; // Unix UTC seconds; zero = no premium expiry for admin.
    char username[65];
    char userId[65];
};
struct HubSnapshot {
    int state, operation, errorCode, httpStatus;
    int32_t nativeCode;
    size_t completed, requested;
};
struct HubResult {
    int operation, errorCode;
    uint64_t catalogVersion;
    UserCatalog* catalog; // Owned by result after consume; release with freeHubResult.
    UserCatalogImportReport imports;
};

// Controller-thread entry points. One operation runs at a time; completing a
// result does not download/install packages. Consume/free a result before starting
// another operation. Credentials and bearer tokens are never persisted/logged.
// Setting the origin clears any session. Configure only the administrator's
// trusted HTTPS origin; a login sends credentials to that exact origin.
bool setHubOrigin(const char* trustedHttpsOrigin);
inline bool configureHubBaseUrl(const char* origin) { return setHubOrigin(origin); }
bool hubConfigured();
bool hubOrigin(char* output, size_t capacity);
bool startHubLogin(const char* username, const char* password);
bool startHubLogout();
bool startHubSync();
bool startHubImportUrls(const char* newlineSeparatedUrls, size_t bytes);
bool startHubAdminCreateUser(const char* username, const char* password, int planDays);
// JSON is an administrator-owned publication request. Server validates metadata;
// successful publication does not make arbitrary source URLs trusted.
bool startHubAdminPublish(const char* json, size_t bytes);
void cancelHubOperation();
HubSnapshot hubSnapshot();
HubSession hubSession(); // Rechecks local session/premium expiry on every call.
bool consumeHubResult(HubResult* output);
void freeHubResult(HubResult* result);
const char* hubErrorMessage(int error);

// Bounded reader used only from worker threads. Native libSceHttp cannot pin DNS
// answers. Consequently this adapter restricts fetches/redirects to reviewed
// public provider hosts, and rejects unsupported generic hosts before connecting.
// It never sends a hub token/cookie to a package host and requires exact 206 framing.
bool hubUserCatalogRangeReader(void* context, const char* url, uint64_t offset,
                               size_t requested, unsigned char* output,
                               UserCatalogRangeInfo* info);
bool hubNativePackageUrl(const char* url);

#endif
