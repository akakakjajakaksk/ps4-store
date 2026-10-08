#ifndef PEPPY_HUB_CLIENT_H
#define PEPPY_HUB_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include "user_catalog.h"

enum HubOperation { HUB_NONE = 0, HUB_LOGIN, HUB_LOGOUT, HUB_SYNC,
                    HUB_IMPORT_URLS, HUB_ADMIN_CREATE_USER, HUB_ADMIN_PUBLISH,
                    HUB_ADMIN_LIST_USERS, HUB_ADMIN_REVOKE_USER, HUB_ADMIN_CHANGE_PASSWORD };
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
struct HubSavedLoginStatus {
    bool configured, hasUsername, hasPassword, hasToken, restoring, storageError;
};
const size_t HUB_ADMIN_MAX_USERS = 5000;
struct HubAdminUser {
    char id[65], username[65], plan[4];
    bool admin, revoked, premiumActive;
    uint64_t expiresAt;
};
struct HubResult {
    int operation, errorCode;
    uint64_t catalogVersion;
    UserCatalog* catalog; // Owned by result after consume; release with freeHubResult.
    UserCatalogImportReport imports;
    HubAdminUser* users; // Owned metadata only; passwords/hashes are never returned.
    size_t userCount;
};

// Controller-thread entry points. One operation runs at a time; completing a
// result does not download/install packages. Consume/free a result before starting
// another operation. Credentials and bearer tokens are never logged or included
// in the PKG. Saved login is opt-in through configureHubSavedLogin below.
// Setting the origin clears any session. Configure only the administrator's
// trusted HTTPS origin; a login sends credentials to that exact origin.
bool setHubOrigin(const char* trustedHttpsOrigin);
inline bool configureHubBaseUrl(const char* origin) { return setHubOrigin(origin); }
bool hubConfigured();
bool hubOrigin(char* output, size_t capacity);
// Creates the private /data/peppy-store/private directory (0700), separate from
// the FTP inbox, after the app's base directory exists. The local file is 0600
// and contains the last successful login's password and revocable
// bearer. This is filesystem privacy, not a PS4 keychain or encryption.
// Its stored role/metadata cannot grant access: restore always asks the server.
bool configureHubSavedLogin(const char* path = 0);
HubSavedLoginStatus hubSavedLoginStatus();
bool hubSavedLoginCredentials(char* username, size_t usernameCapacity,
                             char* password, size_t passwordCapacity);
// Non-blocking startup restore. Reports HUB_LOGIN so the normal result handler
// can synchronize premium items. Expired tokens allow one saved-password login;
// a heartbeat denial never starts a replacement login. Explicit logout forgets
// the local password/token, retaining only the username.
bool startHubSavedLogin();
bool startHubLogin(const char* username, const char* password);
bool startHubLogout();
bool startHubSync();
bool startHubImportUrls(const char* newlineSeparatedUrls, size_t bytes);
bool startHubAdminCreateUser(const char* username, const char* password, int planDays);
// JSON is an administrator-owned publication request. Server validates metadata;
// successful publication does not make arbitrary source URLs trusted.
bool startHubAdminPublish(const char* json, size_t bytes);
bool startHubAdminListUsers();
bool startHubAdminRevokeUser(const char* userId, bool revoked = true);
bool startHubAdminChangePassword(const char* userId, const char* newPassword);
// Call from the render/controller loop. An independent small session request
// runs every five seconds, including while a download/import is active.
// Server denial immediately clears RAM authorization; after 30 seconds without
// a verified session, authorization is also cleared until the user logs in.
void pollHubSession();
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
