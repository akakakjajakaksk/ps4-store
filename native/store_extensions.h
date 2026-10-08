#ifndef PEPPY_STORE_EXTENSIONS_H
#define PEPPY_STORE_EXTENSIONS_H

#include "user_catalog.h"
#include "hub_client.h"
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>

// Compiled entries keep their original indices. The two runtime segments are
// replaced only while download/install workers are idle; their borrowed string
// pointers and operation indices cannot silently switch to a different PKG.
static const int STORE_LOCAL_FIRST = UI_APP_COUNT;
static const int STORE_REMOTE_FIRST = UI_APP_COUNT + USER_CATALOG_MAX_ITEMS;
static const int STORE_CAPACITY = UI_APP_COUNT + 2 * USER_CATALOG_MAX_ITEMS;
static UserCatalog storeLocal;
static UserCatalog* storeRemote = 0;
static UiApp storeRuntimeViews[2 * USER_CATALOG_MAX_ITEMS];
static uint64_t storeCatalogVersion = 0;
static uint32_t storeViewRevision = 0;
static bool storeAdultConfirmed = false, storeFtpRequested = false;
static bool storePremiumCatalogRequested = false, storeLoginSyncPending = false;
static bool storeTransfersBusy();
static void storeStopPremiumDownload();
static bool storeUiEditing();
static bool storeSearchEditing();
static uint64_t downloadNowUs();
static void resetRemoteOperations();
static uint64_t storeNextSyncUs = 0;

enum StorePanel { STORE_PANEL_NONE, STORE_PANEL_SERVICES, STORE_PANEL_PREMIUM,
    STORE_PANEL_TEXT, STORE_PANEL_DONATE, STORE_PANEL_UPDATER, STORE_PANEL_ADMIN,
    STORE_PANEL_ADULT, STORE_PANEL_USERS, STORE_PANEL_REVOKE, STORE_PANEL_ACCOUNT };
enum StoreTextTarget { STORE_TEXT_URLS, STORE_TEXT_LOGIN_USER, STORE_TEXT_LOGIN_PASSWORD,
    STORE_TEXT_ADMIN_USER, STORE_TEXT_ADMIN_PASSWORD, STORE_TEXT_ACCOUNT_PASSWORD };
static int storePanel = STORE_PANEL_NONE, storeMenuSelection = 0;
static bool storeAdminLogin = false, storeAdminChord = false;
static bool storeHadSession = false, storeHadAdmin = false, storeHadEntitlement = false;
static HubAdminUser* storeAdminUsers = 0;
static size_t storeAdminUserCount = 0;
static int storeAdminUserSelection = 0;
static HubAdminUser storeSelectedAdminUser = {};
static char storeAccountPassword[129] = {};
static int storeAdminPlan = 15;
static char storeLoginUser[65] = {}, storeLoginPassword[129] = {};
static char storeSessionUserId[65] = {};
static char storeAdminUser[65] = {}, storeAdminPassword[129] = {};
static char storeNotice[512] = {};
static HubResult storePending = {};
static bool storeHasPending = false;

