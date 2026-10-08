#ifndef PEPPY_UI_PREVIEW
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/VideoOut.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "ui_assets.h"
#include "ui_catalog.h"
#include "catalog_search.h"
#include "downloads.h"
#include "download_meter.h"
#include "install.h"
#include "music.h"
#include "peppy_hub_config.h"
#ifndef PEPPY_UI_PREVIEW
#include "ftp_receiver.h"
#endif

static const int W = 1920, H = 1080;
static const uint32_t BG = 0x800B0F17, PANEL = 0x80131925;
static const uint32_t WHITE = 0x80F4F6FA, MUTED = 0x809BA7BA;
static const uint32_t BLUE = 0x8073B7FF, LINE = 0x80252D3C;
static const int CATEGORY_COUNT = 13;
static const char* CATEGORY_NAMES[CATEGORY_COUNT] = {"Todos", "Utilitários", "Emuladores", "Jogos", "Streaming / mídia", "Atualizações", "DLCs", "Temas", "Base do usuário", "Update do usuário", "DLC do usuário", "Premium", "+18 premium"};
static const uint32_t CATEGORY_COLORS[CATEGORY_COUNT] = {BLUE, 0x807BDECC, 0x80B9A2F9, 0x80EBC48A, 0x8085BBFB, BLUE, BLUE, BLUE, BLUE, BLUE, BLUE, BLUE, 0x80EBA5B4};
enum CatalogControllerButton {
    CATALOG_LEFT = 1U << 0, CATALOG_RIGHT = 1U << 1,
    CATALOG_UP = 1U << 2, CATALOG_DOWN = 1U << 3,
    CATALOG_CROSS = 1U << 4, CATALOG_CIRCLE = 1U << 5,
    CATALOG_SQUARE = 1U << 6, CATALOG_TRIANGLE = 1U << 7,
    CATALOG_L1 = 1U << 8, CATALOG_R1 = 1U << 9,
    CATALOG_L3 = 1U << 10, CATALOG_R3 = 1U << 11,
    CATALOG_OPTIONS = 1U << 12, CATALOG_R2 = 1U << 13
};
#include "store_extensions.h"

static int activeCategory = 0, downloadingApp = -1, installingApp = -1;
static CatalogSearchState catalogSearch;
static void applyPremiumCatalogRequest(int& selected, bool& details) {
    if (!storePremiumCatalogRequested) return;
    storePremiumCatalogRequested = false;
    activeCategory = 11; selected = 0; details = false;
    catalogSearch.query[0] = 0; catalogSearch.open = false;
}
static bool autoInstallPending = false, installCancelRequested = false;
static uint64_t downloadedBytes[STORE_CAPACITY] = {};
static bool installedApps[STORE_CAPACITY] = {};
static DownloadMeter downloadMeter;

#ifndef PEPPY_UI_PREVIEW
static const int FTP_INBOX_CAPACITY = 128;
static FtpInboxItem ftpInboxItems[FTP_INBOX_CAPACITY];
static int ftpInboxCount = 0;
static int ftpInboxSelected = 0;
static bool ftpInboxOpen = false;
static uint32_t ftpInboxInstallGeneration = 0;
static char ftpInboxInstallName[192] = {};
static int ftpInboxInstallKind = -1;

static bool refreshFtpInbox() {
    int previousCount = ftpInboxCount;
    ftpInboxCount = ftpInboxList(ftpInboxItems, FTP_INBOX_CAPACITY);
    if (ftpInboxCount < 0) ftpInboxCount = 0;
    if (ftpInboxSelected >= ftpInboxCount)
        ftpInboxSelected = ftpInboxCount ? ftpInboxCount - 1 : 0;
    if (ftpInboxSelected < 0) ftpInboxSelected = 0;
    return ftpInboxCount != previousCount;
}
#endif

static uint64_t downloadNowUs() {
#ifdef PEPPY_UI_PREVIEW
    return previewNowUs();
#else
    return sceKernelGetProcessTime();
#endif
}

static bool validApp(int index) { return storeAppExists(index); }
static bool storeTransfersBusy() {
    return downloadSnapshot().state == RUNNING || installSnapshot().state == INSTALL_RUNNING || autoInstallPending;
}
static void storeStopPremiumDownload() {
    if (downloadingApp >= STORE_REMOTE_FIRST) {
        autoInstallPending = false;
        if (downloadSnapshot().state == RUNNING) cancelDownload();
    }
}
static bool storeUiEditing() { return storePanel != STORE_PANEL_NONE || catalogSearch.open; }
static bool storeSearchEditing() { return catalogSearch.open; }
static void resetRemoteOperations() {
    memset(downloadedBytes + STORE_REMOTE_FIRST, 0, USER_CATALOG_MAX_ITEMS * sizeof(downloadedBytes[0]));
    memset(installedApps + STORE_REMOTE_FIRST, 0, USER_CATALOG_MAX_ITEMS * sizeof(installedApps[0]));
    if (downloadingApp >= STORE_REMOTE_FIRST) downloadingApp = -1;
    if (installingApp >= STORE_REMOTE_FIRST) installingApp = -1;
}

static bool beginInstall(int index) {
    InstallSnapshot status = installSnapshot();
    if (!validApp(index) || !storeAppAllowed(index) || !downloadedBytes[index] || status.state == INSTALL_RUNNING || status.cleanupCode)
        return false;
    const UiApp& app = storeApp(index);
    InstallSpec spec = {app.filename, app.name, downloadedBytes[index]};
    bool accepted = index < UI_APP_COUNT ? startInstall(spec) : startTypedInstall(spec, storeAppKind(index));
    InstallSnapshot after = installSnapshot();
    bool newAttempt = after.generation != status.generation;
    // A busy worker may reject a start after publishing its previous DONE.
    // Keep that snapshot associated with its original app unless accepted.
    if (accepted || newAttempt) {
        installingApp = index;
        installCancelRequested = false;
    }
    return accepted;
}

// Consume a completed download before starting installation. Failure never
// re-arms this flag; an install retry requires an explicit X press.
static bool pollAutoInstall() {
    bool changed = false;
    DownloadSnapshot download = downloadSnapshot();
    InstallSnapshot install = installSnapshot();
    if (validApp(installingApp) && install.state == INSTALL_DONE && !installedApps[installingApp]) {
        installedApps[installingApp] = true;
        changed = true;
    }
    if (autoInstallPending && download.state == DONE && install.state != INSTALL_RUNNING) {
        autoInstallPending = false;
        if (validApp(downloadingApp) && download.received == storeApp(downloadingApp).sizeBytes) {
            downloadedBytes[downloadingApp] = download.received;
            beginInstall(downloadingApp);
        }
        changed = true;
    } else if (autoInstallPending && (download.state == FAILED || download.state == CANCELLED)) {
        autoInstallPending = false;
        changed = true;
    }
    install = installSnapshot();
    if (install.state != INSTALL_RUNNING && installCancelRequested) {
        installCancelRequested = false;
        changed = true;
    }
    return changed;
}

static bool activateApp(int index) {
    InstallSnapshot status = installSnapshot();
    if (!validApp(index) || !storeAppAllowed(index) || downloadSnapshot().state == RUNNING ||
        status.state == INSTALL_RUNNING || status.cleanupCode || installedApps[index]) return false;
    if (downloadedBytes[index]) return beginInstall(index);
    const UiApp& app = storeApp(index);
    DownloadSpec spec = {app.url, app.filename, app.sizeBytes, app.sha256};
    downloadingApp = index;
    autoInstallPending = true;
    uint64_t startedAt = downloadNowUs();
    if (!startDownload(spec, app.contentId, storeAppKind(index))) autoInstallPending = false;
    else {
        downloadMeter.reset();
        downloadMeter.update(true, 0, app.sizeBytes, startedAt);
    }
    return true;
}

static bool cancelOperation(int selectedIndex) {
    if (downloadSnapshot().state == RUNNING) { cancelDownload(); return true; }
    if (installSnapshot().state == INSTALL_RUNNING && selectedIndex == installingApp) {
        cancelInstall();
        installCancelRequested = true;
        return true;
    }
    return false;
}

static bool matchesCatalogFilter(int index) {
    const UiApp& app = storeApp(index);
    return storeMatchesCategory(index, activeCategory) &&
        catalogSearchMatches(app.name, app.id, app.contentId, catalogSearch.query);
}

static int categoryCount() {
    int count = 0;
    for (int i = 0; i < STORE_CAPACITY; ++i)
        if (storeAppExists(i) && matchesCatalogFilter(i)) ++count;
    return count;
}

static int appIndex(int selected) {
    for (int i = 0; i < STORE_CAPACITY; ++i) {
        if (storeAppExists(i) && matchesCatalogFilter(i)) {
            if (selected-- == 0) return i;
        }
    }
    return -1;
}

// Controller actions are shared by the console loop and host checks. The
// caller supplies press edges, so holding X never fills the query repeatedly.


static bool handleCatalogController(uint32_t pressed, int& selected, bool& details, uint32_t held = 0) {
    if (storePanel != STORE_PANEL_NONE) return storePanelController(pressed, held);
    if (!catalogSearch.open && !details && (pressed & CATALOG_OPTIONS)) {
        storeOpenPanel(STORE_PANEL_SERVICES); return true;
    }
    if (!details && activeCategory == 11 && (pressed & CATALOG_CROSS) && !(hubSession().premium || hubSession().admin)) {
        storeAdminLogin = false; storeOpenPanel(STORE_PANEL_PREMIUM); return true;
    }
    if (!details && activeCategory == 12 && (pressed & CATALOG_CROSS)) {
        if (!(hubSession().premium || hubSession().admin)) { storeAdminLogin = false; storeOpenPanel(STORE_PANEL_PREMIUM); return true; }
        if (!storeAdultConfirmed) { storeOpenPanel(STORE_PANEL_ADULT); return true; }
    }
    int count = categoryCount();
    if (selected < 0 || selected >= count) selected = 0;
    if (!count) details = false;
    if (catalogSearch.open) {
        // Closing consumes the entire press, including simultaneous buttons.
        if (pressed & CATALOG_CIRCLE) return catalogSearch.input(SEARCH_CANCEL);
        if (pressed & CATALOG_OPTIONS) {
            bool changed = catalogSearch.input(SEARCH_APPLY);
            selected = 0;
            details = false;
            return changed;
        }
        bool changed = false;
        if (pressed & CATALOG_LEFT) changed = catalogSearch.input(SEARCH_LEFT) || changed;
        if (pressed & CATALOG_RIGHT) changed = catalogSearch.input(SEARCH_RIGHT) || changed;
        if (pressed & CATALOG_UP) changed = catalogSearch.input(SEARCH_UP) || changed;
        if (pressed & CATALOG_DOWN) changed = catalogSearch.input(SEARCH_DOWN) || changed;
        if (pressed & CATALOG_SQUARE) changed = catalogSearch.input(SEARCH_ERASE) || changed;
        if (pressed & CATALOG_TRIANGLE) changed = catalogSearch.input(SEARCH_SPACE) || changed;
        if (pressed & CATALOG_CROSS) changed = catalogSearch.input(SEARCH_CHARACTER) || changed;
        return changed;
    }
    if (!details && (pressed & CATALOG_R3)) { catalogSearch.begin(); return true; }
    bool changed = false;
    if (!details && (pressed & CATALOG_R1)) {
        activeCategory = (activeCategory + 1) % CATEGORY_COUNT;
        selected = 0;
        changed = true;
    }
    if (!details && (pressed & CATALOG_L1)) {
        activeCategory = (activeCategory + CATEGORY_COUNT - 1) % CATEGORY_COUNT;
        selected = 0;
        changed = true;
    }
    count = categoryCount();
    if (!details && count && (pressed & CATALOG_RIGHT)) { selected = (selected + 1) % count; changed = true; }
    if (!details && count && (pressed & CATALOG_LEFT)) { selected = (selected + count - 1) % count; changed = true; }
    if (count && (pressed & CATALOG_CROSS)) {
        if (!details) details = true;
        else activateApp(appIndex(selected));
        changed = true;
    }
    if (details && (pressed & CATALOG_CIRCLE)) { details = false; changed = true; }
    if (pressed & CATALOG_TRIANGLE) changed = cancelOperation(appIndex(selected)) || changed;
    if (pressed & CATALOG_SQUARE) { musicToggleMute(); changed = true; }
    if (pressed & CATALOG_L3) { musicNextTrack(); changed = true; }
    return changed;
}

