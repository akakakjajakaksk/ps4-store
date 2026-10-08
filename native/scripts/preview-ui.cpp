// Host preview of the exact framebuffer renderer used by the PS4 build.
// Build after generating ui_assets.h and ui_catalog.h:
// g++ -std=c++11 -O2 native/scripts/preview-ui.cpp -o /tmp/peppy-preview
// /tmp/peppy-preview /tmp/peppy
#define PEPPY_UI_PREVIEW
#include "../downloads.h"
#include "../install.h"
#include "../music.h"
#include "../hub_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static DownloadSnapshot previewDownload = {};
static InstallSnapshot previewInstall = {};
static MusicSnapshot previewMusic = {MUSIC_PLAYING, 0, 30, false, 0};
static int downloadCalls = 0, installCalls = 0, cancelInstallCalls = 0, cancelDownloadCalls = 0;
static bool holdDownloadCancel = false;
static bool rejectDownload = false, rejectInstall = false, rejectInstallBusy = false;
static InstallSpec lastInstall = {};
static const char* lastDownloadContentId = 0;
static int lastDownloadKind = 0, lastInstallKind = 0;
DownloadSnapshot downloadSnapshot() { return previewDownload; }
bool startDownload(const DownloadSpec& spec, const char* expectedContentId, int expectedKind) {
    lastDownloadKind = expectedKind;
    ++downloadCalls;
    lastDownloadContentId = expectedContentId;
    previewDownload = {};
    previewDownload.state = rejectDownload ? FAILED : RUNNING;
    previewDownload.total = spec.expectedBytes;
    if (rejectDownload) previewDownload.errorCode = DOWNLOAD_ERROR_THREAD;
    return !rejectDownload;
}
void cancelDownload() { ++cancelDownloadCalls; if (!holdDownloadCancel) previewDownload.state = CANCELLED; }
const char* downloadStageName(int stage) {
    if (stage == DOWNLOAD_STAGE_SOURCE_READ) return "Página da fonte";
    if (stage == DOWNLOAD_STAGE_SOURCE_PARSE) return "Resolver link do PKG";
    return stage == DOWNLOAD_STAGE_SEND ? "Enviar pedido" : "Diagnóstico de rede";
}
InstallSnapshot installSnapshot() { return previewInstall; }
bool startInstall(const InstallSpec& spec) {
    ++installCalls;
    if (rejectInstallBusy) return false;
    lastInstall = spec;
    uint32_t generation = previewInstall.generation + 1;
    previewInstall = {};
    previewInstall.generation = generation;
    previewInstall.state = rejectInstall ? INSTALL_FAILED : INSTALL_RUNNING;
    previewInstall.total = spec.expectedBytes;
    previewInstall.stage = rejectInstall ? INSTALL_STAGE_THREAD : INSTALL_STAGE_REGISTER;
    previewInstall.taskId = 7;
    if (rejectInstall) previewInstall.errorCode = INSTALL_ERROR_THREAD;
    return !rejectInstall;
}
bool startTypedInstall(const InstallSpec& spec, int kind) { lastInstallKind = kind; return startInstall(spec); }
void cancelInstall() { ++cancelInstallCalls; }
const char* installStageName(int stage) {
    switch (stage) {
    case INSTALL_STAGE_REGISTER: return "Registrar instalação";
    case INSTALL_STAGE_PROGRESS: return "Instalar no PS4";
    case INSTALL_STAGE_THREAD: return "Iniciar instalação";
    case INSTALL_STAGE_RESTORE: return "Restaurar acesso";
    case INSTALL_STAGE_USER: return "Usuário do PS4";
    case INSTALL_STAGE_SERVER_START: return "Preparar entrega do PKG";
    case INSTALL_STAGE_SERVER_TRANSFER: return "Entregar PKG ao instalador";
    case INSTALL_STAGE_SERVER_STOP: return "Encerrar entrega do PKG";
    default: return "Preparar instalação";
    }
}
MusicSnapshot musicSnapshot() { return previewMusic; }
bool musicStart(int32_t) { previewMusic.state = MUSIC_PLAYING; return true; }
void musicSetVolume(int volume) { previewMusic.volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume); }
void musicToggleMute() { previewMusic.muted = !previewMusic.muted; }
void musicNextTrack() { previewMusic.track = (previewMusic.track + 1) % 2; }
void musicStop() { previewMusic.state = MUSIC_STOPPED; }
void musicShutdown() { musicStop(); }
const char* musicTrackName(int track) { return track == 1 ? "ACENDAOFAROL" : "FIGHT"; }
static uint64_t previewTimeUs = 0;
static uint64_t previewNowUs() { return previewTimeUs; }
static HubSession previewHubSession = {};
static HubSnapshot previewHub = {};
static HubResult previewHubResult = {};
static bool previewHubReady = false;
static int previewHubCalls = 0;
static char previewHubOrigin[256] = {};
static char previewImportedUrls[8192] = {};
static char previewLoginUser[65] = {}, previewLoginPassword[129] = {};
static int previewCreatedPlan = 0, previewSessionCheckCalls = 0;
static char previewRevokedUser[65] = {};
static bool previewRevokedValue = false;
static char previewPublishedJson[16384] = {};
static bool previewStartHub(int operation) {
    if (previewHub.state == HUB_RUNNING || previewHubReady) return false;
    previewHub = {}; previewHub.state = HUB_RUNNING; previewHub.operation = operation; ++previewHubCalls; return true;
}
bool setHubOrigin(const char* origin) { snprintf(previewHubOrigin, sizeof(previewHubOrigin), "%s", origin); previewHubSession = {}; return true; }
bool hubConfigured() { return previewHubOrigin[0]; }
bool hubOrigin(char* output, size_t cap) { if (!output || !cap) return false; snprintf(output, cap, "%s", previewHubOrigin); return hubConfigured(); }
bool startHubLogin(const char* user, const char* password) {
    if (!previewStartHub(HUB_LOGIN)) return false;
    snprintf(previewLoginUser, sizeof(previewLoginUser), "%s", user);
    snprintf(previewLoginPassword, sizeof(previewLoginPassword), "%s", password); return true;
}
bool startHubLogout() { if (!previewStartHub(HUB_LOGOUT)) return false; previewHubSession = {}; return true; }
bool startHubSync() { return previewStartHub(HUB_SYNC); }
bool startHubImportUrls(const char* urls, size_t bytes) {
    if (!bytes || bytes >= sizeof(previewImportedUrls) || !previewStartHub(HUB_IMPORT_URLS)) return false;
    memcpy(previewImportedUrls, urls, bytes); previewImportedUrls[bytes] = 0; return true;
}
bool startHubAdminCreateUser(const char*, const char*, int days) { previewCreatedPlan = days; return previewHubSession.admin && previewStartHub(HUB_ADMIN_CREATE_USER); }
bool startHubAdminPublish(const char* json, size_t bytes) {
    if (!previewHubSession.admin || bytes >= sizeof(previewPublishedJson) || !previewStartHub(HUB_ADMIN_PUBLISH)) return false;
    memcpy(previewPublishedJson, json, bytes); previewPublishedJson[bytes] = 0; return true;
}
bool startHubAdminListUsers() { return previewHubSession.admin && previewStartHub(HUB_ADMIN_LIST_USERS); }
bool startHubAdminRevokeUser(const char* id, bool revoked) {
    if (!previewHubSession.admin || !previewStartHub(HUB_ADMIN_REVOKE_USER)) return false;
    snprintf(previewRevokedUser, sizeof(previewRevokedUser), "%s", id); previewRevokedValue = revoked; return true;
}
bool startHubAdminChangePassword(const char* id, const char* password) {
    if (!previewHubSession.admin || strlen(password) < 8 || !previewStartHub(HUB_ADMIN_CHANGE_PASSWORD)) return false;
    snprintf(previewRevokedUser, sizeof(previewRevokedUser), "%s", id);
    snprintf(previewLoginPassword, sizeof(previewLoginPassword), "%s", password); return true;
}
void pollHubSession() { ++previewSessionCheckCalls; }
void cancelHubOperation() { previewHub.state = HUB_CANCELLED; }
HubSnapshot hubSnapshot() { return previewHub; }
HubSession hubSession() { return previewHubSession; }
bool consumeHubResult(HubResult* out) {
    if (!previewHubReady) return false;
    *out = previewHubResult; previewHubResult = {}; previewHubReady = false; previewHub = {}; return true;
}
void freeHubResult(HubResult* result) { delete result->catalog; free(result->users); *result = {}; }
const char* hubErrorMessage(int) { return "Falha ao consultar o serviço"; }
bool hubNativePackageUrl(const char* url) { return userCatalogPublicHttpsUrl(url); }
bool hubUserCatalogRangeReader(void*, const char*, uint64_t, size_t, unsigned char*, UserCatalogRangeInfo*) { return false; }

