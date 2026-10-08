#ifndef PEPPY_USER_CATALOG_H
#define PEPPY_USER_CATALOG_H

#include <stddef.h>
#include <stdint.h>
#include "user_pkg_header.h"

static const size_t USER_CATALOG_MAX_ITEMS = 1024;
static const size_t USER_CATALOG_MAX_URL_BYTES = 2048;
static const size_t USER_CATALOG_MAX_LIST_BYTES = 2 * 1024 * 1024;
static const size_t USER_CATALOG_MAX_METADATA_BYTES = 100000;

enum UserCatalogSource {
    USER_SOURCE_COMMUNITY_UNKNOWN = 0,
    USER_SOURCE_RECOGNIZED_AUTHOR_RELEASE = 1
};
enum UserCatalogError {
    USER_CATALOG_OK = 0,
    USER_CATALOG_ERROR_URL = -3000,
    USER_CATALOG_ERROR_NETWORK = -3001,
    USER_CATALOG_ERROR_RANGE = -3002,
    USER_CATALOG_ERROR_HEADER = -3003,
    USER_CATALOG_ERROR_METADATA = -3004,
    USER_CATALOG_ERROR_DUPLICATE = -3005,
    USER_CATALOG_ERROR_FULL = -3006,
    USER_CATALOG_ERROR_MEMORY = -3007,
    USER_CATALOG_ERROR_FILE = -3008,
    USER_CATALOG_ERROR_NOT_FOUND = -3009,
    USER_CATALOG_ERROR_LIST = -3010
};

struct UserCatalogEntry {
    char url[USER_CATALOG_MAX_URL_BYTES + 1];
    char name[129];
    char version[41];       // Empty means metadata unavailable, never a guessed version.
    char contentId[37];
    char titleId[10];
    char filename[96];      // Generated ASCII basename, independent of the remote filename.
    char description[601];
    char requiresData[601];
    char sha256[65];        // Empty means no published full-file hash.
    uint64_t sizeBytes;
    uint32_t contentType;
    uint32_t contentFlags;
    uint32_t iroTag;        // Header theme tag; zero means no evidence for a theme.
    int kind;
    int source;
    int displayCategory;   // 0 default; native categories 1..7, independent of package kind.
    bool titleKnown;
    bool versionKnown;
    bool isTheme;
    bool adult;            // Supplied rating label, never inferred from a package header.
};

struct UserCatalogRangeInfo {
    size_t received;
    uint64_t totalBytes;
    char effectiveUrl[USER_CATALOG_MAX_URL_BYTES + 1];
};

// Callback runs on the caller's import worker, never the render thread.
// It must perform verified HTTPS, resolve ONLY public addresses, validate each
// redirect before connecting, and request precisely the given byte range.
// Return true only for an exact 206 Content-Range response (or an exact whole
// file response if the requested range is the whole file). Reject cookies,
// browser/authentication pages, compressed body/range substitutions and URLs
// that reach local/private/link-local/multicast hosts. Core rechecks sizes and
// effective URL syntax. DNS rebinding protection belongs to this adapter.
typedef bool (*UserCatalogRangeReader)(void* context, const char* url,
                                      uint64_t offset, size_t requested,
                                      unsigned char* output, UserCatalogRangeInfo* info);

struct UserCatalogImportReport {
    size_t requested;
    size_t added;
    size_t duplicates;
    size_t rejected;
    int firstError;
};

bool userCatalogPublicHttpsUrl(const char* url);
int userCatalogSourceForUrl(const char* url);
bool userCatalogEntryValid(const UserCatalogEntry& entry);
// For an API catalog, derive bounded local filename/TitleID/source/category.
// Does not fetch the PKG or authenticate metadata; download header pinning is
// still required. Content type/flags/kind/size/name must already be supplied.
bool userCatalogPrepareEntry(UserCatalogEntry* entry);
const char* userCatalogKindName(int kind);
const char* userCatalogErrorMessage(int error);
const char* userCatalogDefaultPath();
// Read a completed local UTF-8 URL list without following symlinks or blocking
// on devices/FIFOs. Caller frees *output; failure sets it to null/zero bytes.
int userCatalogReadUrlListFile(const char* path, char** output, size_t* bytes);

// Probe downloads less than 100KB: header, bounded entry table, optional SFO.
// It does not download/install the game or prove licensing/signatures/firmware
// compatibility. Header identity/kind/size is checked; SFO names are optional.
int probeUserPackage(const char* url, UserCatalogRangeReader reader, void* context,
                     UserCatalogEntry* output);

class UserCatalog {
public:
    UserCatalog();
    ~UserCatalog();
    size_t count() const;
    const UserCatalogEntry* at(size_t index) const;
    int add(const UserCatalogEntry& entry);
    int importUrl(const char* url, UserCatalogRangeReader reader, void* context);
    UserCatalogImportReport importUrlList(const char* text, size_t bytes,
                                         UserCatalogRangeReader reader, void* context);
    bool remove(size_t index);
    void clear();
    // Atomic .dat persistence with explicit endian-neutral encoding/checksum.
    // Parent directory must exist; no credentials or trusted access flags are
    // stored. Failed load/save preserves the current in-memory catalog/file.
    int save(const char* path = 0) const;
    int load(const char* path = 0);
private:
    UserCatalogEntry* entries_;
    size_t count_;
    UserCatalog(const UserCatalog&);
    UserCatalog& operator=(const UserCatalog&);
};

#endif