#ifndef PEPPY_UI_PREVIEW
static bool beginFtpInboxInstall() {
    if (ftpInboxSelected < 0 || ftpInboxSelected >= ftpInboxCount) return false;

    InstallSnapshot before = installSnapshot();
    if (before.state == INSTALL_RUNNING || before.cleanupCode) return false;

    const FtpInboxItem& item = ftpInboxItems[ftpInboxSelected];
    bool accepted = startInboxInstall(item.path, item.name, item.bytes, item.kind);
    InstallSnapshot after = installSnapshot();

    if (accepted || after.generation != before.generation) {
        ftpInboxInstallGeneration = after.generation;
        ftpInboxInstallKind = item.kind;
        snprintf(ftpInboxInstallName, sizeof(ftpInboxInstallName), "%s", item.name);
        installingApp = -1;
        autoInstallPending = false;
        installCancelRequested = false;
    }
    return accepted;
}

static bool handleFtpInboxController(uint32_t pressed) {
    bool changed = false;

    if (pressed & CATALOG_CIRCLE) {
        ftpInboxOpen = false;
        return true;
    }

    if (pressed & CATALOG_SQUARE) {
        if (!ftpReceiverSnapshot().running) ftpReceiverStart();
        refreshFtpInbox();
        changed = true;
    }

    if (ftpInboxCount > 0) {
        if (pressed & CATALOG_UP) {
            ftpInboxSelected = (ftpInboxSelected + ftpInboxCount - 1) % ftpInboxCount;
            changed = true;
        }
        if (pressed & CATALOG_DOWN) {
            ftpInboxSelected = (ftpInboxSelected + 1) % ftpInboxCount;
            changed = true;
        }
        if (pressed & CATALOG_LEFT) {
            ftpInboxSelected = (ftpInboxSelected + ftpInboxCount - 8) % ftpInboxCount;
            if (ftpInboxSelected < 0) ftpInboxSelected += ftpInboxCount;
            changed = true;
        }
        if (pressed & CATALOG_RIGHT) {
            ftpInboxSelected = (ftpInboxSelected + 8) % ftpInboxCount;
            changed = true;
        }
        if (pressed & CATALOG_CROSS) {
            changed = beginFtpInboxInstall() || changed;
        }
    }

    InstallSnapshot install = installSnapshot();
    bool mine = ftpInboxInstallGeneration &&
                install.generation == ftpInboxInstallGeneration;
    if ((pressed & CATALOG_TRIANGLE) && mine && install.state == INSTALL_RUNNING) {
        cancelInstall();
        installCancelRequested = true;
        changed = true;
    }

    if (pressed & CATALOG_L3) {
        musicNextTrack();
        changed = true;
    }

    return changed;
}
#endif

static uint32_t mix(uint32_t a, uint32_t b, int t) {
    if (t <= 0) return a;
    if (t >= 255) return b;
    uint32_t out = 0x80000000;
    for (int shift = 0; shift <= 16; shift += 8)
        out |= ((((a >> shift) & 255) * (255 - t) + ((b >> shift) & 255) * t) / 255) << shift;
    return out;
}

static void pixel(uint32_t* p, int x, int y, uint32_t color, int alpha = 255) {
    if (x < 0 || y < 0 || x >= W || y >= H || alpha <= 0) return;
    p[y * W + x] = mix(p[y * W + x], color, alpha);
}

static void rect(uint32_t* p, int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx) p[yy * W + xx] = color;
}

static int roundCoverage(int x, int y, int w, int h, int r) {
    if (x < 0 || y < 0 || x >= w || y >= h) return 0;
    if (r <= 0) return 255;
    int dx = x < r ? 2 * (r - x) - 1 : (x >= w - r ? 2 * (x - w + r) + 1 : 0);
    int dy = y < r ? 2 * (r - y) - 1 : (y >= h - r ? 2 * (y - h + r) + 1 : 0);
    if (!dx || !dy) return 255;
    int d = dx * dx + dy * dy;
    int inner = (2 * r - 1) * (2 * r - 1), outer = (2 * r + 1) * (2 * r + 1);
    if (d <= inner) return 255;
    if (d >= outer) return 0;
    return 255 * (outer - d) / (outer - inner);
}

static void roundRect(uint32_t* p, int x, int y, int w, int h, int r, uint32_t c, int alpha = 255) {
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx)
            pixel(p, x + xx, y + yy, c, roundCoverage(xx, yy, w, h, r) * alpha / 255);
}

static void outline(uint32_t* p, int x, int y, int w, int h, int r, int thickness, uint32_t c) {
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            if (xx >= r && xx < w - r && yy >= thickness && yy < h - thickness) continue;
            int outer = roundCoverage(xx, yy, w, h, r);
            int inner = roundCoverage(xx - thickness, yy - thickness, w - 2 * thickness,
                                      h - 2 * thickness, r - thickness);
            pixel(p, x + xx, y + yy, c, outer > inner ? outer - inner : 0);
        }
    }
}

static void gradient(uint32_t* p, int x, int y, int w, int h, int r, uint32_t left, uint32_t right) {
    for (int xx = 0; xx < w; ++xx) {
        uint32_t c = mix(left, right, w > 1 ? xx * 255 / (w - 1) : 0);
        for (int yy = 0; yy < h; ++yy)
            pixel(p, x + xx, y + yy, c, roundCoverage(xx, yy, w, h, r));
    }
}

static void line(uint32_t* p, float x1, float y1, float x2, float y2, float thickness, uint32_t c) {
    float radius = thickness * 0.5f, inner = radius > 0.5f ? radius - 0.5f : 0.0f;
    float outer = radius + 0.5f, vx = x2 - x1, vy = y2 - y1, length2 = vx * vx + vy * vy;
    int left = (int)(x1 < x2 ? x1 : x2) - (int)outer - 1;
    int top = (int)(y1 < y2 ? y1 : y2) - (int)outer - 1;
    int right = (int)(x1 > x2 ? x1 : x2) + (int)outer + 1;
    int bottom = (int)(y1 > y2 ? y1 : y2) + (int)outer + 1;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right >= W) right = W - 1;
    if (bottom >= H) bottom = H - 1;
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) {
            float dx = x + 0.5f - x1, dy = y + 0.5f - y1;
            float t = length2 > 0 ? (dx * vx + dy * vy) / length2 : 0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            dx -= t * vx; dy -= t * vy;
            float distance2 = dx * dx + dy * dy;
            if (distance2 < outer * outer) {
                int alpha = distance2 <= inner * inner ? 255 :
                    (int)(255 * (outer * outer - distance2) / (outer * outer - inner * inner));
                pixel(p, x, y, c, alpha);
            }
        }
    }
}

static void ring(uint32_t* p, int cx, int cy, int radius, int thickness, uint32_t c) {
    int inner = radius - thickness;
    for (int y = -radius - 1; y <= radius + 1; ++y) {
        for (int x = -radius - 1; x <= radius + 1; ++x) {
            int d = x * x + y * y;
            int outer = (radius + 1) * (radius + 1), full = radius * radius;
            int hole = inner * inner, edge = (inner + 1) * (inner + 1);
            int alpha = d <= full ? 255 : (d < outer ? 255 * (outer - d) / (outer - full) : 0);
            if (d <= hole) alpha = 0;
            else if (d < edge) alpha = alpha * (d - hole) / (edge - hole);
            pixel(p, cx + x, cy + y, c, alpha);
        }
    }
}

static uint32_t nextCodepoint(const char*& s) {
    uint32_t c = (uint8_t)*s++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && ((uint8_t)*s & 0xC0) == 0x80)
        return ((c & 31) << 6) | ((uint8_t)*s++ & 63);
    return '?';
}

static const UiGlyph* glyph(const UiFont& font, uint32_t c) {
    for (int i = 0; i < font.count; ++i) if (font.glyphs[i].code == c) return font.glyphs + i;
    for (int i = 0; i < font.count; ++i) if (font.glyphs[i].code == '?') return font.glyphs + i;
    return 0;
}

static int textWidth(const char* s, const UiFont& font) {
    int width = 0;
    while (*s) { const UiGlyph* g = glyph(font, nextCodepoint(s)); if (g) width += g->advance; }
    return width;
}

static void text(uint32_t* p, int x, int y, const char* s, const UiFont& font, uint32_t c) {
    int baseline = y + font.ascent;
    while (*s) {
        const UiGlyph* g = glyph(font, nextCodepoint(s));
        if (!g) continue;
        for (int yy = 0; yy < g->height; ++yy)
            for (int xx = 0; xx < g->width; ++xx)
                pixel(p, x + g->left + xx, baseline + g->top + yy, c,
                      font.pixels[g->offset + yy * g->width + xx]);
        x += g->advance;
    }
}

static void pill(uint32_t* p, int x, int y, const char* label, uint32_t bg, uint32_t fg) {
    int w = textWidth(label, FONT_SMALL) + 32;
    roundRect(p, x, y, w, 38, 19, bg);
    text(p, x + 16, y + 4, label, FONT_SMALL, fg);
}