#include "../boot_test.cpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void resetController() {
    activeCategory = 0;
    storeLocal.clear();
    storeClearRemote(); storeClearAdminUsers(); storeSelectedAdminUser = {}; storeAccountPassword[0] = 0;
    if (storeHasPending) freeHubResult(&storePending);
    storePending = {}; storeHasPending = false;
    if (previewHubReady) freeHubResult(&previewHubResult);
    previewHubReady = false; previewHubResult = {}; previewHub = {}; previewHubSession = {};
    previewHubCalls = 0; storePanel = STORE_PANEL_NONE; storeMenuSelection = 0;
    storeAdminLogin = storeAdminChord = storeAdultConfirmed = storeFtpRequested = false;
    storeHadSession = storeHadAdmin = storeHadEntitlement = false;
    previewSessionCheckCalls = 0; previewRevokedUser[0] = 0; previewRevokedValue = false;
    storeKeyboard = StoreKeyboard(); storeNotice[0] = 0;
    storeLoginUser[0] = storeLoginPassword[0] = storeAdminUser[0] = storeAdminPassword[0] = 0;
    lastDownloadKind = lastInstallKind = 0;
    catalogSearch = CatalogSearchState();
    downloadingApp = installingApp = -1;
    autoInstallPending = installCancelRequested = false;
    memset(downloadedBytes, 0, sizeof(downloadedBytes));
    memset(installedApps, 0, sizeof(installedApps));
    previewDownload = {};
    previewInstall = {};
    previewMusic = {MUSIC_PLAYING, 0, 30, false, 0};
    downloadCalls = installCalls = cancelInstallCalls = cancelDownloadCalls = 0; holdDownloadCancel = false;
    rejectDownload = rejectInstall = rejectInstallBusy = false;
    lastDownloadContentId = 0;
    previewTimeUs = 0;
    storeNextSyncUs = 0;
    downloadMeter.reset();
}

static bool expect(bool condition, const char* message) {
    if (!condition) fprintf(stderr, "Controller check failed: %s\n", message);
    return condition;
}