struct StoreKeyboard {
    char draft[8192];
    int key, target, returnPanel;
    size_t limit;
    bool masked;
    StoreKeyboard() : draft{}, key(0), target(0), returnPanel(0), limit(0), masked(false) {}
};
static StoreKeyboard storeKeyboard;
static const char* storeKeyboardKeys() {
    return "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:/?%=&_-.+@!$#[]";
}
static const int STORE_KEY_COLUMNS = 13;
static void storeWipe(char* value, size_t bytes) {
    volatile char* p = value;
    while (bytes--) *p++ = 0;
}
static void storeClearPasswords() {
    storeWipe(storeLoginPassword, sizeof(storeLoginPassword));
    storeWipe(storeAdminPassword, sizeof(storeAdminPassword));
    storeWipe(storeAccountPassword, sizeof(storeAccountPassword));
    if (storePanel == STORE_PANEL_TEXT && storeKeyboard.masked)
        storeWipe(storeKeyboard.draft, sizeof(storeKeyboard.draft));
}
static void storeClearAdminUsers() {
    free(storeAdminUsers); storeAdminUsers = 0; storeAdminUserCount = 0; storeAdminUserSelection = 0;
}
static void storeLogout() { startHubLogout(); storeClearPasswords(); storeAdultConfirmed = false; }
static void storeRestoreLoginFields() {
    char username[65] = {}, password[129] = {};
    if (hubSavedLoginCredentials(username, sizeof(username), password, sizeof(password))) {
        if (!storeLoginUser[0]) snprintf(storeLoginUser, sizeof(storeLoginUser), "%s", username);
        if (!strcmp(storeLoginUser, username) && !storeLoginPassword[0])
            snprintf(storeLoginPassword, sizeof(storeLoginPassword), "%s", password);
    }
    storeWipe(password, sizeof(password));
}
static void storeOpenPanel(int panel) {
    storePanel = panel; storeMenuSelection = 0;
    if (panel == STORE_PANEL_PREMIUM) storeRestoreLoginFields();
}
static void storeBeginText(int target, int returnPanel) {
    storeKeyboard = StoreKeyboard();
    storeKeyboard.target = target; storeKeyboard.returnPanel = returnPanel;
    storeKeyboard.limit = target == STORE_TEXT_URLS ? sizeof(storeKeyboard.draft) - 1 :
        (target == STORE_TEXT_LOGIN_USER || target == STORE_TEXT_ADMIN_USER ? 64 : 128);
    storeKeyboard.masked = target == STORE_TEXT_LOGIN_PASSWORD || target == STORE_TEXT_ADMIN_PASSWORD || target == STORE_TEXT_ACCOUNT_PASSWORD;
    const char* initial = target == STORE_TEXT_LOGIN_USER ? storeLoginUser :
        target == STORE_TEXT_LOGIN_PASSWORD ? storeLoginPassword :
        target == STORE_TEXT_ADMIN_USER ? storeAdminUser :
        target == STORE_TEXT_ADMIN_PASSWORD ? storeAdminPassword : target == STORE_TEXT_ACCOUNT_PASSWORD ? storeAccountPassword : "";
    snprintf(storeKeyboard.draft, sizeof(storeKeyboard.draft), "%s", initial);
    storePanel = STORE_PANEL_TEXT;
}
static const UserCatalogEntry* storeEntry(int index) {
    if (index >= STORE_REMOTE_FIRST && storeRemote)
        return storeRemote->at(size_t(index - STORE_REMOTE_FIRST));
    if (index >= STORE_LOCAL_FIRST && index < STORE_REMOTE_FIRST)
        return storeLocal.at(size_t(index - STORE_LOCAL_FIRST));
    return 0;
}
static bool storeAppExists(int index) {
    return index >= 0 && (index < UI_APP_COUNT || storeEntry(index));
}
static bool storeIsRemote(int index) { return index >= STORE_REMOTE_FIRST && storeEntry(index); }
static bool storeAppAllowed(int index) {
    if (!storeAppExists(index)) return false;
    if (!storeIsRemote(index)) return true;
    HubSession session = hubSession();
    const UserCatalogEntry* entry = storeEntry(index);
    return session.authenticated && (session.premium || session.admin) && (!entry->adult || storeAdultConfirmed);
}
static const UiApp& storeApp(int index) {
    return index < UI_APP_COUNT ? UI_APPS[index] : storeRuntimeViews[index - STORE_LOCAL_FIRST];
}
static int storeAppKind(int index) { const UserCatalogEntry* e = storeEntry(index); return e ? e->kind : USER_PACKAGE_BASE; }
static int storeDisplayCategory(const UserCatalogEntry& entry, bool remote) {
    if (!remote) return 8 + entry.kind;
    if (entry.adult) return 12;
    if (entry.isTheme) return 7;
    if (entry.displayCategory >= 1 && entry.displayCategory <= 7) return entry.displayCategory;
    return entry.kind == USER_PACKAGE_UPDATE ? 5 : entry.kind == USER_PACKAGE_DLC ? 6 : 3;
}
static void storeRebuildViews() {
    for (int segment = 0; segment < 2; ++segment) {
        const UserCatalog* catalog = segment ? storeRemote : &storeLocal;
        for (size_t i = 0; catalog && i < catalog->count(); ++i) {
            const UserCatalogEntry& e = *catalog->at(i);
            UiApp& app = storeRuntimeViews[segment * USER_CATALOG_MAX_ITEMS + i];
            app = {e.titleId, e.name, userCatalogKindName(e.kind),
                e.description[0] ? e.description : "Pacote identificado pelo cabeçalho. Confira os dados da fonte antes de instalar.",
                e.versionKnown ? e.version : "Versão não informada", "Fonte cadastrada",
                "Compatibilidade: testar no console", e.url, e.url, e.filename, e.sha256, e.requiresData,
                segment ? "PKG / CATÁLOGO PREMIUM" :
                    (e.source == USER_SOURCE_RECOGNIZED_AUTHOR_RELEASE ? "PKG / RELEASE DO AUTOR" : "PKG / LINK DO USUÁRIO"),
                e.contentId, e.sizeBytes, storeDisplayCategory(e, segment != 0), 3};
        }
    }
    ++storeViewRevision;
}
static bool storeMatchesCategory(int index, int category) {
    if (!storeAppAllowed(index)) return false;
    if (category == 11) return storeIsRemote(index);
    if (category == 7) { const UserCatalogEntry* e = storeEntry(index); return e && e->isTheme; }
    return !category || storeApp(index).category == category;
}
static void storeClearRemote() {
    delete storeRemote; storeRemote = 0;
    storeAdultConfirmed = false; resetRemoteOperations();
    ++storeViewRevision;
}