static void artwork(uint32_t* p, int x, int y, int w, int h, int r, bool fade = false) {
    int srcW = ui_peppy_width, srcH = ui_peppy_height, startX = 0, startY = 0;
    if (w > h) { srcH = ui_peppy_height * h / w; startY = (ui_peppy_height - srcH) / 2; }
    else { srcW = ui_peppy_width * w / h; startX = (ui_peppy_width - srcW) / 2; }
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            uint32_t c = 0x80000000 | ui_peppy_pixels[(startY + yy * srcH / h) * ui_peppy_width + startX + xx * srcW / w];
            if (fade) {
                c = mix(c, 0x80121F34, 20);
                if (xx < 165) c = mix(c, 0x80121F34, (165 - xx) * 255 / 165);
                if (yy > h - 90) c = mix(c, BG, (yy - h + 90) * 130 / 90);
            }
            pixel(p, x + xx, y + yy, c, roundCoverage(xx, yy, w, h, r));
        }
    }
}

static void appIcon(uint32_t* p, int x, int y, int size, int id, uint32_t c) {
    int u = size / 8;
    int t = size > 120 ? 6 : 3;
    if (id == 0) {
        outline(p, x + u, y + u, size - 2 * u, size - 2 * u, u / 2, t, c);
        outline(p, x + 2 * u, y + u, 4 * u, 2 * u, 2, t, c);
        outline(p, x + 2 * u, y + 4 * u, 4 * u, 3 * u, 2, t, c);
        line(p, x + 3 * u, y + 5 * u, x + 5 * u, y + 5 * u, t, c);
    } else if (id == 1) {
        outline(p, x + u, y + 2 * u, 6 * u, 4 * u, u, t, c);
        line(p, x + 2 * u, y + 4 * u, x + 4 * u, y + 4 * u, t, c);
        line(p, x + 3 * u, y + 3 * u, x + 3 * u, y + 5 * u, t, c);
        roundRect(p, x + 5 * u, y + 3 * u, t + 2, t + 2, 2, c);
        roundRect(p, x + 6 * u, y + 4 * u, t + 2, t + 2, 2, c);
    } else if (id == 2) {
        line(p, x + u, y + 2 * u, x + 3 * u, y + 2 * u, t, c);
        line(p, x + 3 * u, y + 2 * u, x + 4 * u, y + 3 * u, t, c);
        line(p, x + 4 * u, y + 3 * u, x + 7 * u, y + 3 * u, t, c);
        line(p, x + 7 * u, y + 3 * u, x + 7 * u, y + 6 * u, t, c);
        line(p, x + 7 * u, y + 6 * u, x + u, y + 6 * u, t, c);
        line(p, x + u, y + 6 * u, x + u, y + 2 * u, t, c);
    } else {
        for (int yy = 0; yy < 2; ++yy)
            for (int xx = 0; xx < 2; ++xx)
                outline(p, x + u + xx * 4 * u, y + u + yy * 4 * u, 2 * u, 2 * u, u / 2, t, c);
    }
}

static void crossButton(uint32_t* p, int x, int y) {
    ring(p, x, y, 17, 2, MUTED);
    line(p, x - 6, y - 6, x + 6, y + 6, 2, BLUE);
    line(p, x + 6, y - 6, x - 6, y + 6, 2, BLUE);
}

static void header(uint32_t* p) {
    rect(p, 0, 0, W, H, BG);
    artwork(p, 72, 48, 60, 60, 14);
    text(p, 150, 45, "PEPPY", FONT_TITLE, WHITE);
    text(p, 152, 84, "S T O R E", FONT_SMALL, MUTED);
    int tabX = 420;
    int firstTab = activeCategory / 6 * 6;
    for (int i = firstTab; i < CATEGORY_COUNT && i < firstTab + 6; ++i) {
        int width = textWidth(CATEGORY_NAMES[i], FONT_BODY);
        text(p, tabX, 67, CATEGORY_NAMES[i], FONT_BODY, i == activeCategory ? WHITE : MUTED);
        if (i == activeCategory) roundRect(p, tabX, 119, width, 3, 1, BLUE);
        tabX += width + 26;
    }
    char label[64];
    snprintf(label, sizeof(label), "L1 / R1  |  %d/%d", activeCategory / 6 + 1, (CATEGORY_COUNT + 5) / 6);
    pill(p, W - 72 - textWidth(label, FONT_SMALL) - 32, 61, label, PANEL, MUTED);
    rect(p, 72, 130, W - 144, 1, LINE);
}

static void textElided(uint32_t* p, int x, int y, const char* s, const UiFont& font,
                       uint32_t color, int maxWidth) {
    if (textWidth(s, font) <= maxWidth) { text(p, x, y, s, font, color); return; }
    char label[256];
    size_t used = 0;
    int width = 0, dots = textWidth("...", font);
    while (*s) {
        const char* begin = s;
        const UiGlyph* g = glyph(font, nextCodepoint(s));
        size_t bytes = (size_t)(s - begin);
        if (!g || width + g->advance + dots > maxWidth || used + bytes + 4 >= sizeof(label)) break;
        memcpy(label + used, begin, bytes); used += bytes; width += g->advance;
    }
    memcpy(label + used, "...", 4);
    text(p, x, y, label, font, color);
}

static void textWrapped(uint32_t* p, int x, int y, const char* s, const UiFont& font,
                        uint32_t color, int maxWidth, int maxLines) {
    char label[512];
    int lineNumber = 0;
    while (*s && lineNumber < maxLines) {
        while (*s == ' ') ++s;
        size_t used = 0, lastSpace = 0;
        const char* start = s;
        int width = 0;
        while (*s) {
            const char* begin = s;
            const UiGlyph* g = glyph(font, nextCodepoint(s));
            size_t bytes = (size_t)(s - begin);
            if (!g || width + g->advance > maxWidth || used + bytes + 1 >= sizeof(label)) {
                s = begin;
                break;
            }
            if (*begin == ' ') lastSpace = used;
            memcpy(label + used, begin, bytes); used += bytes; width += g->advance;
        }
        if (*s && lastSpace) { used = lastSpace; s = start + lastSpace + 1; }
        if (!used) return;
        label[used] = 0;
        if (*s && lineNumber + 1 == maxLines)
            textElided(p, x, y + lineNumber * font.lineHeight, start, font, color, maxWidth);
        else text(p, x, y + lineNumber * font.lineHeight, label, font, color);
        ++lineNumber;
    }
}

static void sizeLabel(char* label, size_t capacity, uint64_t bytes) {
    const uint64_t gib = 1024ULL * 1024 * 1024;
    const uint64_t unit = bytes >= gib ? gib : 1024ULL * 1024;
    const char* suffix = bytes >= gib ? "GB" : "MB";
    uint64_t whole = bytes / unit;
    uint64_t fraction = (bytes % unit) * 10 / unit;
    snprintf(label, capacity, "%llu.%llu %s", (unsigned long long)whole,
             (unsigned long long)fraction, suffix);
}

static int transferPercent(uint64_t received, uint64_t total) {
    if (!total) return 0;
    if (received >= total) return 100;
    return (int)((double)received * 100.0 / (double)total);
}

static void transferMeasurementLabel(char* label, size_t capacity,
                                     const DownloadSnapshot& status) {
    if (status.total && status.received >= status.total) {
        snprintf(label, capacity, "Finalizando o download...");
        return;
    }
    DownloadMeasurement meter = downloadMeter.measurement();
    if (!meter.rateAvailable) {
        snprintf(label, capacity, "Medindo a velocidade...");
        return;
    }
    // MB/s is decimal, matching network speed units. The measured counter
    // follows received bytes; completion still waits for queued writes and hash.
    double mbps = meter.bytesPerSecond / 1000000.0;
    if (!meter.etaAvailable) {
        snprintf(label, capacity, "%.2f MB/s | %s", mbps,
                 meter.bytesPerSecond > 0.0 ? "Calculando tempo restante..." : "Aguardando dados...");
    } else if (meter.remainingSeconds >= 3600) {
        snprintf(label, capacity, "%.2f MB/s | Estimativa: %llu h %llu min", mbps,
                 (unsigned long long)(meter.remainingSeconds / 3600),
                 (unsigned long long)(meter.remainingSeconds % 3600 / 60));
    } else if (meter.remainingSeconds >= 60) {
        snprintf(label, capacity, "%.2f MB/s | Estimativa: %llu min %llu s", mbps,
                 (unsigned long long)(meter.remainingSeconds / 60),
                 (unsigned long long)(meter.remainingSeconds % 60));
    } else {
        snprintf(label, capacity, "%.2f MB/s | Estimativa: %llu s", mbps,
                 (unsigned long long)meter.remainingSeconds);
    }
}

static int installPercent(const InstallSnapshot& status) {
    return status.percent < 0 ? 0 : (status.percent > 100 ? 100 : status.percent);
}

static void triangleButton(uint32_t* p, int x, int y) {
    line(p, x - 9, y + 7, x, y - 8, 2, 0x808BD4B0);
    line(p, x, y - 8, x + 9, y + 7, 2, 0x808BD4B0);
    line(p, x - 9, y + 7, x + 9, y + 7, 2, 0x808BD4B0);
}