static UserCatalogEntry serviceFixture(int serial, int kind, bool theme = false, bool adult = false, int category = 0) {
    UserCatalogEntry e = {};
    snprintf(e.contentId, sizeof(e.contentId), "UP0000-CUSA%05d_00-PEPPYUITEST00000", serial);
    snprintf(e.name, sizeof(e.name), "Peppy fixture %d", serial);
    snprintf(e.url, sizeof(e.url), "https://github.com/skidgfx/PS4-2048/releases/download/v1.0/fixture-%d-%d.pkg", serial, kind);
    e.kind = kind; e.contentType = kind == USER_PACKAGE_DLC ? (theme ? 0x1B : 0x1C) : kind == USER_PACKAGE_UPDATE ? 0x1E : 0x1A;
    e.contentFlags = 0x0A000000; e.sizeBytes = 4096; e.titleKnown = true;
    e.isTheme = theme; e.iroTag = theme ? 1 : 0; e.adult = adult; e.displayCategory = category;
    bool okay = userCatalogPrepareEntry(&e);
    if (!okay) fprintf(stderr, "Service fixture metadata failed\n");
    return e;
}
static bool verifyServicesController() {
    resetController(); int selected = 0; bool details = false;
    for (int first = 0; first < CATEGORY_COUNT; first += 6) {
        int right = 420;
        for (int i = first; i < CATEGORY_COUNT && i < first + 6; ++i) right += textWidth(CATEGORY_NAMES[i], FONT_BODY) + 26;
        if (!expect(right - 26 < 1650, "paginated category tabs fit beside the header indicator")) return false;
    }
    if (!expect(categoryCount() == UI_APP_COUNT, "all compiled free entries remain available without a session")) return false;
    handleCatalogController(CATALOG_OPTIONS, selected, details);
    if (!expect(storePanel == STORE_PANEL_SERVICES && !catalogSearch.open, "Options opens services and preserves catalog search")) return false;
    const uint32_t chord = CATALOG_R2 | CATALOG_R3 | CATALOG_OPTIONS;
    handleCatalogController(CATALOG_OPTIONS, selected, details, chord);
    if (!expect(storePanel == STORE_PANEL_SERVICES && !storeAdminLogin, "the admin chord is unavailable outside premium login")) return false;
    storeMenuSelection = 0; handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(storePanel == STORE_PANEL_TEXT && storeKeyboard.target == STORE_TEXT_URLS, "normal users can import direct PKG links")) return false;
    const char* needed = "abcABC012:/?%=&_-.";
    for (const char* p = needed; *p; ++p) if (!expect(strchr(storeKeyboardKeys(), *p), "URL keyboard contains every required ASCII key")) return false;
    snprintf(storeKeyboard.draft, sizeof(storeKeyboard.draft), "https://github.com/one.pkg");
    handleCatalogController(CATALOG_L1, selected, details);
    if (!expect(strchr(storeKeyboard.draft, '\n'), "URL keyboard supports a newline-separated list")) return false;
    handleCatalogController(CATALOG_OPTIONS, selected, details);
    if (!expect(previewHub.operation == HUB_IMPORT_URLS && !strcmp(previewImportedUrls, "https://github.com/one.pkg\n"), "URL import queues a worker instead of blocking the UI")) return false;
    previewHub = {}; storeOpenPanel(STORE_PANEL_PREMIUM);
    handleCatalogController(CATALOG_R2, selected, details, CATALOG_R2);
    handleCatalogController(CATALOG_R3, selected, details, CATALOG_R2 | CATALOG_R3);
    handleCatalogController(CATALOG_OPTIONS, selected, details, chord);
    if (!expect(storeAdminLogin && storePanel == STORE_PANEL_PREMIUM && !previewHubSession.admin,
                "held R2/R3 plus a later Options edge reveals admin login without granting access")) return false;
    storeMenuSelection = 1; handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(storeKeyboard.masked, "password editor requests masked rendering")) return false;
    snprintf(storeKeyboard.draft, sizeof(storeKeyboard.draft), "fixture-secret");
    handleCatalogController(CATALOG_OPTIONS, selected, details);
    if (!expect(!storeKeyboard.draft[0] && !strcmp(storeLoginPassword, "fixture-secret"), "accepted password leaves no editor draft")) return false;
    snprintf(storeLoginUser, sizeof(storeLoginUser), "fixture-user");
    storeMenuSelection = 2; handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(!storeLoginPassword[0] && !strcmp(previewLoginPassword, "fixture-secret") && !previewHubSession.authenticated,
                "login copies the password to its worker, wipes the UI field and waits for server authentication")) return false;
    previewHubResult = {}; previewHubResult.operation = HUB_LOGIN; previewHubResult.errorCode = HUB_ERROR_AUTH; previewHubReady = true;
    pollStoreExtensions();
    if (!expect(!previewHubSession.authenticated && storeNotice[0], "failed authentication displays an error and grants no session")) return false;
    storeOpenPanel(STORE_PANEL_NONE); storeAdminLogin = false;
    for (int kind = 0; kind < 3; ++kind) if (!expect(storeLocal.add(serviceFixture(99990, kind)) == 0, "personal base/update/DLC metadata accepted")) return false;
    storeRebuildViews();
    for (int kind = 0; kind < 3; ++kind) {
        activeCategory = 8 + kind;
        if (!expect(categoryCount() == 1 && appIndex(0) == STORE_LOCAL_FIRST + kind, "each personal package kind has its own category")) return false;
    }
    if (!expect(activateApp(STORE_LOCAL_FIRST + 1) && lastDownloadKind == USER_PACKAGE_UPDATE,
                "an imported update passes its pinned kind to the downloader")) return false;
    previewDownload.state = DONE; previewDownload.received = previewDownload.total = storeApp(STORE_LOCAL_FIRST + 1).sizeBytes;
    pollAutoInstall();
    if (!expect(lastInstallKind == USER_PACKAGE_UPDATE && previewInstall.state == INSTALL_RUNNING,
                "a completed imported update starts typed installation")) return false;
    previewInstall.state = INSTALL_DONE; pollAutoInstall();
    previewHubSession.authenticated = previewHubSession.admin = true;
    storeAdminChord = false; storeOpenPanel(STORE_PANEL_PREMIUM);
    handleCatalogController(CATALOG_OPTIONS, selected, details, chord);
    if (!expect(storePanel == STORE_PANEL_ADMIN, "the hidden panel opens only after server-provided admin authorization")) return false;
    storeMenuSelection = 2; storeAdminPlan = 15;
    handleCatalogController(CATALOG_RIGHT, selected, details);
    if (!expect(storeAdminPlan == 30, "administrator can select the one-month premium plan")) return false;
    snprintf(storeAdminUser, sizeof(storeAdminUser), "new-fixture-user");
    snprintf(storeAdminPassword, sizeof(storeAdminPassword), "new-fixture-secret");
    storeMenuSelection = 3; handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(previewHub.operation == HUB_ADMIN_CREATE_USER && previewCreatedPlan == 30 && !storeAdminPassword[0],
                "creating a premium account queues the selected plan and wipes its password field")) return false;
    previewHub = {}; storeMenuSelection = 4; handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(previewHub.operation == HUB_ADMIN_PUBLISH && strstr(previewPublishedJson, "\"kind\":\"update\"") &&
                strstr(previewPublishedJson, "\"kind\":\"dlc\"") && strstr(previewPublishedJson, "\"content_id\":") &&
                strstr(previewPublishedJson, "\"display_category\":") && !strstr(previewPublishedJson, "new-fixture-secret"),
                "publication contains pinned package metadata and no premium credentials")) return false;
    previewHub = {}; storeMenuSelection = 6; handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(storePanel == STORE_PANEL_USERS && previewHub.operation == HUB_ADMIN_LIST_USERS,
                "administrator loads user metadata without credentials to manage access")) return false;
    previewHubResult = {}; previewHubResult.operation = HUB_ADMIN_LIST_USERS;
    previewHubResult.users = static_cast<HubAdminUser*>(calloc(2, sizeof(HubAdminUser))); previewHubResult.userCount = 2;
    snprintf(previewHubResult.users[0].id, 65, "%s", "00000000000000000000000000000000");
    snprintf(previewHubResult.users[0].username, 65, "%s", "owner"); previewHubResult.users[0].admin = true;
    snprintf(previewHubResult.users[1].id, 65, "%s", "11111111111111111111111111111111");
    snprintf(previewHubResult.users[1].username, 65, "%s", "premium_fixture"); previewHubResult.users[1].premiumActive = true;
    previewHubReady = true; pollStoreExtensions();
    handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(storePanel == STORE_PANEL_USERS, "administrator accounts cannot be invalidated by premium management")) return false;
    handleCatalogController(CATALOG_DOWN, selected, details); handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(storePanel == STORE_PANEL_ACCOUNT && !strcmp(storeSelectedAdminUser.id, "11111111111111111111111111111111"), "editing an account pins its identity")) return false;
    handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(storePanel == STORE_PANEL_REVOKE && previewHub.state == HUB_IDLE,
                "invalidation requires confirmation of the selected premium user")) return false;
    previewHubResult = {}; previewHubResult.operation = HUB_ADMIN_LIST_USERS;
    previewHubResult.users = static_cast<HubAdminUser*>(calloc(2, sizeof(HubAdminUser))); previewHubResult.userCount = 2;
    snprintf(previewHubResult.users[0].id, 65, "%s", "22222222222222222222222222222222");
    snprintf(previewHubResult.users[0].username, 65, "%s", "different_account");
    snprintf(previewHubResult.users[1].id, 65, "%s", "owner");
    snprintf(previewHubResult.users[1].username, 65, "%s", "owner"); previewHubResult.users[1].admin = true;
    previewHubReady = true; pollStoreExtensions();
    if (!expect(storePanel == STORE_PANEL_REVOKE && !strcmp(storeSelectedAdminUser.id, "11111111111111111111111111111111"),
                "a refreshed and reordered list cannot change the confirmation target")) return false;
    handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(previewHub.operation == HUB_ADMIN_REVOKE_USER && previewRevokedValue &&
                !strcmp(previewRevokedUser, "11111111111111111111111111111111"), "confirmation sends the exact account ID for server invalidation")) return false;
    previewHub = {}; storeSelectedAdminUser.revoked = true; storeOpenPanel(STORE_PANEL_REVOKE);
    handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(previewHub.operation == HUB_ADMIN_REVOKE_USER && !previewRevokedValue,
                "the same pinned account can be reactivated")) return false;
    previewHub = {}; storeOpenPanel(STORE_PANEL_ACCOUNT); storeMenuSelection = 1;
    handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(storePanel == STORE_PANEL_TEXT && storeKeyboard.masked, "administrator password rotation uses the masked editor")) return false;
    snprintf(storeKeyboard.draft, sizeof(storeKeyboard.draft), "new_rotated_password");
    handleCatalogController(CATALOG_OPTIONS, selected, details); storeMenuSelection = 2;
    handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(previewHub.operation == HUB_ADMIN_CHANGE_PASSWORD && !storeAccountPassword[0] && !storeKeyboard.draft[0] &&
                !strcmp(previewRevokedUser, "11111111111111111111111111111111") && !strcmp(previewLoginPassword, "new_rotated_password"),
                "password rotation keeps the selected identity, transfers its secret to the worker and wipes UI copies")) return false;
    previewHub = {}; previewHubSession = {}; storeOpenPanel(STORE_PANEL_NONE);
    storeRemote = new UserCatalog;
    storeRemote->add(serviceFixture(99991, 0, false, false, 4));
    storeRemote->add(serviceFixture(99992, 2, true));
    storeRemote->add(serviceFixture(99993, 0, false, true));
    storeRebuildViews(); activeCategory = 0;
    if (!expect(categoryCount() == UI_APP_COUNT + 3 && !activateApp(STORE_REMOTE_FIRST), "remote premium metadata is hidden and cannot be activated without entitlement")) return false;
    previewHubSession.authenticated = previewHubSession.premium = true;
    if (!expect(categoryCount() == UI_APP_COUNT + 5, "entitled users see remote media/themes while adult entries remain gated")) return false;
    activeCategory = 7;
    if (!expect(categoryCount() == 1 && appIndex(0) == STORE_REMOTE_FIRST + 1, "theme metadata appears in the theme menu")) return false;
    activeCategory = 12; selected = 0;
    handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(storePanel == STORE_PANEL_ADULT && !storeAdultConfirmed, "adult category requires an explicit age confirmation")) return false;
    handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(storeAdultConfirmed && categoryCount() == 1, "confirmation reveals only labelled adult entries")) return false;
    activeCategory = 0;
    if (!expect(activateApp(STORE_REMOTE_FIRST), "entitled remote download starts")) return false;
    const char* stableName = storeApp(STORE_REMOTE_FIRST).name;
    UserCatalog* replacement = new UserCatalog; replacement->add(serviceFixture(99994, 0));
    previewHubResult = {}; previewHubResult.operation = HUB_SYNC; previewHubResult.catalog = replacement;
    previewHubResult.catalogVersion = 9; previewHubReady = true;
    pollStoreExtensions();
    if (!expect(storeHasPending && storeApp(STORE_REMOTE_FIRST).name == stableName && downloadingApp == STORE_REMOTE_FIRST,
                "catalog refresh defers replacement while a download owns runtime indices")) return false;
    previewDownload.state = DONE; previewDownload.received = previewDownload.total = storeApp(STORE_REMOTE_FIRST).sizeBytes;
    pollAutoInstall(); pollStoreExtensions();
    if (!expect(storeHasPending && storeApp(STORE_REMOTE_FIRST).name == stableName,
                "refresh remains deferred through the automatic installation handoff")) return false;
    previewInstall.state = INSTALL_DONE; pollAutoInstall(); pollStoreExtensions();
    if (!expect(!storeHasPending && storeRemote == replacement && storeCatalogVersion == 9 && downloadingApp == -1,
                "idle refresh replaces the remote catalog and clears old operation associations")) return false;
    previewHub = {}; startHubLogout();
    if (!expect(!storeAppAllowed(STORE_REMOTE_FIRST) && categoryCount() == UI_APP_COUNT + 3,
                "logout immediately hides premium entries but retains free and personal packages")) return false;
    pollStoreExtensions();
    if (!expect(!storeRemote && storeLocal.count() == 3, "logout discards only the RAM remote catalog")) return false;
    if (!expect(!strcmp(STORE_LIVEPIX, "https://livepix.gg/peppystore"), "LivePix link is exact")) return false;
    resetController(); configureHubBaseUrl(PEPPY_HUB_URL);
    previewHubSession.authenticated = previewHubSession.premium = true;
    pollStoreExtensions(); previewTimeUs = 300000000ULL;
    storeOpenPanel(STORE_PANEL_PREMIUM); pollStoreExtensions();
    if (!expect(!previewHubCalls, "periodic synchronization waits while credentials are edited")) return false;
    storeOpenPanel(STORE_PANEL_NONE); previewDownload.state = RUNNING; pollStoreExtensions();
    if (!expect(!previewHubCalls, "periodic synchronization waits for active downloads")) return false;
    previewDownload.state = DONE; previewInstall.state = INSTALL_RUNNING; pollStoreExtensions();
    if (!expect(!previewHubCalls, "periodic synchronization waits for active installation")) return false;
    previewInstall.state = INSTALL_DONE; catalogSearch.begin(); pollStoreExtensions();
    if (!expect(!previewHubCalls, "periodic synchronization does not interrupt search editing")) return false;
    catalogSearch.input(SEARCH_CANCEL); pollStoreExtensions();
    if (!expect(previewHub.operation == HUB_SYNC && previewHub.state == HUB_RUNNING && storeNextSyncUs == 600000000ULL,
                "idle premium clients schedule asynchronous catalog refresh every five minutes")) return false;
    resetController(); configureHubBaseUrl(PEPPY_HUB_URL);
    previewHubSession.authenticated = previewHubSession.premium = true;
    storeRemote = new UserCatalog; storeRemote->add(serviceFixture(99995, 0)); storeRebuildViews(); pollStoreExtensions();
    activateApp(STORE_REMOTE_FIRST); holdDownloadCancel = true;
    const char* activeName = storeApp(STORE_REMOTE_FIRST).name;
    uint32_t oldView = storeViewRevision; int pollsBefore = previewSessionCheckCalls;
    previewHubSession = {}; pollStoreExtensions();
    if (!expect(previewSessionCheckCalls > pollsBefore && cancelDownloadCalls == 1 && !autoInstallPending &&
                !storeAppAllowed(STORE_REMOTE_FIRST) && storeViewRevision != oldView && storeRemote &&
                storeApp(STORE_REMOTE_FIRST).name == activeName,
                "revocation checks run during downloads, cancel only premium work, hide entries and retain worker pointers")) return false;
    previewDownload.state = DONE; previewDownload.received = previewDownload.total; pollAutoInstall();
    if (!expect(!installCalls, "revoked download completion cannot start an automatic premium install")) return false;
    pollStoreExtensions(); if (!expect(!storeRemote, "retired premium metadata is freed after its worker becomes idle")) return false;
    resetController(); configureHubBaseUrl(PEPPY_HUB_URL); previewHubSession.authenticated = previewHubSession.premium = true;
    storeRemote = new UserCatalog; storeRemote->add(serviceFixture(99996, 0)); storeRebuildViews(); pollStoreExtensions();
    activateApp(STORE_REMOTE_FIRST); previewDownload.state = DONE; previewDownload.received = previewDownload.total; pollAutoInstall();
    previewHubSession = {}; pollStoreExtensions();
    if (!expect(previewInstall.state == INSTALL_RUNNING && !cancelInstallCalls && storeRemote && !storeAppAllowed(STORE_REMOTE_FIRST),
                "revocation lets an existing system installation finish safely while hiding premium metadata")) return false;
    previewInstall.state = INSTALL_DONE; pollAutoInstall(); pollStoreExtensions();
    if (!expect(!storeRemote, "completed revoked installation releases its retained catalog")) return false;
    resetController(); previewHubSession.authenticated = previewHubSession.premium = true; pollStoreExtensions();
    activateApp(0); previewHubSession = {}; pollStoreExtensions();
    if (!expect(previewDownload.state == RUNNING && !cancelDownloadCalls, "revocation leaves a free download running")) return false;
    resetController();
    puts("Checked services, masked passwords, admin user invalidation/reactivation/rotation, pinned confirmation targets, live revocation transfer safety, held admin chord, personal package kinds and stable async catalog replacement.");
    return true;
}