// Exact links and prices remain readable in both the free and premium UI.
static const char STORE_LIVEPIX[] = "https://livepix.gg/peppystore";
static const char STORE_DISCORD[] = "djdarknes.com_66953";
static const char* storeUpdaterUrl() { return PEPPY_HUB_URL; }

struct StoreJson {
    char* data; size_t used, cap; bool okay;
    StoreJson() : data(static_cast<char*>(malloc(4 * 1024 * 1024))), used(0), cap(4 * 1024 * 1024), okay(data != 0) { if (data) data[0] = 0; }
    ~StoreJson() { free(data); }
    void raw(const char* text) {
        size_t n = strlen(text);
        if (!okay || n >= cap - used) { okay = false; return; }
        memcpy(data + used, text, n + 1); used += n;
    }
    void string(const char* text) {
        raw("\"");
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); okay && *p; ++p) {
            char small[8] = {};
            if (*p == '"' || *p == '\\') { small[0] = '\\'; small[1] = char(*p); }
            else if (*p < 32) snprintf(small, sizeof(small), "\\u%04x", unsigned(*p));
            else small[0] = char(*p);
            raw(small);
        }
        raw("\"");
    }
    void number(uint64_t value) { char b[32]; snprintf(b, sizeof(b), "%llu", (unsigned long long)value); raw(b); }
};
static bool storePublishLocal() {
    if (!hubSession().admin || !storeLocal.count()) { snprintf(storeNotice, sizeof(storeNotice), "Importe os PKGs que deseja publicar antes de enviar o catálogo."); return false; }
    StoreJson json; json.raw("{\"replace\":false,\"entries\":[");
    for (size_t i = 0; i < storeLocal.count(); ++i) {
        const UserCatalogEntry& e = *storeLocal.at(i);
        if (i) json.raw(",");
        json.raw("{\"id\":"); json.string(e.filename);
        json.raw(",\"name\":"); json.string(e.name);
        json.raw(",\"url\":"); json.string(e.url);
        json.raw(",\"filename\":"); json.string(e.filename);
        json.raw(",\"version\":"); json.string(e.version);
        json.raw(",\"size\":"); json.number(e.sizeBytes);
        json.raw(",\"content_id\":"); json.string(e.contentId);
        json.raw(",\"content_type\":"); json.number(e.contentType);
        json.raw(",\"content_flags\":"); json.number(e.contentFlags);
        json.raw(",\"iro_tag\":"); json.number(e.iroTag);
        json.raw(",\"display_category\":"); json.number(e.displayCategory);
        json.raw(",\"kind\":"); json.string(e.isTheme ? "theme" : e.displayCategory == 4 ? "media" : e.kind == USER_PACKAGE_UPDATE ? "update" : e.kind == USER_PACKAGE_DLC ? "dlc" : "base");
        json.raw(",\"platform\":\"ps4\",\"language\":[],\"description\":"); json.string(e.description);
        json.raw(",\"requires_data\":"); json.string(e.requiresData);
        json.raw(",\"sha256\":"); json.string(e.sha256);
        json.raw(e.isTheme ? ",\"is_theme\":true" : ",\"is_theme\":false");
        json.raw(e.adult ? ",\"adult\":true}" : ",\"adult\":false}");
    }
    json.raw("]}");
    if (!json.okay) { snprintf(storeNotice, sizeof(storeNotice), "O catálogo excede o limite de envio. Reduza a lista e tente novamente."); return false; }
    return startHubAdminPublish(json.data, json.used);
}
static bool storeStartImportFile() {
    char* list = 0; size_t bytes = 0;
    int result = userCatalogReadUrlListFile("/data/peppy-store/urls.txt", &list, &bytes);
    if (result) {
        if (result == USER_CATALOG_ERROR_FILE || result == USER_CATALOG_ERROR_NOT_FOUND)
            snprintf(storeNotice, sizeof(storeNotice), "Não foi possível ler /data/peppy-store/urls.txt. Copie a lista de links para esse arquivo e tente novamente.");
        else snprintf(storeNotice, sizeof(storeNotice), "%s", userCatalogErrorMessage(result));
        return false;
    }
    bool started = startHubImportUrls(list, bytes);
    free(list);
    return started;
}
static bool storeSubmitText() {
    int target = storeKeyboard.target, returnPanel = storeKeyboard.returnPanel;
    bool started = true;
    if (target == STORE_TEXT_URLS) started = startHubImportUrls(storeKeyboard.draft, strlen(storeKeyboard.draft));
    else {
        if (target == STORE_TEXT_LOGIN_USER && strcmp(storeLoginUser, storeKeyboard.draft))
            storeWipe(storeLoginPassword, sizeof(storeLoginPassword));
        char* out = target == STORE_TEXT_LOGIN_USER ? storeLoginUser : target == STORE_TEXT_LOGIN_PASSWORD ? storeLoginPassword :
            target == STORE_TEXT_ADMIN_USER ? storeAdminUser : target == STORE_TEXT_ACCOUNT_PASSWORD ? storeAccountPassword : storeAdminPassword;
        size_t cap = target == STORE_TEXT_LOGIN_USER || target == STORE_TEXT_ADMIN_USER ? 65 : 129;
        snprintf(out, cap, "%.*s", int(cap - 1), storeKeyboard.draft);
    }
    storeWipe(storeKeyboard.draft, sizeof(storeKeyboard.draft));
    storePanel = returnPanel;
    return started;
}
static bool storePanelController(uint32_t pressed, uint32_t held) {
    const uint32_t secret = CATALOG_R2 | CATALOG_R3 | CATALOG_OPTIONS;
    bool chord = (held & secret) == secret;
    if (!chord) storeAdminChord = false;
    if (storePanel == STORE_PANEL_PREMIUM && chord && !storeAdminChord) {
        storeAdminChord = true; storeAdminLogin = true;
        if (hubSession().admin) storeOpenPanel(STORE_PANEL_ADMIN);
        else { storeMenuSelection = 0; snprintf(storeNotice, sizeof(storeNotice), "Entre com uma conta administradora."); }
        return true;
    }
    if (storePanel == STORE_PANEL_NONE) return false;
    if (!pressed) return false;
    if (storePanel == STORE_PANEL_TEXT) {
        if (pressed & CATALOG_CIRCLE) { storeWipe(storeKeyboard.draft, sizeof(storeKeyboard.draft)); storePanel = storeKeyboard.returnPanel; return true; }
        if (pressed & CATALOG_OPTIONS) { storeSubmitText(); return true; }
        int count = int(strlen(storeKeyboardKeys()));
        if (pressed & CATALOG_LEFT) storeKeyboard.key = (storeKeyboard.key + count - 1) % count;
        if (pressed & CATALOG_RIGHT) storeKeyboard.key = (storeKeyboard.key + 1) % count;
        if (pressed & CATALOG_UP) storeKeyboard.key = (storeKeyboard.key + count - STORE_KEY_COLUMNS) % count;
        if (pressed & CATALOG_DOWN) storeKeyboard.key = (storeKeyboard.key + STORE_KEY_COLUMNS) % count;
        size_t n = strlen(storeKeyboard.draft);
        if ((pressed & CATALOG_SQUARE) && n) storeKeyboard.draft[--n] = 0;
        char add = pressed & CATALOG_CROSS ? storeKeyboardKeys()[storeKeyboard.key] :
            pressed & CATALOG_TRIANGLE ? ' ' : pressed & CATALOG_L1 && storeKeyboard.target == STORE_TEXT_URLS ? '\n' : 0;
        if (add && n < storeKeyboard.limit) { storeKeyboard.draft[n] = add; storeKeyboard.draft[n + 1] = 0; }
        return true;
    }
    if (pressed & CATALOG_CIRCLE) {
        if (storePanel == STORE_PANEL_REVOKE) { storeOpenPanel(STORE_PANEL_ACCOUNT); return true; }
        if (storePanel == STORE_PANEL_ACCOUNT) { storeClearPasswords(); storeOpenPanel(STORE_PANEL_USERS); return true; }
        if (storePanel == STORE_PANEL_USERS) { storeOpenPanel(STORE_PANEL_ADMIN); return true; }
        storeClearPasswords();
        if (storePanel == STORE_PANEL_PREMIUM) {
            storeWipe(storeLoginPassword, sizeof(storeLoginPassword)); storeAdminLogin = false;
        }
        storeOpenPanel(STORE_PANEL_NONE); return true;
    }
    if (storePanel == STORE_PANEL_USERS || storePanel == STORE_PANEL_REVOKE || storePanel == STORE_PANEL_ACCOUNT) {
        if (!hubSession().admin) return true;
        if (storePanel == STORE_PANEL_USERS) {
            if (pressed & CATALOG_SQUARE) startHubAdminListUsers();
            int users = int(storeAdminUserCount);
            if (users && (pressed & CATALOG_UP)) storeAdminUserSelection = (storeAdminUserSelection + users - 1) % users;
            if (users && (pressed & CATALOG_DOWN)) storeAdminUserSelection = (storeAdminUserSelection + 1) % users;
            if (users && (pressed & CATALOG_CROSS)) {
                if (storeAdminUsers[storeAdminUserSelection].admin)
                    snprintf(storeNotice, sizeof(storeNotice), "A conta administradora não pode ser invalidada aqui.");
                else {
                    storeSelectedAdminUser = storeAdminUsers[storeAdminUserSelection];
                    storeWipe(storeAccountPassword, sizeof(storeAccountPassword));
                    storeOpenPanel(STORE_PANEL_ACCOUNT);
                }
            }
        } else if (storePanel == STORE_PANEL_ACCOUNT) {
            if (pressed & CATALOG_UP) storeMenuSelection = (storeMenuSelection + 2) % 3;
            if (pressed & CATALOG_DOWN) storeMenuSelection = (storeMenuSelection + 1) % 3;
            if (pressed & CATALOG_CROSS) {
                if (storeMenuSelection == 0) storeOpenPanel(STORE_PANEL_REVOKE);
                else if (storeMenuSelection == 1) storeBeginText(STORE_TEXT_ACCOUNT_PASSWORD, STORE_PANEL_ACCOUNT);
                else {
                    bool started = startHubAdminChangePassword(storeSelectedAdminUser.id, storeAccountPassword);
                    storeWipe(storeAccountPassword, sizeof(storeAccountPassword));
                    if (started) storeOpenPanel(STORE_PANEL_USERS);
                    else snprintf(storeNotice, sizeof(storeNotice), "Informe uma senha de pelo menos 8 caracteres e tente novamente.");
                }
            }
        } else if (pressed & CATALOG_CROSS) {
            if (!storeSelectedAdminUser.admin && startHubAdminRevokeUser(storeSelectedAdminUser.id, !storeSelectedAdminUser.revoked))
                storeOpenPanel(STORE_PANEL_USERS);
        }
        return true;
    }
    if ((pressed & CATALOG_TRIANGLE) && hubSnapshot().state == HUB_RUNNING) { cancelHubOperation(); return true; }
    if (storePanel == STORE_PANEL_ADULT) {
        if (pressed & CATALOG_CROSS) { storeAdultConfirmed = true; storeOpenPanel(STORE_PANEL_NONE); }
        return true;
    }
    int count = storePanel == STORE_PANEL_SERVICES ? 8 : storePanel == STORE_PANEL_PREMIUM ? 5 : storePanel == STORE_PANEL_ADMIN ? 7 : 0;
    if (!count) return true;
    if (pressed & CATALOG_UP) storeMenuSelection = (storeMenuSelection + count - 1) % count;
    if (pressed & CATALOG_DOWN) storeMenuSelection = (storeMenuSelection + 1) % count;
    if (storePanel == STORE_PANEL_ADMIN && storeMenuSelection == 2 && (pressed & (CATALOG_LEFT | CATALOG_RIGHT))) {
        storeAdminPlan = storeAdminPlan == 15 ? 30 : storeAdminPlan == 30 ? 60 : 15;
    }
    if (!(pressed & CATALOG_CROSS)) return true;
    if (storePanel == STORE_PANEL_SERVICES) {
        switch (storeMenuSelection) {
        case 0: storeBeginText(STORE_TEXT_URLS, STORE_PANEL_SERVICES); break;
        case 1: storeStartImportFile(); break;
        case 2: storeAdminLogin = false; storeOpenPanel(STORE_PANEL_PREMIUM); break;
        case 3: storeOpenPanel(STORE_PANEL_DONATE); break;
        case 4: startHubSync(); break;
        case 5: storeOpenPanel(STORE_PANEL_UPDATER); break;
        case 6: storeFtpRequested = true; storeOpenPanel(STORE_PANEL_NONE); break;
        case 7: storeLogout(); break;
        }
    } else if (storePanel == STORE_PANEL_PREMIUM) {
        if (storeMenuSelection == 0) storeBeginText(STORE_TEXT_LOGIN_USER, STORE_PANEL_PREMIUM);
        else if (storeMenuSelection == 1) storeBeginText(STORE_TEXT_LOGIN_PASSWORD, STORE_PANEL_PREMIUM);
        else if (storeMenuSelection == 2) {
            startHubLogin(storeLoginUser, storeLoginPassword); storeWipe(storeLoginPassword, sizeof(storeLoginPassword));
        } else if (storeMenuSelection == 3) storeOpenPanel(STORE_PANEL_DONATE);
        else storeLogout();
    } else if (storePanel == STORE_PANEL_ADMIN) {
        if (!hubSession().admin) { snprintf(storeNotice, sizeof(storeNotice), "Acesso administrativo necessário."); return true; }
        if (storeMenuSelection == 0) storeBeginText(STORE_TEXT_ADMIN_USER, STORE_PANEL_ADMIN);
        else if (storeMenuSelection == 1) storeBeginText(STORE_TEXT_ADMIN_PASSWORD, STORE_PANEL_ADMIN);
        else if (storeMenuSelection == 2) storeAdminPlan = storeAdminPlan == 15 ? 30 : storeAdminPlan == 30 ? 60 : 15;
        else if (storeMenuSelection == 3) {
            startHubAdminCreateUser(storeAdminUser, storeAdminPassword, storeAdminPlan);
            storeWipe(storeAdminPassword, sizeof(storeAdminPassword));
        } else if (storeMenuSelection == 4) storePublishLocal();
        else if (storeMenuSelection == 5) startHubSync();
        else { storeOpenPanel(STORE_PANEL_USERS); startHubAdminListUsers(); }
    }
    return true;
}
static bool pollStoreExtensions() {
    bool changed = false;
    pollHubSession();
    HubSession session = hubSession();
    if (session.authenticated && strcmp(storeSessionUserId, session.userId)) {
        storeAdultConfirmed = false;
        snprintf(storeSessionUserId, sizeof(storeSessionUserId), "%s", session.userId);
        ++storeViewRevision; changed = true;
    } else if (!session.authenticated) storeSessionUserId[0] = 0;
    if (storeHadSession && !session.authenticated) {
        storeClearPasswords();
        storeAdultConfirmed = false; storeLoginSyncPending = false;
        snprintf(storeNotice, sizeof(storeNotice), "A sessão foi encerrada ou invalidada. Entre novamente.");
        changed = true;
    }
    if (storeHadAdmin && !session.admin) {
        storeWipe(storeAdminPassword, sizeof(storeAdminPassword)); storeClearAdminUsers();
        storeSelectedAdminUser = {}; storeWipe(storeAccountPassword, sizeof(storeAccountPassword));
    }
    storeHadSession = session.authenticated; storeHadAdmin = session.admin;
    bool entitled = session.authenticated && (session.premium || session.admin);
    if (storeHadEntitlement != entitled) {
        ++storeViewRevision; changed = true;
        if (!entitled) storeStopPremiumDownload();
        storeHadEntitlement = entitled;
    }
    uint64_t now = downloadNowUs();
    if (!entitled) storeNextSyncUs = 0;
    else if (!storeNextSyncUs) storeNextSyncUs = now + 300000000ULL;
    else if (now >= storeNextSyncUs && hubConfigured() && !storeHasPending &&
             hubSnapshot().state == HUB_IDLE && !storeTransfersBusy() && !storeUiEditing()) {
        if (startHubSync()) { storeNextSyncUs = now + 300000000ULL; changed = true; }
    }
    if ((storePanel == STORE_PANEL_ADMIN || storePanel == STORE_PANEL_USERS || storePanel == STORE_PANEL_REVOKE || storePanel == STORE_PANEL_ACCOUNT) && !session.admin) {
        storeWipe(storeAdminPassword, sizeof(storeAdminPassword));
        storeAdminLogin = true; storeOpenPanel(STORE_PANEL_PREMIUM); changed = true;
    }
    if (storeRemote && !(session.authenticated && (session.premium || session.admin)) && !storeTransfersBusy()) {
        storeClearRemote(); changed = true;
    }
    if (!storeHasPending && consumeHubResult(&storePending)) storeHasPending = true;
    if (!storeHasPending || storePanel == STORE_PANEL_TEXT ||
        (storePending.catalog && (storeTransfersBusy() || storeSearchEditing()))) return changed;
    int operation = storePending.operation, error = storePending.errorCode;
    if (operation == HUB_SYNC && error) storeLoginSyncPending = false;
    if (operation == HUB_IMPORT_URLS && storePending.catalog) {
        size_t added = 0, duplicates = storePending.imports.duplicates, rejected = storePending.imports.rejected;
        for (size_t i = 0; i < storePending.catalog->count(); ++i) {
            const UserCatalogEntry& e = *storePending.catalog->at(i);
            bool inFreeCatalog = false;
            for (int f = 0; e.kind == USER_PACKAGE_BASE && f < UI_APP_COUNT; ++f)
                if (UI_APPS[f].contentId[0] && !strcmp(UI_APPS[f].contentId, e.contentId)) { inFreeCatalog = true; break; }
            int result = inFreeCatalog ? USER_CATALOG_ERROR_DUPLICATE : storeLocal.add(e);
            if (result == USER_CATALOG_OK) ++added;
            else if (result == USER_CATALOG_ERROR_DUPLICATE) ++duplicates;
            else ++rejected;
        }
        int saved = storeLocal.save();
        snprintf(storeNotice, sizeof(storeNotice), "%u adicionados | %u duplicados | %u recusados%s",
            unsigned(added), unsigned(duplicates), unsigned(rejected), saved ? " | Não foi possível salvar a lista local." : "");
        if (storePending.imports.firstError || error) {
            size_t used = strlen(storeNotice);
            const char* message = error ? hubErrorMessage(error) : userCatalogErrorMessage(storePending.imports.firstError);
            snprintf(storeNotice + used, sizeof(storeNotice) - used, " | %s", message);
        }
        storeRebuildViews();
    } else if (operation == HUB_ADMIN_LIST_USERS && !error && session.admin) {
        int selectedUser = storeAdminUserSelection;
        storeClearAdminUsers(); storeAdminUsers = storePending.users; storePending.users = 0;
        storeAdminUserCount = storePending.userCount;
        storeAdminUserSelection = selectedUser >= 0 && selectedUser < int(storeAdminUserCount) ? selectedUser : 0;
        snprintf(storeNotice, sizeof(storeNotice), "%u contas. Selecione uma para invalidar ou reativar.", unsigned(storeAdminUserCount));
    } else if (operation == HUB_SYNC && !error && storePending.catalog && session.authenticated && (session.premium || session.admin)) {
        bool adultConfirmed = storeAdultConfirmed;
        storeClearRemote(); storeRemote = storePending.catalog; storePending.catalog = 0;
        storeAdultConfirmed = adultConfirmed;
        storeCatalogVersion = storePending.catalogVersion; storeRebuildViews();
        snprintf(storeNotice, sizeof(storeNotice), "Catálogo premium atualizado: %u itens.", unsigned(storeRemote->count()));
        if (storeLoginSyncPending && !(storeAdminLogin && session.admin)) {
            storeOpenPanel(STORE_PANEL_NONE); storePremiumCatalogRequested = true;
        }
        storeLoginSyncPending = false;
    } else if (operation == HUB_SYNC && !error && !(session.premium || session.admin))
        snprintf(storeNotice, sizeof(storeNotice), "A sessão premium foi encerrada. Entre novamente para sincronizar.");
    else if (error) snprintf(storeNotice, sizeof(storeNotice), "%s (código %d)", hubErrorMessage(error), error);
    else if (operation == HUB_LOGIN) snprintf(storeNotice, sizeof(storeNotice), hubSavedLoginStatus().storageError ?
        "Conta autenticada; não foi possível salvar o login neste PS4." : "Conta autenticada. Login salvo neste PS4.");
    else if (operation == HUB_ADMIN_CREATE_USER) snprintf(storeNotice, sizeof(storeNotice), "Conta premium criada. Envie as credenciais ao usuário.");
    else if (operation == HUB_ADMIN_CHANGE_PASSWORD) snprintf(storeNotice, sizeof(storeNotice), "Senha alterada. As sessões anteriores foram encerradas.");
    else if (operation == HUB_ADMIN_REVOKE_USER) snprintf(storeNotice, sizeof(storeNotice), "Acesso alterado no servidor; sessões anteriores foram invalidadas.");
    else if (operation == HUB_ADMIN_PUBLISH) snprintf(storeNotice, sizeof(storeNotice), "Catálogo publicado. As lojas premium podem sincronizar.");
    else if (operation == HUB_LOGOUT) snprintf(storeNotice, sizeof(storeNotice), hubSavedLoginStatus().storageError ?
        "Sessão encerrada; não foi possível apagar o login salvo." : "Sessão encerrada. Login salvo apagado.");
    freeHubResult(&storePending); storePending = {}; storeHasPending = false; changed = true;
    if (!error && (operation == HUB_ADMIN_REVOKE_USER || operation == HUB_ADMIN_CHANGE_PASSWORD) && session.admin) startHubAdminListUsers();
    if (!error && operation == HUB_LOGIN) {
        session = hubSession();
        if (storeAdminLogin && session.admin) storeOpenPanel(STORE_PANEL_ADMIN);
        else if (storeAdminLogin) snprintf(storeNotice, sizeof(storeNotice), "Esta conta não tem acesso administrativo.");
        if (session.premium || session.admin) storeLoginSyncPending = startHubSync();
    }
    return changed;
}

#endif