static void footer(uint32_t* p, bool details, int selectedIndex) {
    DownloadSnapshot download = downloadSnapshot();
    InstallSnapshot install = installSnapshot();
    MusicSnapshot music = musicSnapshot();
    rect(p, 72, 978, W - 144, 1, LINE);
    if (details) {
        ring(p, 86, 1006, 10, 2, 0x80EBA5B4);
        text(p, 112, 992, "Biblioteca", FONT_SMALL, WHITE);
    } else {
        line(p, 80, 1006, 87, 999, 2, MUTED);
        line(p, 80, 1006, 87, 1013, 2, MUTED);
        line(p, 108, 1006, 101, 999, 2, MUTED);
        line(p, 108, 1006, 101, 1013, 2, MUTED);
        text(p, 122, 992, "Navegar", FONT_SMALL, MUTED);
        if (validApp(selectedIndex)) {
            crossButton(p, 260, 1006);
            text(p, 288, 992, "Ver detalhes", FONT_SMALL, WHITE);
        }
        text(p, 472, 992, "L1 / R1  Categorias", FONT_SMALL, MUTED);
    }
    bool canCancel = download.state == RUNNING ||
        (install.state == INSTALL_RUNNING && selectedIndex == installingApp);
    if (canCancel) {
        int x = details ? 285 : 750;
        triangleButton(p, x, 1006);
        text(p, x + 24, 992, "Cancelar", FONT_SMALL, MUTED);
    }
    char label[256];
    uint32_t statusColor = MUTED;
    if (install.cleanupCode) {
        snprintf(label, sizeof(label), "Feche e reabra a Peppy Store  |  limpeza 0x%08X", (unsigned)install.cleanupCode);
        statusColor = 0x80EBA5B4;
    } else if (install.state == INSTALL_RUNNING && validApp(installingApp)) {
        snprintf(label, sizeof(label), "%s %s  |  %s  |  %d%%",
                 installCancelRequested ? "Cancelando:" : "Instalando:", storeApp(installingApp).name,
                 installStageName(install.stage), installPercent(install));
        statusColor = BLUE;
    } else if (download.state == RUNNING && validApp(downloadingApp)) {
        DownloadMeasurement meter = downloadMeter.measurement();
        if (meter.rateAvailable && (!download.total || download.received < download.total))
            snprintf(label, sizeof(label), "Baixando: %d%% | %.2f MB/s | %d conexão%s | %s",
                     transferPercent(download.received, download.total), meter.bytesPerSecond / 1000000.0,
                     download.connections > 0 ? download.connections : 1, download.connections > 1 ? "s" : "",
                     storeApp(downloadingApp).name);
        else snprintf(label, sizeof(label), "Baixando: %s | %d%%", storeApp(downloadingApp).name,
                      transferPercent(download.received, download.total));
        statusColor = BLUE;
    } else if (install.state == INSTALL_FAILED && validApp(installingApp)) {
        snprintf(label, sizeof(label), "Falha ao instalar: %s  |  0x%08X",
                 storeApp(installingApp).name, (unsigned)install.nativeCode);
        statusColor = 0x80EBA5B4;
    } else if (install.state == INSTALL_DONE && validApp(installingApp)) {
        snprintf(label, sizeof(label), "Instalado no PS4: %s", storeApp(installingApp).name);
        statusColor = 0x807BDECC;
    } else snprintf(label, sizeof(label), "Feito para o seu PS4.");
    textElided(p, 1040, 992, label, FONT_SMALL, statusColor, 808);

    if (music.state == MUSIC_FAILED)
        snprintf(label, sizeof(label), "Música indisponível: 0x%08X", (unsigned)music.errorCode);
    else if (music.state == MUSIC_STARTING) snprintf(label, sizeof(label), "Preparando a música...");
    else if (music.state == MUSIC_PLAYING) snprintf(label, sizeof(label), "Tocando: %s", musicTrackName(music.track));
    else snprintf(label, sizeof(label), "Música parada");
    textElided(p, 72, 1030, label, FONT_SMALL, music.state == MUSIC_FAILED ? MUTED : WHITE, 420);
    outline(p, 508, 1031, 22, 22, 3, 2, MUTED);
    if (music.muted) snprintf(label, sizeof(label), "Som: mudo");
    else snprintf(label, sizeof(label), "Som: %d%%", music.volume);
    text(p, 545, 1030, label, FONT_SMALL, MUTED);
    ring(p, 747, 1044, 18, 2, MUTED);
    text(p, 733, 1030, "L3", FONT_SMALL, MUTED);
    text(p, 781, 1030, "Próxima faixa", FONT_SMALL, MUTED);
    if (!details) {
        ring(p, 1111, 1044, 18, 2, MUTED);
        text(p, 1096, 1030, "R3", FONT_SMALL, MUTED);
        text(p, 1145, 1030, "Buscar", FONT_SMALL, WHITE);
#ifndef PEPPY_UI_PREVIEW
        text(p, 1325, 1030, "OPTIONS  Serviços", FONT_SMALL, WHITE);
#endif
    }
}

static const char* installErrorText(int code) {
    switch (code) {
    case INSTALL_ERROR_SPEC: return "Os dados de instalação são inválidos.";
    case INSTALL_ERROR_THREAD: return "Não foi possível iniciar a instalação.";
    case INSTALL_ERROR_FILE: return "Não foi possível abrir o PKG baixado.";
    case INSTALL_ERROR_PACKAGE: return "O arquivo salvo não passou na validação de PKG.";
    case INSTALL_ERROR_ALREADY_INSTALLED: return "Esse app já está instalado no PS4.";
    case INSTALL_ERROR_BUSY: return "O serviço de instalação está ocupado.";
    case INSTALL_ERROR_SELF: return "Esse pacote aponta para a própria Peppy Store.";
    case INSTALL_ERROR_TIMEOUT: return "O serviço de instalação excedeu o tempo de espera.";
    case INSTALL_ERROR_SDK: return "O suporte de instalação não ficou disponível.";
    case INSTALL_ERROR_JAILBREAK: return "Não foi possível preparar o acesso para instalar.";
    case INSTALL_ERROR_GLOBAL_PATH: return "O PS4 não encontrou o PKG no caminho de instalação.";
    case INSTALL_ERROR_COPY: return "Não foi possível preparar o PKG para a instalação.";
    case INSTALL_ERROR_RESTORE: return "Não foi possível encerrar a preparação de instalação.";
    case INSTALL_ERROR_SERVER: return "Não foi possível entregar o PKG ao instalador.";
    case INSTALL_ERROR_USER: return "Não foi possível identificar o usuário do PS4 para instalar.";
    case INSTALL_ERROR_BASE_REQUIRED: return "Instale primeiro o jogo/base correspondente a este update ou DLC.";
    default: return "A instalação não foi concluída.";
    }
}

static const char* downloadErrorText(int code) {
    switch (code) {
    case DOWNLOAD_ERROR_SPEC: return "Os dados do pacote são inválidos.";
    case DOWNLOAD_ERROR_THREAD: return "Não foi possível iniciar o download.";
    case DOWNLOAD_ERROR_NETWORK: return "Não foi possível estabelecer a conexão.";
    case DOWNLOAD_ERROR_NOT_READY: return "O PS4 ainda não obteve uma conexão de rede para baixar.";
    case DOWNLOAD_ERROR_RESPONSE_HEADERS: return "A resposta do servidor excedeu o limite permitido.";
    case DOWNLOAD_ERROR_HTTP: return "O servidor não disponibilizou o pacote.";
    case DOWNLOAD_ERROR_REDIRECT: return "O link do pacote foi recusado.";
    case DOWNLOAD_ERROR_TLS: return "Falha na conexão segura. Confira a data e a hora do PS4.";
    case DOWNLOAD_ERROR_FILESYSTEM: return "Não foi possível salvar. Confira o espaço disponível.";
    case DOWNLOAD_ERROR_LENGTH: return "Download incompleto. Tente novamente.";
    case DOWNLOAD_ERROR_PACKAGE: return "O arquivo recebido não é um PKG.";
    case DOWNLOAD_ERROR_HASH: return "O pacote não confere com o hash publicado.";
    case DOWNLOAD_ERROR_SOURCE: return "Não foi possível obter o link do PKG nesta fonte. Tente novamente.";
    default: return "O download não foi concluído. Tente novamente.";
    }
}

static void drawStore(uint32_t* p, int selected, int padState) {
    (void)padState;
    header(p);
    gradient(p, 72, 164, 1776, 380, 24, 0x80192540, 0x80121F34);
    artwork(p, 1288, 164, 560, 380, 24, true);
    pill(p, 112, 201, "BEM-VINDO À PEPPY", 0x8023334E, BLUE);
    text(p, 108, 252, "Seu PS4.", FONT_HERO, WHITE);
    text(p, 108, 332, "Do seu jeito.", FONT_HERO, WHITE);
    text(p, 112, 444, "Homebrews e ferramentas em um só lugar.", FONT_BODY, 0x80B6C4DB);
    pill(p, 1604, 482, "PEPPY ORIGINAL", 0x80111B2B, WHITE);
    roundRect(p, 112, 486, 1040, 42, 12, 0x8010192A);
    text(p, 130, 492, "R3  Buscar", FONT_SMALL, BLUE);
    textElided(p, 280, 492, catalogSearch.query[0] ? catalogSearch.query : "Nome, ID do jogo ou Content ID",
               FONT_SMALL, catalogSearch.query[0] ? WHITE : MUTED, 850);

    int count = categoryCount(), first = selected / 4 * 4;
    text(p, 72, 578, activeCategory ? CATEGORY_NAMES[activeCategory] : "Explore sua biblioteca", FONT_TITLE, WHITE);
    char pageLabel[80];
    snprintf(pageLabel, sizeof(pageLabel), "%d %s  |  Página %d/%d", count, count == 1 ? "item" : "itens", count ? selected / 4 + 1 : 0, (count + 3) / 4);
    text(p, W - 72 - textWidth(pageLabel, FONT_SMALL), 587, pageLabel, FONT_SMALL, MUTED);
    const int start = 72, y = 650, cw = 426, ch = 294, gap = 24;
    for (int slot = 0; slot < 4 && first + slot < count; ++slot) {
        int index = appIndex(first + slot);
        if (index < 0) continue;
        const UiApp& app = storeApp(index);
        uint32_t accent = CATEGORY_COLORS[app.category];
        int x = start + slot * (cw + gap);
        bool focus = selected == first + slot;
        if (focus) roundRect(p, x - 5, y - 5, cw + 10, ch + 10, 25, 0x80243A58);
        roundRect(p, x, y, cw, ch, 20, focus ? 0x801B283B : PANEL);
        outline(p, x, y, cw, ch, 20, focus ? 2 : 1, focus ? BLUE : LINE);
        roundRect(p, x + 26, y + 27, 104, 104, 24, mix(PANEL, accent, 22));
        if (index >= UI_APP_COUNT) artwork(p, x + 42, y + 43, 72, 72, 16);
        else appIcon(p, x + 42, y + 43, 72, app.art, accent);
        text(p, x + 152, y + 53, CATEGORY_NAMES[app.category], FONT_SMALL, accent);
        char label[80];
        sizeLabel(label, sizeof(label), app.sizeBytes);
        text(p, x + 152, y + 88, label, FONT_SMALL, MUTED);
        textElided(p, x + 26, y + 157, app.name, FONT_CARD, WHITE, cw - 52);
        textElided(p, x + 26, y + 203, app.caption, FONT_SMALL, MUTED, cw - 52);
        text(p, x + 26, y + 254, "Ver detalhes", FONT_SMALL, focus ? BLUE : MUTED);
        line(p, x + cw - 51, y + 268, x + cw - 33, y + 268, 2, focus ? BLUE : MUTED);
        line(p, x + cw - 40, y + 261, x + cw - 33, y + 268, 2, focus ? BLUE : MUTED);
        line(p, x + cw - 40, y + 275, x + cw - 33, y + 268, 2, focus ? BLUE : MUTED);
    }
    if (!count) {
        HubSession session = hubSession();
        const char* message = activeCategory == 12 && session.authenticated && (session.premium || session.admin) && !storeAdultConfirmed ?
            "X  Confirme sua idade para abrir os jogos +18." :
            (activeCategory == 11 || activeCategory == 12) && !(session.authenticated && (session.premium || session.admin)) ?
            "X  Entre na sua conta premium para abrir o catálogo." :
            catalogSearch.query[0] ? "Nenhum resultado para esta busca nesta categoria." :
            (activeCategory == 5 ? "Ainda não há atualizações disponíveis." :
             (activeCategory == 6 ? "Ainda não há DLCs disponíveis." : "Nenhum item nesta categoria."));
        text(p, 72, 706, message, FONT_BODY, MUTED);
        text(p, 72, 753, "R3  Alterar busca    |    L1 / R1  Trocar categoria", FONT_SMALL, MUTED);
        if (activeCategory >= 8 && activeCategory <= 10)
            text(p, 72, 803, "OPTIONS  Serviços  >  Importar link PKG ou lista urls.txt", FONT_SMALL, BLUE);
        else if (activeCategory == 11 || activeCategory == 12)
            text(p, 72, 803, "X  Entrar / continuar    |    OPTIONS  Planos e LivePix", FONT_SMALL, BLUE);
        else if (activeCategory == 7)
            text(p, 72, 803, "Temas cadastrados aparecem após sincronizar o catálogo premium.", FONT_SMALL, BLUE);
    }
    footer(p, false, appIndex(selected));
}