static void completeDownload(int index) {
    previewDownload.state = DONE;
    previewDownload.received = previewDownload.total = storeApp(index).sizeBytes;
}

static void samplePreviewDownload(uint64_t now, uint64_t received) {
    previewTimeUs = now;
    previewDownload.received = received;
    downloadMeter.update(previewDownload.state == RUNNING, previewDownload.received,
                         previewDownload.total, previewNowUs());
}

static bool verifySearchController() {
    resetController();
    int selected = UI_APP_COUNT - 1;
    bool details = false;
    if (!expect(categoryCount() == UI_APP_COUNT && appIndex(selected) == selected,
                "an empty search retains the full verified catalog")) return false;
    handleCatalogController(CATALOG_RIGHT, selected, details);
    if (!expect(selected == 0, "library navigation wraps the filtered selection")) return false;
    handleCatalogController(CATALOG_L1, selected, details);
    if (!expect(activeCategory == CATEGORY_COUNT - 1 && selected == 0,
                "category navigation reaches the last extension tab")) return false;
    handleCatalogController(CATALOG_R1, selected, details);
    if (!expect(activeCategory == 0, "all categories wrap back to Todos")) return false;

    activateApp(0);
    handleCatalogController(CATALOG_R3 | CATALOG_CROSS | CATALOG_SQUARE, selected, details);
    if (!expect(catalogSearch.open && !details && !previewMusic.muted && !catalogSearch.draft[0],
                "opening search consumes simultaneous outside controls")) return false;
    handleCatalogController(CATALOG_CROSS, selected, details);
    handleCatalogController(CATALOG_TRIANGLE, selected, details);
    if (!expect(strcmp(catalogSearch.draft, "A ") == 0 && previewDownload.state == RUNNING,
                "modal X types and Triangle inserts a space without cancelling a download")) return false;
    handleCatalogController(CATALOG_SQUARE | CATALOG_R1 | CATALOG_L3, selected, details);
    if (!expect(strcmp(catalogSearch.draft, "A") == 0 && !previewMusic.muted &&
                previewMusic.track == 0 && activeCategory == 0,
                "modal erase isolates music and category controls")) return false;
    handleCatalogController(CATALOG_CIRCLE | CATALOG_TRIANGLE | CATALOG_CROSS, selected, details);
    if (!expect(!catalogSearch.open && !catalogSearch.query[0] && !details &&
                previewDownload.state == RUNNING && downloadCalls == 1 && installCalls == 0,
                "cancelling the editor preserves the committed filter and active transfer")) return false;

    resetController();
    selected = UI_APP_COUNT - 1;
    handleCatalogController(CATALOG_R3, selected, details);
    snprintf(catalogSearch.draft, sizeof(catalogSearch.draft), "%s", UI_APPS[0].id);
    handleCatalogController(CATALOG_OPTIONS | CATALOG_CROSS | CATALOG_SQUARE, selected, details);
    if (!expect(!catalogSearch.open && selected == 0 && !details && !previewMusic.muted &&
                categoryCount() > 0 && appIndex(0) == 0 && downloadCalls == 0,
                "applying an ID query safely resets selection and consumes other actions")) return false;
    handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(details && appIndex(selected) == 0, "search results open the correct package details")) return false;
    handleCatalogController(CATALOG_R3, selected, details);
    if (!expect(!catalogSearch.open, "details retain their existing controls")) return false;
    handleCatalogController(CATALOG_CIRCLE, selected, details);
    handleCatalogController(CATALOG_R1, selected, details);
    if (!expect(activeCategory == 1 && appIndex(selected) == 0 && catalogSearch.query[0],
                "changing categories preserves and combines the query")) return false;
    handleCatalogController(CATALOG_R1, selected, details);
    handleCatalogController(CATALOG_CROSS | CATALOG_LEFT | CATALOG_RIGHT, selected, details);
    if (!expect(categoryCount() == 0 && selected == 0 && !details && appIndex(selected) == -1 && downloadCalls == 0,
                "an empty filtered category cannot open or download an unrelated package")) return false;

    resetController();
    for (int i = 0; i < UI_APP_COUNT; ++i) {
        if (!UI_APPS[i].contentId[0]) continue;
        snprintf(catalogSearch.query, sizeof(catalogSearch.query), "%s", UI_APPS[i].contentId);
        if (!expect(categoryCount() > 0 && appIndex(0) == i,
                    "a full Content ID finds its actual verified catalog entry")) return false;
        break;
    }
    catalogSearch = CatalogSearchState();
    snprintf(catalogSearch.query, sizeof(catalogSearch.query), "__NO_SUCH_PKG_74C6__");
    selected = UI_APP_COUNT + 12;
    details = true;
    handleCatalogController(CATALOG_CROSS, selected, details);
    if (!expect(categoryCount() == 0 && selected == 0 && !details && downloadCalls == 0,
                "a stale selection is clamped safely when no query results remain")) return false;
    handleCatalogController(CATALOG_R3, selected, details);
    catalogSearch.draft[0] = 0;
    handleCatalogController(CATALOG_OPTIONS, selected, details);
    if (!expect(categoryCount() == UI_APP_COUNT && selected == 0,
                "applying an empty query restores the full category")) return false;
    for (int category = 5; category <= 10; ++category) {
        activeCategory = category;
        selected = 0;
        handleCatalogController(CATALOG_CROSS, selected, details);
        if (!expect(categoryCount() || (!details && appIndex(selected) == -1 && downloadCalls == 0),
                    "empty update and DLC tabs cannot launch an action")) return false;
        details = false;
    }
    resetController();
    puts("Checked controller search, modal isolation, category/query composition, Content IDs and safe empty selection.");
    return true;
}