static void drawCatalogSearch(uint32_t* p, int selected) {
    drawStore(p, selected, 2);
    for (int i = 0; i < W * H; ++i) p[i] = mix(p[i], BG, 205);
    roundRect(p, 330, 228, 1260, 642, 24, PANEL);
    outline(p, 330, 228, 1260, 642, 24, 2, LINE);
    text(p, 390, 274, "Buscar na biblioteca", FONT_HEADING, WHITE);
    text(p, 390, 330, "Nome, ID do jogo ou Content ID  |  Todos os termos", FONT_SMALL, MUTED);
    roundRect(p, 390, 372, 1140, 60, 12, BG);
    outline(p, 390, 372, 1140, 60, 12, 2, BLUE);
    textElided(p, 410, 384, catalogSearch.draft[0] ? catalogSearch.draft : "Digite sua busca...",
               FONT_BODY, catalogSearch.draft[0] ? WHITE : MUTED, 1084);
    for (int key = 0; key < CATALOG_SEARCH_KEY_COUNT; ++key) {
        const int x = 400 + (key % CATALOG_SEARCH_COLUMNS) * 114;
        const int y = 460 + (key / CATALOG_SEARCH_COLUMNS) * 78;
        const bool focus = key == catalogSearch.key;
        roundRect(p, x, y, 102, 62, 12, focus ? 0x80243A58 : 0x801B2535);
        outline(p, x, y, 102, 62, 12, focus ? 2 : 1, focus ? BLUE : LINE);
        char label[2] = {catalogSearchKeys()[key], 0};
        text(p, x + (102 - textWidth(label, FONT_BODY)) / 2, y + 15, label, FONT_BODY, focus ? WHITE : MUTED);
    }
    text(p, 390, 788, "Direcional  Navegar    X  Inserir    Quadrado  Apagar    Triângulo  Espaço", FONT_SMALL, MUTED);
    text(p, 390, 827, "OPTIONS  Aplicar busca    |    Círculo  Cancelar", FONT_SMALL, WHITE);
    char limit[32];
    snprintf(limit, sizeof(limit), "%u / %u", (unsigned)strlen(catalogSearch.draft), (unsigned)CATALOG_SEARCH_MAX_BYTES);
    text(p, 1530 - textWidth(limit, FONT_SMALL), 827, limit, FONT_SMALL, MUTED);
}

static void drawStorePanel(uint32_t* p) {
    header(p);
    roundRect(p, 72, 164, 1776, 800, 24, PANEL);
    const char* title = storePanel == STORE_PANEL_SERVICES ? "Serviços da Peppy" :
        storePanel == STORE_PANEL_PREMIUM ? (storeAdminLogin ? "Entrar no painel" : "Peppy Premium") :
        storePanel == STORE_PANEL_TEXT ? (storeKeyboard.target == STORE_TEXT_URLS ? "Importar links PKG" : "Editar campo") :
        storePanel == STORE_PANEL_DONATE ? "Apoie a Peppy Store" :
        storePanel == STORE_PANEL_UPDATER ? "Peppy Assets Updater" :
        storePanel == STORE_PANEL_ADMIN ? "Painel administrativo" :
        storePanel == STORE_PANEL_USERS ? "Gerenciar contas premium" :
        storePanel == STORE_PANEL_REVOKE ? "Confirmar alteração de acesso" :
        storePanel == STORE_PANEL_ACCOUNT ? "Editar conta premium" : "Conteúdo para maiores de 18 anos";
    text(p, 112, 198, title, FONT_HEADING, WHITE);
    if (storePanel == STORE_PANEL_TEXT) {
        char shown[513];
        size_t n = strlen(storeKeyboard.draft);
        if (storeKeyboard.masked) { size_t length = n < sizeof(shown) - 1 ? n : sizeof(shown) - 1; memset(shown, '*', length); shown[length] = 0; }
        else snprintf(shown, sizeof(shown), "%.500s", storeKeyboard.draft + (n > 500 ? n - 500 : 0));
        roundRect(p, 112, 284, 1696, 88, 12, BG);
        textWrapped(p, 134, 298, shown[0] ? shown : "Digite com o controle...", FONT_SMALL, shown[0] ? WHITE : MUTED, 1650, 2);
        for (int k = 0; k < int(strlen(storeKeyboardKeys())); ++k) {
            int x = 116 + k % STORE_KEY_COLUMNS * 130, y = 402 + k / STORE_KEY_COLUMNS * 72;
            bool focused = k == storeKeyboard.key;
            roundRect(p, x, y, 118, 58, 10, focused ? 0x80243A58 : BG);
            outline(p, x, y, 118, 58, 10, focused ? 2 : 1, focused ? BLUE : LINE);
            char key[2] = {storeKeyboardKeys()[k], 0};
            text(p, x + (118 - textWidth(key, FONT_BODY)) / 2, y + 11, key, FONT_BODY, focused ? WHITE : MUTED);
        }
        text(p, 112, 898, "Direcional  Navegar    X  Inserir    Quadrado  Apagar    Triângulo  Espaço", FONT_SMALL, MUTED);
        text(p, 112, 932, storeKeyboard.target == STORE_TEXT_URLS ? "L1  Nova linha    OPTIONS  Importar    Círculo  Cancelar" : "OPTIONS  Salvar campo    Círculo  Cancelar", FONT_SMALL, BLUE);
    } else if (storePanel == STORE_PANEL_DONATE) {
        artwork(p, 1330, 238, 420, 360, 24);
        text(p, 112, 298, "LivePix oficial da Peppy", FONT_TITLE, WHITE);
        text(p, 112, 354, STORE_LIVEPIX, FONT_BODY, BLUE);
        text(p, 112, 425, "Doações: a partir de R$ 1,00.", FONT_BODY, WHITE);
        text(p, 112, 493, "Premium: envie exatamente o valor do plano.", FONT_BODY, WHITE);
        text(p, 112, 554, "R$ 10,00  /  15 dias", FONT_TITLE, BLUE);
        text(p, 112, 616, "R$ 20,00  /  1 mês", FONT_TITLE, BLUE);
        text(p, 112, 678, "R$ 30,00  /  2 meses", FONT_TITLE, BLUE);
        textWrapped(p, 112, 740, "Depois do pagamento, envie o comprovante no Discord. A liberação é manual, após conferência do administrador.", FONT_SMALL, MUTED, 1600, 2);
        char discord[128]; snprintf(discord, sizeof(discord), "Discord: %s", STORE_DISCORD);
        text(p, 112, 818, discord, FONT_BODY, WHITE);
    } else if (storePanel == STORE_PANEL_UPDATER) {
        textWrapped(p, 112, 306, "Abra este endereço no navegador para ver a atualização publicada da Peppy Store:", FONT_BODY, WHITE, 1580, 2);
        textWrapped(p, 112, 418, storeUpdaterUrl(), FONT_BODY, BLUE, 1580, 2);
        textWrapped(p, 112, 554, "A atualização da loja é um PKG. Baixe o arquivo publicado e instale pelo instalador do PS4. Não é um payload para o navegador.", FONT_BODY, MUTED, 1580, 3);
        textWrapped(p, 112, 722, "Catálogo premium: use Serviços > Atualizar catálogo premium. Seus links pessoais continuam salvos neste PS4.", FONT_BODY, WHITE, 1580, 2);
    } else if (storePanel == STORE_PANEL_USERS) {
        if (!storeAdminUserCount) text(p, 112, 320, "Nenhuma conta carregada. Quadrado atualiza a lista.", FONT_BODY, MUTED);
        int first = (storeAdminUserSelection / 7) * 7;
        for (int i = first; i < int(storeAdminUserCount) && i < first + 7; ++i) {
            const HubAdminUser& user = storeAdminUsers[i]; int y = 280 + (i - first) * 76;
            bool selected = i == storeAdminUserSelection;
            roundRect(p, 112, y, 1696, 64, 10, selected ? 0x80243A58 : BG);
            textElided(p, 132, y + 16, user.username, FONT_BODY, selected ? WHITE : MUTED, 800);
            const char* state = user.admin ? "Administrador" : user.revoked ? "INVALIDADO" : user.premiumActive ? "Premium ativo" : "Expirado";
            text(p, 1050, y + 16, state, FONT_BODY, user.revoked ? 0x80EBA5B4 : BLUE);
            text(p, 1510, y + 16, user.plan[0] ? user.plan : "Sem prazo", FONT_BODY, MUTED);
        }
        text(p, 112, 838, "X  Editar conta    Quadrado  Atualizar lista", FONT_BODY, BLUE);
        text(p, 112, 885, "O servidor encerra as sessões. PS4s conectados conferem o acesso a cada 5 segundos.", FONT_SMALL, MUTED);
    } else if (storePanel == STORE_PANEL_ACCOUNT) {
        textElided(p, 112, 292, storeSelectedAdminUser.username, FONT_TITLE, WHITE, 1580);
        char password[100]; snprintf(password, sizeof(password), "Nova senha: %s", storeAccountPassword[0] ? "********" : "preencher");
        const char* rows[] = {storeSelectedAdminUser.revoked ? "Reativar acesso premium" : "Invalidar usuário e senha", password, "Salvar nova senha"};
        for (int i = 0; i < 3; ++i) {
            int y = 414 + i * 94; bool focus = i == storeMenuSelection;
            roundRect(p, 112, y, 1580, 72, 10, focus ? 0x80243A58 : BG);
            text(p, 134, y + 20, rows[i], FONT_BODY, focus ? WHITE : MUTED);
        }
        textWrapped(p, 112, 774, "Invalidar bloqueia novos logins e encerra as sessões. Trocar a senha também encerra as sessões existentes.", FONT_BODY, MUTED, 1580, 2);
    } else if (storePanel == STORE_PANEL_REVOKE) {
        const HubAdminUser* user = &storeSelectedAdminUser;
        if (user) {
            char question[256]; snprintf(question, sizeof(question), "%s a conta %s?", user->revoked ? "Reativar" : "Invalidar", user->username);
            textWrapped(p, 112, 320, question, FONT_TITLE, WHITE, 1560, 2);
            textWrapped(p, 112, 482, user->revoked ? "A conta poderá entrar novamente com a senha atual se o plano estiver válido. As sessões anteriores continuam encerradas." :
                "O usuário e a senha deixam de permitir acesso premium. Todas as sessões existentes são encerradas no servidor.", FONT_BODY, MUTED, 1560, 3);
            text(p, 112, 698, "X  Confirmar    Círculo  Cancelar", FONT_TITLE, BLUE);
        }
    } else if (storePanel == STORE_PANEL_ADULT) {
        textWrapped(p, 112, 322, "Esta categoria reúne apenas os itens cadastrados com indicação de idade 18+. Confirme que você tem 18 anos ou mais para visualizar.", FONT_BODY, WHITE, 1500, 3);
        text(p, 112, 584, "X  Tenho 18 anos ou mais", FONT_TITLE, BLUE);
        text(p, 112, 664, "Círculo  Voltar", FONT_BODY, MUTED);
        text(p, 112, 768, "A categoria fica vazia quando não há itens cadastrados.", FONT_BODY, MUTED);
    } else {
        const char* services[] = {"Importar link PKG", "Importar /data/peppy-store/urls.txt", "Entrar no Premium", "Doações e planos / LivePix", "Atualizar catálogo premium", "Peppy Assets Updater", "PKGs recebidos pelo FTP", "Sair do Premium"};
        char user[180], password[80], plan[100];
        bool adminPanel = storePanel == STORE_PANEL_ADMIN;
        snprintf(user, sizeof(user), "Usuário: %s", adminPanel ? storeAdminUser : storeLoginUser);
        snprintf(password, sizeof(password), "Senha: %s", (adminPanel ? storeAdminPassword[0] : storeLoginPassword[0]) ? "********" : "preencher");
        snprintf(plan, sizeof(plan), "Plano: %s  |  Esquerda / Direita", storeAdminPlan == 15 ? "R$ 10 / 15 dias" : storeAdminPlan == 30 ? "R$ 20 / 1 mês" : "R$ 30 / 2 meses");
        const char* premium[] = {user, password, "Entrar", "Ver planos / LivePix", "Sair da conta"};
        const char* admin[] = {user, password, plan, "Criar usuário premium", "Publicar meus PKGs no catálogo premium", "Sincronizar catálogo premium", "Gerenciar / invalidar contas premium"};
        const char** labels = storePanel == STORE_PANEL_SERVICES ? services : adminPanel ? admin : premium;
        int count = storePanel == STORE_PANEL_SERVICES ? 8 : adminPanel ? 7 : 5;
        for (int i = 0; i < count; ++i) {
            int y = 278 + i * 69;
            bool focus = i == storeMenuSelection;
            roundRect(p, 112, y, 1040, 58, 10, focus ? 0x80243A58 : BG);
            textElided(p, 132, y + 13, labels[i], FONT_BODY, focus ? WHITE : MUTED, 990);
        }
        artwork(p, 1298, 275, 460, 360, 24);
        HubSession session = hubSession();
        char account[192]; snprintf(account, sizeof(account), session.authenticated ? "Conta: %s" : "Sem sessão premium", session.username);
        textElided(p, 1218, 688, account, FONT_SMALL, BLUE, 530);
        if (storePanel == STORE_PANEL_PREMIUM) {
            HubSavedLoginStatus saved = hubSavedLoginStatus();
            text(p, 1218, 886, saved.restoring ? "Validando login salvo..." : saved.hasPassword ? "Login salvo neste PS4" :
                 "O login será salvo após entrar.", FONT_SMALL, BLUE);
        }
        textWrapped(p, 1218, 733, storePanel == STORE_PANEL_SERVICES ? "Importe links diretos de PKG ou uma lista, um link por linha. Base, update e DLC são identificados pelo pacote." :
            "A conta é validada pelo servidor. Seu catálogo gratuito e seus links pessoais continuam disponíveis.", FONT_SMALL, MUTED, 530, 4);
    }
    HubSnapshot task = hubSnapshot();
    char status[512];
    if (task.state == HUB_RUNNING) snprintf(status, sizeof(status), "Processando... %u / %u  |  Triângulo cancela", unsigned(task.completed), unsigned(task.requested));
    else snprintf(status, sizeof(status), "%s", storeNotice);
    if (storePanel != STORE_PANEL_TEXT && storePanel != STORE_PANEL_DONATE)
        textElided(p, 112, 922, status, FONT_SMALL, BLUE, 1680);
    text(p, 112, 998, "Direcional  Navegar    X  Selecionar    Círculo  Voltar", FONT_SMALL, WHITE);
    text(p, 112, 1036, STORE_LIVEPIX, FONT_SMALL, BLUE);
}