static bool verifyMeasurementUi() {
    resetController();
    if (!expect(activateApp(0), "a new download starts the measurement baseline")) return false;
    char label[96];
    transferMeasurementLabel(label, sizeof(label), previewDownload);
    if (!expect(strcmp(label, "Medindo a velocidade...") == 0,
                "a new transfer does not invent a speed or ETA")) return false;
    samplePreviewDownload(1000000, 1000000);
    transferMeasurementLabel(label, sizeof(label), previewDownload);
    if (!expect(strstr(label, "1.00 MB/s | Estimativa:") == label,
                "one million delivered bytes per second displays decimal MB/s and an estimate")) return false;
    samplePreviewDownload(4000000, 1000000);
    transferMeasurementLabel(label, sizeof(label), previewDownload);
    if (!expect(strcmp(label, "0.00 MB/s | Aguardando dados...") == 0,
                "a stalled transfer reports zero without reusing a stale ETA")) return false;
    samplePreviewDownload(4200000, previewDownload.total);
    transferMeasurementLabel(label, sizeof(label), previewDownload);
    if (!expect(strcmp(label, "Finalizando o download...") == 0,
                "the final bytes do not promise validation or installation completed")) return false;

    previewDownload.state = CANCELLED;
    previewTimeUs = 5000000;
    if (!expect(activateApp(0) && !downloadMeter.measurement().rateAvailable,
                "an accepted same-app retry discards the previous measurement")) return false;
    samplePreviewDownload(6000000, 1000000);
    completeDownload(0);
    downloadMeter.update(false, previewDownload.received, previewDownload.total, 6200000);
    int networkStarts = downloadCalls;
    downloadedBytes[0] = UI_APPS[0].sizeBytes;
    if (!expect(activateApp(0) && downloadCalls == networkStarts && !downloadMeter.measurement().rateAvailable,
                "installing a saved PKG does not start a fictitious network measurement")) return false;

    resetController();
    previewDownload.state = RUNNING;
    previewDownload.total = 0;
    samplePreviewDownload(0, 0);
    samplePreviewDownload(1000000, 1000000);
    transferMeasurementLabel(label, sizeof(label), previewDownload);
    if (!expect(strcmp(label, "1.00 MB/s | Calculando tempo restante...") == 0,
                "an unknown total can show measured speed while withholding ETA")) return false;
    resetController();
    return true;
}

// Exercise the user-visible handoff: one validated download, one automatic
// installation, an explicit install-only retry, and cancellation/cleanup gates.
static bool verifyController() {
    if (UI_APP_COUNT < 3) return expect(false, "preview requires at least three catalog entries");
    resetController();
    rejectDownload = true;
    activateApp(0);
    if (!expect(previewDownload.state == FAILED && !autoInstallPending,
                "an immediate download failure cannot queue an installation")) return false;
    pollAutoInstall();
    if (!expect(installCalls == 0, "failed downloads never install")) return false;

    resetController();
    activateApp(0);
    completeDownload(0);
    rejectInstall = true;
    pollAutoInstall();
    for (int i = 0; i < 20; ++i) pollAutoInstall();
    if (!expect(downloadCalls == 1 && installCalls == 1 && !autoInstallPending,
                "automatic installation is attempted exactly once, including startup failure")) return false;
    if (!expect(lastInstall.expectedBytes == UI_APPS[0].sizeBytes &&
                strcmp(lastInstall.filename, UI_APPS[0].filename) == 0 &&
                strcmp(lastInstall.name, UI_APPS[0].name) == 0,
                "installation receives the completed download's validated bytes and metadata")) return false;

    rejectInstall = false;
    activateApp(1);
    previewDownload.state = FAILED;
    pollAutoInstall();
    activateApp(0);
    if (!expect(downloadCalls == 2 && installCalls == 2,
                "install retry reuses the saved PKG after navigating to another app")) return false;
    for (int i = 0; i < 20; ++i) pollAutoInstall();
    if (!expect(installCalls == 2 && !activateApp(2),
                "an active installation blocks another download and cannot auto-retry")) return false;
    if (!expect(!cancelOperation(1) && cancelOperation(0) && cancelInstallCalls == 1,
                "Triangle cancels only the selected app's own installation")) return false;
    if (!expect(!activateApp(1), "downloads remain blocked while cancellation is running")) return false;
    previewInstall.state = INSTALL_CANCELLED;
    pollAutoInstall();
    activateApp(0);
    if (!expect(installCalls == 3 && downloadCalls == 2,
                "cancelled installation can be explicitly retried without downloading again")) return false;
    previewInstall.state = INSTALL_DONE;
    previewInstall.percent = 100;
    pollAutoInstall();
    if (!expect(installedApps[0] && !activateApp(0),
                "only a confirmed completed installation displays and records success")) return false;

    activateApp(1);
    completeDownload(1);
    pollAutoInstall();
    previewInstall.state = INSTALL_FAILED;
    previewInstall.cleanupCode = (int32_t)0x80990001;
    previewInstall.cleanupStage = INSTALL_STAGE_RESTORE;
    if (!expect(!activateApp(1) && !activateApp(2) && downloadCalls == 3 && installCalls == 4,
                "unsafe cleanup blocks both new downloads and saved-PKG retries")) return false;

    resetController();
    installingApp = 0;
    previewInstall.state = INSTALL_DONE;
    previewInstall.taskId = 7;
    downloadedBytes[1] = UI_APPS[1].sizeBytes;
    rejectInstallBusy = true;
    bool accepted = activateApp(1);
    pollAutoInstall();
    if (!expect(!accepted && installingApp == 0 && installedApps[0] && !installedApps[1],
                "a rejected start cannot attribute an older DONE snapshot to another app")) return false;
    if (!expect(downloadedBytes[1] == UI_APPS[1].sizeBytes && downloadCalls == 0,
                "a busy rejection preserves the downloaded PKG for an explicit retry")) return false;
    resetController();
    rejectInstall = true;
    downloadedBytes[0] = UI_APPS[0].sizeBytes;
    downloadedBytes[1] = UI_APPS[1].sizeBytes;
    activateApp(0);
    uint32_t firstGeneration = previewInstall.generation;
    activateApp(1);
    if (!expect(previewInstall.state == INSTALL_FAILED && installingApp == 1 &&
                previewInstall.generation != firstGeneration && !installedApps[1],
                "a new failed generation is associated with its own app even when startup returns false")) return false;
    musicToggleMute();
    musicNextTrack();
    if (!expect(previewMusic.muted && previewMusic.track == 1 && previewMusic.volume == 30,
                "sound and playlist controls preserve the default volume")) return false;
    char size[48];
    sizeLabel(size, sizeof(size), 22675456ULL);
    if (!expect(strcmp(size, "21.6 MB") == 0, "small package labels retain MB precision")) return false;
    sizeLabel(size, sizeof(size), 1024ULL * 1024 * 1024);
    if (!expect(strcmp(size, "1.0 GB") == 0, "one GiB switches the displayed unit")) return false;
    sizeLabel(size, sizeof(size), 5ULL * 1024 * 1024 * 1024);
    if (!expect(strcmp(size, "5.0 GB") == 0, "large package labels show GB")) return false;
    sizeLabel(size, sizeof(size), UINT64_MAX);
    if (!expect(strcmp(size, "17179869183.9 GB") == 0, "size labels do not overflow uint64")) return false;
    for (int i = 0; i < UI_APP_COUNT; ++i) {
        if (!UI_APPS[i].contentId[0]) continue;
        resetController();
        activateApp(i);
        if (!expect(lastDownloadContentId && strcmp(lastDownloadContentId, UI_APPS[i].contentId) == 0,
                    "a checked download receives its catalog Content ID")) return false;
        completeDownload(i);
        pollAutoInstall();
        if (!expect(installCalls == 1 && lastInstall.expectedBytes == UI_APPS[i].sizeBytes,
                    "a checked download keeps the exact size during install handoff")) return false;
        bool official = strncmp(UI_APPS[i].url, "https://github.com/", 19) == 0;
        if (!expect((strcmp(UI_APPS[i].sourceBadge, "PKG / FONTE OFICIAL") == 0) == official,
                    "checked packages display the badge for their actual source")) return false;
    }
    if (!verifyMeasurementUi() || !verifySearchController()) return false;
    resetController();
    puts("Checked automatic install handoff, retry, cancellation, cleanup, music, size labels and measured download speed.");
    return true;
}