static void drawDetails(uint32_t* p, int selected) {
    int index = appIndex(selected);
    if (index < 0) { drawStore(p, selected, 2); return; }
    const UiApp& app = storeApp(index);
    uint32_t accent = CATEGORY_COLORS[app.category];
    header(p);
    text(p, 72, 169, "Biblioteca", FONT_BODY, MUTED);
    text(p, 225, 169, "/", FONT_BODY, MUTED);
    textElided(p, 254, 169, app.name, FONT_BODY, WHITE, 1550);
    gradient(p, 72, 246, 552, 626, 28, mix(PANEL, accent, 28), PANEL);
    outline(p, 72, 246, 552, 626, 28, 1, LINE);
    roundRect(p, 232, 359, 232, 232, 46, mix(PANEL, accent, 20));
    if (index >= UI_APP_COUNT) artwork(p, 260, 387, 176, 176, 24);
    else appIcon(p, 260, 387, 176, app.art, accent);
    int nameWidth = textWidth(app.name, FONT_TITLE);
    textElided(p, nameWidth < 500 ? 348 - nameWidth / 2 : 98, 659, app.name, FONT_TITLE, WHITE, 500);
    int categoryWidth = textWidth(CATEGORY_NAMES[app.category], FONT_SMALL) + 32;
    pill(p, 348 - categoryWidth / 2, 725, CATEGORY_NAMES[app.category], 0x80212B3B, accent);

    pill(p, 708, 252, app.sourceBadge, 0x80212D41, BLUE);
    textElided(p, 704, 321, app.name, FONT_HEADING, WHITE, 1140);
    text(p, 708, 396, app.caption, FONT_BODY, MUTED);
    char info[192], size[40];
    sizeLabel(size, sizeof(size), app.sizeBytes);
    snprintf(info, sizeof(info), "%s  |  %s  |  %s", app.version, size, app.developer);
    textElided(p, 708, 443, info, FONT_SMALL, MUTED, 1140);
    rect(p, 708, 494, 1140, 1, LINE);
    text(p, 708, 517, "Sobre este app", FONT_TITLE, WHITE);
    textWrapped(p, 708, 577, app.description, FONT_BODY, MUTED, 1110, 2);
    textWrapped(p, 708, 661, app.firmware, FONT_SMALL, BLUE, 1110, 2);
    if (*app.requiresData) textElided(p, 708, 714, app.requiresData, FONT_SMALL, MUTED, 1110);

    DownloadSnapshot status = downloadSnapshot();
    InstallSnapshot install = installSnapshot();
    char measurementLabel[96] = {};
    bool mine = downloadingApp == index, myInstall = installingApp == index;
    const char* action = "Baixar e instalar";
    uint32_t button = 0x8032609A;
    char buttonLabel[96];
    if (install.cleanupCode) {
        action = "Reabra a Peppy Store";
        button = 0x8024344A;
    } else if (install.state == INSTALL_RUNNING) {
        if (myInstall) {
            snprintf(buttonLabel, sizeof(buttonLabel), "%s %d%%",
                     installCancelRequested ? "Cancelando..." : "Instalando...", installPercent(install));
            action = buttonLabel;
        } else action = "Outro app em instalação";
        button = 0x8024344A;
    } else if (status.state == RUNNING) {
        if (mine) {
            int percent = transferPercent(status.received, status.total);
            snprintf(buttonLabel, sizeof(buttonLabel), "Baixando... %d%%", percent);
            action = buttonLabel;
        } else action = "Outro download em andamento";
        button = 0x8024344A;
    } else if (installedApps[index] || (myInstall && install.state == INSTALL_DONE)) {
        action = "Instalado no PS4"; button = 0x80254B43;
    } else if (downloadedBytes[index]) action = "Tentar instalar novamente";
    else if (mine && status.state == DONE && autoInstallPending) action = "Preparando instalação...";
    else if (mine && status.state == FAILED) action = "Tentar novamente";
    else if (mine && status.state == CANCELLED) action = "Baixar novamente";
    roundRect(p, 708, 771, 610, 70, 16, button);
    crossButton(p, 744, 806);
    text(p, 778, 788, action, FONT_BODY, WHITE);
    if (install.cleanupCode) {
        snprintf(info, sizeof(info), "Feche e reabra a Peppy Store antes de tentar novamente.");
    } else if (install.state == INSTALL_RUNNING) {
        if (myInstall)
            snprintf(info, sizeof(info), "%s  |  %d%%", installStageName(install.stage), installPercent(install));
        else snprintf(info, sizeof(info), "Aguarde a instalação de %s.",
                      validApp(installingApp) ? storeApp(installingApp).name : "outro app");
    } else if (installedApps[index] || (myInstall && install.state == INSTALL_DONE)) {
        snprintf(info, sizeof(info), "Instalação concluída. O app está disponível no menu do PS4.");
    } else if (myInstall && install.state == INSTALL_FAILED) {
        snprintf(info, sizeof(info), "%s PKG preservado para tentar novamente. (código %d)",
                 installErrorText(install.errorCode), install.errorCode);
    } else if (myInstall && install.state == INSTALL_CANCELLED) {
        snprintf(info, sizeof(info), "Instalação cancelada. X tenta instalar o PKG salvo sem baixar novamente.");
    } else if (downloadedBytes[index]) {
        snprintf(info, sizeof(info), "PKG salvo e validado. X inicia a instalação no PS4.");
    } else if (mine && status.state == RUNNING) {
        char received[40], total[40];
        sizeLabel(received, sizeof(received), status.received);
        sizeLabel(total, sizeof(total), status.total);
        transferMeasurementLabel(measurementLabel, sizeof(measurementLabel), status);
        snprintf(info, sizeof(info), "%s de %s", received, total);
    } else if (mine && status.state == DONE && autoInstallPending) {
        snprintf(info, sizeof(info), "Download validado. Preparando a instalação automática.");
    } else if (mine && status.state == FAILED) {
        snprintf(info, sizeof(info), "%s (código %d)", downloadErrorText(status.errorCode), status.errorCode);
    } else if (mine && status.state == CANCELLED) snprintf(info, sizeof(info), "Download cancelado.");
    else if (*app.sha256) snprintf(info, sizeof(info), "SHA-256 conferido ao concluir o download.");
    else snprintf(info, sizeof(info), "Hash não informado pela fonte.");
    textWrapped(p, 708, 861, info, FONT_SMALL, MUTED, 1140, 2);
    if (measurementLabel[0])
        textElided(p, 708, 861 + FONT_SMALL.lineHeight, measurementLabel, FONT_SMALL, BLUE, 1140);
    if (install.cleanupCode || (myInstall && install.state == INSTALL_FAILED)) {
        char diagnostic[192];
        snprintf(diagnostic, sizeof(diagnostic), "%s | 0x%08X | tarefa %d | limpeza 0x%08X",
                 installStageName(install.stage), (unsigned)install.nativeCode,
                 install.taskId, (unsigned)install.cleanupCode);
        textElided(p, 708, 918, diagnostic, FONT_SMALL, BLUE, 1140);
        if (install.mode != INSTALL_MODE_NONE) {
            if (install.mode == INSTALL_MODE_HTTP_LOCAL) {
                char sent[40];
                sizeLabel(sent, sizeof(sent), install.httpBytes);
                snprintf(diagnostic, sizeof(diagnostic),
                         "HTTP local | pedidos %u | resposta %d | enviado %s | SDK errno %d",
                         (unsigned)install.httpRequests, install.httpStatus, sent, install.sdkErrno);
            } else {
                snprintf(diagnostic, sizeof(diagnostic), "Arquivo global | SDK 0x%08X | errno %d",
                         (unsigned)install.sdkVersion, install.sdkErrno);
            }
            textElided(p, 708, 947, diagnostic, FONT_SMALL, MUTED, 1140);
        }
    } else if (mine && status.state == FAILED && !downloadedBytes[index]) {
        char diagnostic[160];
        int used = snprintf(diagnostic, sizeof(diagnostic), "%s | 0x%08X | rede %d",
                            downloadStageName(status.stage), (unsigned)status.nativeCode, status.networkState);
        if (used > 0 && (size_t)used < sizeof(diagnostic)) {
            if (status.sslCode || status.sslDetails)
                snprintf(diagnostic + used, sizeof(diagnostic) - used, " | SSL %08X/%08X",
                         (unsigned)status.sslCode, (unsigned)status.sslDetails);
            else if (status.networkCode)
                snprintf(diagnostic + used, sizeof(diagnostic) - used, " | net %08X", (unsigned)status.networkCode);
        }
        textElided(p, 708, 926, diagnostic, FONT_SMALL, BLUE, 1140);
    }
    footer(p, true, index);
}