static bool save(const char* path, const uint32_t* frame) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; ++i) {
        uint8_t rgb[3] = {(uint8_t)(frame[i] >> 16), (uint8_t)(frame[i] >> 8), (uint8_t)frame[i]};
        if (fwrite(rgb, 1, 3, f) != 3) { fclose(f); return false; }
    }
    return fclose(f) == 0;
}

static bool saveState(const char* prefix, const char* state, const uint32_t* frame) {
    char path[4096];
    int size = snprintf(path, sizeof(path), "%s-%s.ppm", prefix, state);
    return size > 0 && (size_t)size < sizeof(path) && save(path, frame);
}

static bool previewMeasurementStates(const char* prefix) {
    uint32_t* allocation = static_cast<uint32_t*>(malloc(((size_t)W * H + 2) * sizeof(uint32_t)));
    if (!allocation) return false;
    const uint32_t canary = 0xBAADF00D;
    allocation[0] = allocation[(size_t)W * H + 1] = canary;
    uint32_t* frame = allocation + 1;
    resetController();
    int selected = 0;
    for (int i = 0; i < UI_APP_COUNT; ++i) {
        if (UI_APPS[i].category == 3 && UI_APPS[i].sizeBytes >= 20ULL * 1024 * 1024 * 1024) {
            selected = i;
            break;
        }
    }
    downloadingApp = selected;
    previewDownload.state = RUNNING;
    previewDownload.total = UI_APPS[selected].sizeBytes;
    samplePreviewDownload(0, 0);
    drawDetails(frame, selected);
    bool okay = saveState(prefix, "download-measuring", frame);
    samplePreviewDownload(1000000, 1200000);
    samplePreviewDownload(2000000, 2400000);
    drawDetails(frame, selected);
    okay = saveState(prefix, "download-rate-eta", frame) && okay;
    samplePreviewDownload(5000000, 2400000);
    drawDetails(frame, selected);
    okay = saveState(prefix, "download-stalled", frame) && okay;
    samplePreviewDownload(6000000, 4800000);
    drawStore(frame, selected, 2);
    okay = saveState(prefix, "download-footer-rate", frame) && okay;
    samplePreviewDownload(6200000, previewDownload.total);
    drawDetails(frame, selected);
    okay = saveState(prefix, "download-finalizing", frame) && okay;
    okay = allocation[0] == canary && allocation[(size_t)W * H + 1] == canary && okay;
    free(allocation);
    resetController();
    return okay;
}

static bool previewSearchStates(const char* prefix) {
    uint32_t* allocation = static_cast<uint32_t*>(malloc(((size_t)W * H + 2) * sizeof(uint32_t)));
    if (!allocation) return false;
    const uint32_t canary = 0xBAADF00D;
    allocation[0] = allocation[(size_t)W * H + 1] = canary;
    uint32_t* frame = allocation + 1;
    resetController();
    drawStore(frame, 0, 2);
    bool okay = saveState(prefix, "search-home", frame);
    catalogSearch.begin();
    drawCatalogSearch(frame, 0);
    okay = saveState(prefix, "search-keyboard", frame) && okay;
    catalogSearch.key = CATALOG_SEARCH_KEY_COUNT - 1;
    snprintf(catalogSearch.draft, sizeof(catalogSearch.draft), "CUSA00001");
    drawCatalogSearch(frame, 0);
    okay = saveState(prefix, "search-keyboard-last-key", frame) && okay;
    catalogSearch.input(SEARCH_CANCEL);
    snprintf(catalogSearch.query, sizeof(catalogSearch.query), "Apollo");
    drawStore(frame, 0, 2);
    okay = saveState(prefix, "search-results", frame) && okay;
    snprintf(catalogSearch.query, sizeof(catalogSearch.query), "__NO_SUCH_PKG_74C6__");
    drawStore(frame, 0, 2);
    okay = saveState(prefix, "search-empty", frame) && okay;
    catalogSearch.query[0] = 0;
    activeCategory = 5;
    drawStore(frame, 0, 2);
    okay = saveState(prefix, "updates-empty", frame) && okay;
    activeCategory = 6;
    drawStore(frame, 0, 2);
    okay = saveState(prefix, "dlc-empty", frame) && okay;
    resetController();
    activateApp(0);
    samplePreviewDownload(1000000, 1200000);
    samplePreviewDownload(2000000, 2400000);
    catalogSearch.begin();
    drawCatalogSearch(frame, 0);
    okay = saveState(prefix, "search-during-download", frame) && okay;
    okay = allocation[0] == canary && allocation[(size_t)W * H + 1] == canary && okay;
    free(allocation);
    resetController();
    if (okay) puts("Rendered 8 bounded search and separate category states with framebuffer guards.");
    return okay;
}

static bool renderServiceStates(const char* prefix) {
    uint32_t* allocation = static_cast<uint32_t*>(malloc(((size_t)W * H + 2) * sizeof(uint32_t)));
    if (!allocation) return false;
    const uint32_t guard = 0xFEED4A31;
    allocation[0] = allocation[(size_t)W * H + 1] = guard;
    uint32_t* frame = allocation + 1;
    resetController(); bool okay = true;
    const int panels[] = {STORE_PANEL_SERVICES, STORE_PANEL_PREMIUM, STORE_PANEL_DONATE, STORE_PANEL_UPDATER, STORE_PANEL_TEXT, STORE_PANEL_ADMIN, STORE_PANEL_USERS, STORE_PANEL_ACCOUNT, STORE_PANEL_REVOKE};
    const char* names[] = {"services", "premium-login", "livepix-plans", "assets-updater", "url-keyboard", "admin", "admin-users", "admin-account", "admin-confirm"};
    storeAdminUsers = static_cast<HubAdminUser*>(calloc(3, sizeof(HubAdminUser))); storeAdminUserCount = 3;
    snprintf(storeAdminUsers[0].username, 65, "%s", "owner_account"); storeAdminUsers[0].admin = true;
    snprintf(storeAdminUsers[1].username, 65, "%s", "premium_fixture"); storeAdminUsers[1].premiumActive = true; snprintf(storeAdminUsers[1].plan, 4, "%s", "1m");
    snprintf(storeAdminUsers[2].username, 65, "%s", "invalidated_fixture"); storeAdminUsers[2].revoked = true;
    storeAdminUserSelection = 1; storeSelectedAdminUser = storeAdminUsers[1];
    for (int i = 0; i < 9; ++i) {
        storeOpenPanel(panels[i]);
        if (panels[i] == STORE_PANEL_TEXT) { storeBeginText(STORE_TEXT_URLS, STORE_PANEL_SERVICES); storeKeyboard.key = int(strlen(storeKeyboardKeys())) - 1; }
        drawStorePanel(frame); okay = saveState(prefix, names[i], frame) && okay;
    }
    okay = okay && allocation[0] == guard && allocation[(size_t)W * H + 1] == guard;
    free(allocation); resetController();
    return okay;
}