#ifndef PEPPY_UI_PREVIEW
static const char* ftpKindLabel(int kind) {
    if (kind == FTP_UPDATE) return "UPDATE";
    if (kind == FTP_DLC) return "DLC";
    return "BASE";
}

static uint32_t ftpKindColor(int kind) {
    if (kind == FTP_UPDATE) return BLUE;
    if (kind == FTP_DLC) return 0x80B9A2F9;
    return 0x807BDECC;
}

static void drawFtpInbox(uint32_t* p) {
    header(p);

    FtpReceiverSnapshot ftp = ftpReceiverSnapshot();
    InstallSnapshot install = installSnapshot();
    bool mine = ftpInboxInstallGeneration &&
                install.generation == ftpInboxInstallGeneration;

    text(p, 72, 164, "PKGs recebidos", FONT_HEADING, WHITE);
    text(p, 72, 218,
         "Envie seus próprios PKGs pelo celular ou PC e instale direto pela Peppy Store.",
         FONT_BODY, MUTED);

    char endpoint[192];
    if (ftp.running && ftp.ip[0]) {
        snprintf(endpoint, sizeof(endpoint), "FTP  %s:%u", ftp.ip, unsigned(ftp.port));
        pill(p, 72, 258, endpoint, 0x80212D41, BLUE);
    } else if (ftp.running) {
        pill(p, 72, 258, "FTP ativo | obtendo IP...", 0x80212D41, BLUE);
    } else {
        snprintf(endpoint, sizeof(endpoint), "FTP indisponível | erro 0x%08X",
                 (unsigned)ftp.lastError);
        pill(p, 72, 258, endpoint, 0x8024344A, 0x80EBA5B4);
    }

    char stats[160], uploaded[40];
    sizeLabel(uploaded, sizeof(uploaded), ftp.bytesReceived);
    snprintf(stats, sizeof(stats), "%d arquivos prontos | %u enviados nesta sessão | %s",
             ftpInboxCount, unsigned(ftp.filesReceived), uploaded);
    text(p, 410, 264, stats, FONT_SMALL, MUTED);

    roundRect(p, 72, 312, 1168, 620, 22, PANEL);
    outline(p, 72, 312, 1168, 620, 22, 1, LINE);
    roundRect(p, 1264, 312, 584, 620, 22, PANEL);
    outline(p, 1264, 312, 584, 620, 22, 1, LINE);

    text(p, 104, 342, "Arquivos", FONT_TITLE, WHITE);
    text(p, 104, 384, "BASE / UPDATE / DLC", FONT_SMALL, MUTED);

    if (!ftpInboxCount) {
        text(p, 104, 458, "Nenhum PKG concluído no inbox ainda.", FONT_BODY, WHITE);
        text(p, 104, 514, "Conecte ao IP e à porta acima com login anônimo e modo passivo.", FONT_SMALL, MUTED);
        text(p, 104, 550, "Entre em /base, /update ou /dlc e envie os arquivos .pkg.", FONT_SMALL, MUTED);
        text(p, 104, 586, ftp.running ? "A porta pode mudar quando o FTP do GoldHEN está ativo." :
             "Quadrado  Tentar iniciar o FTP novamente", FONT_SMALL, BLUE);
    } else {
        const int visible = 7;
        int first = (ftpInboxSelected / visible) * visible;
        for (int slot = 0; slot < visible && first + slot < ftpInboxCount; ++slot) {
            int index = first + slot;
            const FtpInboxItem& item = ftpInboxItems[index];
            bool focus = index == ftpInboxSelected;
            int y = 420 + slot * 68;
            uint32_t accent = ftpKindColor(item.kind);

            if (focus) roundRect(p, 96, y - 3, 1120, 62, 13, 0x80243A58);
            outline(p, 96, y - 3, 1120, 62, 13, focus ? 2 : 1,
                    focus ? BLUE : 0x801E2838);

            pill(p, 116, y + 9, ftpKindLabel(item.kind),
                 mix(PANEL, accent, 22), accent);
            textElided(p, 260, y + 8, item.name, FONT_BODY,
                       focus ? WHITE : 0x80D3DBE8, 700);

            char size[48];
            sizeLabel(size, sizeof(size), item.bytes);
            text(p, 1184 - textWidth(size, FONT_SMALL), y + 14,
                 size, FONT_SMALL, MUTED);
        }

        char page[64];
        snprintf(page, sizeof(page), "Página %d/%d",
                 ftpInboxSelected / visible + 1,
                 (ftpInboxCount + visible - 1) / visible);
        text(p, 104, 892, page, FONT_SMALL, MUTED);
    }

    text(p, 1296, 342, "Instalação local", FONT_TITLE, WHITE);
    rect(p, 1296, 390, 520, 1, LINE);

    if (ftpInboxCount > 0 && ftpInboxSelected < ftpInboxCount) {
        const FtpInboxItem& item = ftpInboxItems[ftpInboxSelected];
        uint32_t accent = ftpKindColor(item.kind);
        pill(p, 1296, 422, ftpKindLabel(item.kind), mix(PANEL, accent, 22), accent);
        textWrapped(p, 1296, 482, item.name, FONT_BODY, WHITE, 500, 3);

        char size[48];
        sizeLabel(size, sizeof(size), item.bytes);
        text(p, 1296, 584, size, FONT_SMALL, MUTED);
        textElided(p, 1296, 620, item.path, FONT_SMALL, MUTED, 500);

        const char* action = "Instalar PKG";
        uint32_t button = 0x8032609A;
        if (install.cleanupCode) {
            action = "Reabra a Peppy Store";
            button = 0x8024344A;
        } else if (install.state == INSTALL_RUNNING) {
            action = mine ? "Instalando..." : "Outra instalação em andamento";
            button = 0x8024344A;
        } else if (mine && install.state == INSTALL_DONE) {
            action = "Instalado no PS4";
            button = 0x80254B43;
        } else if (mine && install.state == INSTALL_FAILED) {
            action = "Tentar instalar novamente";
        } else if (mine && install.state == INSTALL_CANCELLED) {
            action = "Instalar novamente";
        }

        roundRect(p, 1296, 682, 500, 68, 15, button);
        crossButton(p, 1331, 716);
        textElided(p, 1366, 699, action, FONT_BODY, WHITE, 405);

        char status[512];
        if (install.cleanupCode) {
            snprintf(status, sizeof(status),
                     "A instalação precisa de reinício da loja. Limpeza: 0x%08X.",
                     (unsigned)install.cleanupCode);
        } else if (mine && install.state == INSTALL_RUNNING) {
            snprintf(status, sizeof(status), "%s | %d%%",
                     installStageName(install.stage), installPercent(install));
        } else if (mine && install.state == INSTALL_FAILED) {
            snprintf(status, sizeof(status), "%s | código %d | nativo 0x%08X",
                     installErrorText(install.errorCode), install.errorCode,
                     (unsigned)install.nativeCode);
        } else if (mine && install.state == INSTALL_DONE) {
            snprintf(status, sizeof(status),
                     "Concluído. O arquivo original continua salvo no inbox.");
        } else if (mine && install.state == INSTALL_CANCELLED) {
            snprintf(status, sizeof(status),
                     "Instalação cancelada. O PKG foi preservado.");
        } else {
            snprintf(status, sizeof(status),
                     "X instala direto deste arquivo, sem duplicar no cache de downloads.");
        }
        textWrapped(p, 1296, 790, status, FONT_SMALL,
                    (mine && install.state == INSTALL_FAILED) ? 0x80EBA5B4 : MUTED,
                    500, 4);
    } else {
        textWrapped(p, 1296, 440,
                    "Quando um upload terminar, ele aparece aqui pronto para instalar.",
                    FONT_BODY, MUTED, 500, 4);
    }

    rect(p, 72, 978, W - 144, 1, LINE);
    crossButton(p, 88, 1011);
    text(p, 120, 997, "Instalar", FONT_SMALL, WHITE);
    ring(p, 262, 1011, 10, 2, 0x80EBA5B4);
    text(p, 288, 997, "Voltar", FONT_SMALL, WHITE);
    text(p, 430, 997, "Quadrado  Atualizar", FONT_SMALL, MUTED);
    triangleButton(p, 650, 1011);
    text(p, 678, 997, "Cancelar instalação", FONT_SMALL, MUTED);
    text(p, 910, 997, "Direcional  Navegar", FONT_SMALL, MUTED);
    ring(p, 1198, 1011, 18, 2, MUTED);
    text(p, 1183, 997, "L3", FONT_SMALL, MUTED);
    text(p, 1231, 997, "Próxima faixa", FONT_SMALL, MUTED);

    text(p, 72, 1038,
         "Use somente PKGs próprios ou que você tenha autorização para instalar.",
         FONT_SMALL, MUTED);
}
#endif