int main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--check-services") == 0) return verifyServicesController() ? 0 : 1;
    if (argc == 3 && strcmp(argv[1], "--preview-services") == 0) return renderServiceStates(argv[2]) ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--check-controller") == 0) return verifyController() ? 0 : 1;
    if (argc == 2 && strcmp(argv[1], "--check-search") == 0) return verifySearchController() ? 0 : 1;
    if (argc == 3 && strcmp(argv[1], "--preview-download-meter") == 0)
        return verifyController() && previewMeasurementStates(argv[2]) ? 0 : 1;
    if (argc == 3 && strcmp(argv[1], "--preview-search") == 0)
        return verifyController() && previewSearchStates(argv[2]) ? 0 : 1;
    if (argc != 2) { fprintf(stderr, "Usage: %s output-prefix | --check-controller | --check-search | --preview-download-meter prefix | --preview-search prefix\n", argv[0]); return 1; }
    if (!verifyController()) return 1;
    uint32_t* allocation = (uint32_t*)malloc(((size_t)W * H + 2) * sizeof(uint32_t));
    if (!allocation) return 1;
    allocation[0] = allocation[(size_t)W * H + 1] = 0xBAADF00D;
    uint32_t* frame = allocation + 1;
    int states = 0;
    // Bounded smoke rendering: at most four entries per category, even when
    // the verified catalog contains hundreds of games.
    for (activeCategory = 0; activeCategory < CATEGORY_COUNT; ++activeCategory) {
      const int count = categoryCount();
      if (!count) {
        char state[80];
        drawStore(frame, 0, 2);
        snprintf(state, sizeof(state), "home-%d-empty", activeCategory);
        if (!saveState(argv[1], state, frame)) { free(allocation); return 1; }
        ++states;
      }
      for (int selected = 0; selected < count && selected < 4; ++selected) {
        char state[80];
        drawStore(frame, selected, 2);
        snprintf(state, sizeof(state), "home-%d-%d", activeCategory, selected);
        if (!saveState(argv[1], state, frame)) { free(allocation); return 1; }
        drawDetails(frame, selected);
        snprintf(state, sizeof(state), "details-%d-%d", activeCategory, selected);
        if (!saveState(argv[1], state, frame)) { free(allocation); return 1; }
        states += 2;
      }
    }
    resetController();
    downloadingApp = 0;
    for (int state = RUNNING; state <= CANCELLED; ++state) {
        char name[80];
        previewDownload = {};
        previewDownload.state = state;
        autoInstallPending = state == DONE;
        previewDownload.received = UI_APPS[0].sizeBytes / 2;
        previewDownload.total = UI_APPS[0].sizeBytes;
        previewDownload.errorCode = DOWNLOAD_ERROR_NETWORK;
        previewDownload.stage = DOWNLOAD_STAGE_SEND;
        previewDownload.nativeCode = (int32_t)0x80431068;
        previewDownload.networkState = 3;
        drawDetails(frame, 0);
        snprintf(name, sizeof(name), "download-%d", state);
        if (!saveState(argv[1], name, frame)) { free(allocation); return 1; }
        ++states;
    }
    for (int state = INSTALL_RUNNING; state <= INSTALL_CANCELLED; ++state) {
        resetController();
        downloadingApp = installingApp = 0;
        downloadedBytes[0] = UI_APPS[0].sizeBytes;
        completeDownload(0);
        previewInstall.state = state;
        previewInstall.total = UI_APPS[0].sizeBytes;
        previewInstall.received = previewInstall.total / 2;
        previewInstall.percent = 42;
        previewInstall.taskId = 7;
        previewInstall.stage = state == INSTALL_FAILED ? INSTALL_STAGE_REGISTER : INSTALL_STAGE_PROGRESS;
        previewInstall.errorCode = state == INSTALL_FAILED ? INSTALL_ERROR_TASK : 0;
        previewInstall.nativeCode = state == INSTALL_FAILED ? (int32_t)0x80990015 : 0;
        if (state == INSTALL_DONE) previewInstall.percent = 100;
        char name[80];
        drawDetails(frame, 0);
        snprintf(name, sizeof(name), "install-%d", state);
        if (!saveState(argv[1], name, frame)) { free(allocation); return 1; }
        drawStore(frame, 0, 2);
        snprintf(name, sizeof(name), "install-home-%d", state);
        if (!saveState(argv[1], name, frame)) { free(allocation); return 1; }
        states += 2;
    }
    resetController();
    installingApp = downloadingApp = 0;
    downloadedBytes[0] = UI_APPS[0].sizeBytes;
    completeDownload(0);
    previewInstall.state = INSTALL_RUNNING;
    previewInstall.mode = INSTALL_MODE_HTTP_LOCAL;
    previewInstall.sdkVersion = 0xFFFFFFFFU;
    previewInstall.sdkErrno = 256;
    previewInstall.stage = INSTALL_STAGE_PROGRESS;
    previewInstall.taskId = 7;
    previewInstall.percent = 42;
    previewInstall.httpRequests = 3;
    previewInstall.httpStatus = 206;
    previewInstall.httpBytes = UI_APPS[0].sizeBytes / 2;
    drawDetails(frame, 0);
    if (!saveState(argv[1], "install-http-local", frame)) { free(allocation); return 1; }
    previewInstall.state = INSTALL_FAILED;
    previewInstall.errorCode = INSTALL_ERROR_PROGRESS;
    previewInstall.stage = INSTALL_STAGE_PROGRESS;
    previewInstall.nativeCode = (int32_t)0x80991404;
    previewInstall.httpStatus = 404;
    drawDetails(frame, 0);
    if (!saveState(argv[1], "install-http-native-failure", frame)) { free(allocation); return 1; }
    states += 2;
    previewInstall.state = INSTALL_RUNNING;
    drawDetails(frame, 1);
    if (!saveState(argv[1], "install-other-app", frame)) { free(allocation); return 1; }
    installCancelRequested = true;
    drawDetails(frame, 0);
    if (!saveState(argv[1], "install-cancelling", frame)) { free(allocation); return 1; }
    states += 2;
    installCancelRequested = false;
    previewInstall.state = INSTALL_FAILED;
    previewInstall.errorCode = INSTALL_ERROR_RESTORE;
    previewInstall.stage = previewInstall.cleanupStage = INSTALL_STAGE_RESTORE;
    previewInstall.nativeCode = previewInstall.cleanupCode = (int32_t)0x80990001;
    drawDetails(frame, 0);
    if (!saveState(argv[1], "install-reopen", frame)) { free(allocation); return 1; }
    drawDetails(frame, 1);
    if (!saveState(argv[1], "install-reopen-other-app", frame)) { free(allocation); return 1; }
    drawStore(frame, 0, 2);
    if (!saveState(argv[1], "install-reopen-home", frame)) { free(allocation); return 1; }
    states += 3;

    resetController();
    for (int state = MUSIC_STOPPED; state <= MUSIC_FAILED; ++state) {
        previewMusic.state = state;
        previewMusic.track = state == MUSIC_PLAYING ? 1 : 0;
        previewMusic.errorCode = state == MUSIC_FAILED ? (int32_t)0x80260001 : 0;
        char name[80];
        drawStore(frame, 0, 2);
        snprintf(name, sizeof(name), "music-%d", state);
        if (!saveState(argv[1], name, frame)) { free(allocation); return 1; }
        ++states;
    }
    previewMusic = {MUSIC_PLAYING, 0, 30, true, 0};
    drawStore(frame, 0, 2);
    if (!saveState(argv[1], "music-muted", frame)) { free(allocation); return 1; }
    ++states;
    bool guards = allocation[0] == 0xBAADF00D && allocation[(size_t)W * H + 1] == 0xBAADF00D;
    free(allocation);
    if (!expect(guards, "rendering preserves both framebuffer guards")) return 1;
    printf("Rendered %d catalog, download, install and music states.\n", states);
    return 0;
}