#ifndef PEPPY_UI_PREVIEW
int main(void){
 sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_VIDEO_OUT);
 int32_t video=sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN,ORBIS_VIDEO_OUT_BUS_MAIN,0,0);
 if(video<0)for(;;)sceKernelUsleep(1000000);

 const size_t one=(size_t)W*H*4,align=0x200000;
 const size_t sz=one*2,alloc=((sz+align-1)/align)*align;
 off_t off=0;int rc=sceKernelAllocateDirectMemory(0,sceKernelGetDirectMemorySize(),alloc,align,3,&off);
 if(rc<0)for(;;)sceKernelUsleep(1000000);
 void*mem=0;rc=sceKernelMapDirectMemory(&mem,alloc,0x33,0,off,align);
 if(rc<0||!mem)for(;;)sceKernelUsleep(1000000);

 uint32_t*fb[2]={(uint32_t*)mem,(uint32_t*)((char*)mem+one)};
 OrbisVideoOutBufferAttribute attr;sceVideoOutSetBufferAttribute(&attr,0x80000000,1,0,W,H,W);
 void*bufs[2]={fb[0],fb[1]};rc=sceVideoOutRegisterBuffers(video,0,bufs,2,&attr);
 if(rc<0)for(;;)sceKernelUsleep(1000000);
 sceVideoOutSetFlipRate(video,0);

 int32_t userModule=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE);
 int32_t userId=-1,userRc=userModule;
 if(userModule>=0){
  OrbisUserServiceInitializeParams usp;
  usp.priority=ORBIS_KERNEL_PRIO_FIFO_LOWEST;
  userRc=sceUserServiceInitialize(&usp);
  if(userRc==0) userRc=sceUserServiceGetInitialUser(&userId);
 }
 musicStart(userId);
 ftpReceiverStart();
 refreshFtpInbox();
 configureHubBaseUrl(PEPPY_HUB_URL);
 if(mkdir("/data/peppy-store",0700)!=0 && errno!=EEXIST)
  snprintf(storeNotice,sizeof(storeNotice),"Não foi possível preparar o armazenamento da biblioteca pessoal.");
 int localLoad=storeLocal.load();
 if(localLoad && localLoad!=USER_CATALOG_ERROR_NOT_FOUND)
  snprintf(storeNotice,sizeof(storeNotice),"%s",userCatalogErrorMessage(localLoad));
 if(configureHubSavedLogin()) startHubSavedLogin();
 else snprintf(storeNotice,sizeof(storeNotice),"Não foi possível preparar o login salvo neste PS4.");
 storeRebuildViews();
 uint32_t ftpReceivedSeen=ftpReceiverSnapshot().filesReceived;
 int32_t padModule=sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_PAD);
 int32_t padInit=(padModule>=0)?scePadInit():padModule;
 int32_t pad=(padInit==0 && userRc==0)?scePadOpen(userId,0,0,0):-1;

 int selected=0,front=0;bool details=false;uint32_t prev=0;int64_t frame=1;
 int progressTicks=0;
 DownloadSnapshot previousProgress=downloadSnapshot();
 InstallSnapshot previousInstall=installSnapshot();
 MusicSnapshot previousMusic=musicSnapshot();
 HubSnapshot previousHub=hubSnapshot();
 FtpReceiverSnapshot previousFtp=ftpReceiverSnapshot();
 uint32_t previousStoreView=storeViewRevision;
 drawStore(fb[front],selected,2);
 sceVideoOutSubmitFlip(video,front,ORBIS_VIDEO_OUT_FLIP_VSYNC,frame++);

 for(;;){
  bool changed=pollAutoInstall();
  changed=pollStoreExtensions() || changed;
  if(storePremiumCatalogRequested) { applyPremiumCatalogRequest(selected,details); changed=true; }
  if(previousStoreView!=storeViewRevision){previousStoreView=storeViewRevision;selected=0;details=false;changed=true;}
  OrbisPadData pd;
  if(pad>=0 && scePadReadState(pad,&pd)>=0){
   static bool readShown=false;
   if(!readShown){front=1-front;drawStore(fb[front],selected,2);sceVideoOutSubmitFlip(video,front,ORBIS_VIDEO_OUT_FLIP_VSYNC,frame++);readShown=true;}
   uint32_t now=pd.buttons;
   uint32_t edge=now & ~prev, pressed=0;
   if(edge&ORBIS_PAD_BUTTON_LEFT) pressed|=CATALOG_LEFT;
   if(edge&ORBIS_PAD_BUTTON_RIGHT) pressed|=CATALOG_RIGHT;
   if(edge&ORBIS_PAD_BUTTON_UP) pressed|=CATALOG_UP;
   if(edge&ORBIS_PAD_BUTTON_DOWN) pressed|=CATALOG_DOWN;
   if(edge&ORBIS_PAD_BUTTON_CROSS) pressed|=CATALOG_CROSS;
   if(edge&ORBIS_PAD_BUTTON_CIRCLE) pressed|=CATALOG_CIRCLE;
   if(edge&ORBIS_PAD_BUTTON_SQUARE) pressed|=CATALOG_SQUARE;
   if(edge&ORBIS_PAD_BUTTON_TRIANGLE) pressed|=CATALOG_TRIANGLE;
   if(edge&ORBIS_PAD_BUTTON_L1) pressed|=CATALOG_L1;
   if(edge&ORBIS_PAD_BUTTON_R1) pressed|=CATALOG_R1;
   if(edge&ORBIS_PAD_BUTTON_L3) pressed|=CATALOG_L3;
   if(edge&ORBIS_PAD_BUTTON_R3) pressed|=CATALOG_R3;
   if(edge&ORBIS_PAD_BUTTON_OPTIONS) pressed|=CATALOG_OPTIONS;
   if(edge&ORBIS_PAD_BUTTON_R2) pressed|=CATALOG_R2;
   uint32_t held=0;
   if(now&ORBIS_PAD_BUTTON_R2) held|=CATALOG_R2;
   if(now&ORBIS_PAD_BUTTON_R3) held|=CATALOG_R3;
   if(now&ORBIS_PAD_BUTTON_OPTIONS) held|=CATALOG_OPTIONS;
   if(ftpInboxOpen) {
    changed=handleFtpInboxController(pressed) || changed;
   } else {
    changed=handleCatalogController(pressed,selected,details,held) || changed;
    if(storeFtpRequested) { storeFtpRequested=false; refreshFtpInbox(); ftpInboxOpen=true; changed=true; }
   }
   prev=now;
  }
  if(++progressTicks>=12){
   progressTicks=0;
   uint64_t measuredAt=downloadNowUs();
   DownloadSnapshot progress=downloadSnapshot();
   if(downloadMeter.update(progress.state==RUNNING,progress.received,progress.total,measuredAt)) changed=true;
   if(progress.state!=previousProgress.state || progress.received!=previousProgress.received || progress.errorCode!=previousProgress.errorCode) changed=true;
   previousProgress=progress;
   InstallSnapshot install=installSnapshot();
   if(install.state!=previousInstall.state || install.percent!=previousInstall.percent ||
      install.received!=previousInstall.received || install.stage!=previousInstall.stage ||
      install.errorCode!=previousInstall.errorCode || install.nativeCode!=previousInstall.nativeCode ||
      install.preparingPercent!=previousInstall.preparingPercent || install.localCopyPercent!=previousInstall.localCopyPercent ||
      install.cleanupCode!=previousInstall.cleanupCode || install.cleanupStage!=previousInstall.cleanupStage ||
      install.generation!=previousInstall.generation || install.mode!=previousInstall.mode ||
      install.sdkVersion!=previousInstall.sdkVersion || install.sdkErrno!=previousInstall.sdkErrno ||
      install.httpRequests!=previousInstall.httpRequests || install.httpStatus!=previousInstall.httpStatus ||
      install.httpBytes!=previousInstall.httpBytes) changed=true;
   previousInstall=install;
   MusicSnapshot music=musicSnapshot();
   if(music.state!=previousMusic.state || music.track!=previousMusic.track || music.volume!=previousMusic.volume ||
      music.muted!=previousMusic.muted || music.errorCode!=previousMusic.errorCode) changed=true;
   previousMusic=music;
   HubSnapshot hub=hubSnapshot();
   if(hub.state!=previousHub.state || hub.completed!=previousHub.completed || hub.errorCode!=previousHub.errorCode) changed=true;
   previousHub=hub;
   FtpReceiverSnapshot ftp=ftpReceiverSnapshot();
   if(ftp.running!=previousFtp.running || ftp.port!=previousFtp.port ||
      ftp.lastError!=previousFtp.lastError || strcmp(ftp.ip,previousFtp.ip)) changed=true;
   if(ftp.filesReceived!=ftpReceivedSeen){
    ftpReceivedSeen=ftp.filesReceived;
    refreshFtpInbox();
    changed=true;
   }
   previousFtp=ftp;
  }
  if(changed){
   front=1-front;
   if(storePanel!=STORE_PANEL_NONE) drawStorePanel(fb[front]);
   else if(ftpInboxOpen) drawFtpInbox(fb[front]);
   else if(catalogSearch.open) drawCatalogSearch(fb[front],selected);
   else if(details) drawDetails(fb[front],selected); else drawStore(fb[front],selected,2);
   sceVideoOutSubmitFlip(video,front,ORBIS_VIDEO_OUT_FLIP_VSYNC,frame++);
  }
  sceKernelUsleep(16000);
 }
 musicStop();
 musicShutdown();
 return 0;
}
#endif
