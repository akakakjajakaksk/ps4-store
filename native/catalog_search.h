#ifndef PEPPY_CATALOG_SEARCH_H
#define PEPPY_CATALOG_SEARCH_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const size_t CATALOG_SEARCH_MAX_BYTES = 64;
static const int CATALOG_SEARCH_COLUMNS = 10;
static const int CATALOG_SEARCH_KEY_COUNT = 40;

// Decode only complete UTF-8 sequences. An invalid byte consumes one byte,
// including a truncated sequence at the end of a string.
inline uint32_t catalogSearchCodepoint(const char*& cursor) {
    const unsigned char first = (unsigned char)*cursor;
    if (!first) return 0;
    ++cursor;
    if (first < 0x80) return first;
    int extra = first >= 0xC2 && first <= 0xDF ? 1 :
                (first >= 0xE0 && first <= 0xEF ? 2 :
                 (first >= 0xF0 && first <= 0xF4 ? 3 : 0));
    if (!extra) return 0x110000U + first;
    const char* next = cursor;
    uint32_t code = first & (extra == 1 ? 31U : (extra == 2 ? 15U : 7U));
    for (int i = 0; i < extra; ++i) {
        unsigned char byte = (unsigned char)*next;
        if (!byte || (byte & 0xC0) != 0x80) return 0x110000U + first;
        code = (code << 6) | (byte & 63U);
        ++next;
    }
    if ((extra == 1 && code < 0x80) || (extra == 2 && code < 0x800) ||
        (extra == 3 && code < 0x10000) || code > 0x10FFFF ||
        (code >= 0xD800 && code <= 0xDFFF)) return 0x110000U + first;
    cursor = next;
    return code;
}

inline bool catalogSearchWhitespace(uint32_t code) {
    return code == ' ' || (code >= '\t' && code <= '\r') || code == 0xA0;
}

inline uint32_t catalogSearchFold(uint32_t code) {
    if (code >= 'A' && code <= 'Z') return code + ('a' - 'A');
    if (code >= 0xC0 && code <= 0xDE && code != 0xD7) code += 0x20;
    if (code >= 0xE0 && code <= 0xE5) return 'a';
    if (code == 0xE7) return 'c';
    if (code >= 0xE8 && code <= 0xEB) return 'e';
    if (code >= 0xEC && code <= 0xEF) return 'i';
    if (code == 0xF1) return 'n';
    if (code >= 0xF2 && code <= 0xF6) return 'o';
    if (code >= 0xF9 && code <= 0xFC) return 'u';
    if (code == 0xFD || code == 0xFF) return 'y';
    return code;
}

inline uint32_t catalogSearchNext(const char*& cursor, const char* end = 0) {
    while (*cursor && (!end || cursor < end)) {
        uint32_t code = catalogSearchCodepoint(cursor);
        // Portuguese accents may also be encoded as combining marks.
        if (code < 0x300 || code > 0x36F) return catalogSearchFold(code);
    }
    return 0;
}

inline bool catalogSearchContains(const char* field, const char* begin, const char* end) {
    if (!field) return false;
    const char* token = begin;
    if (!catalogSearchNext(token, end)) return true;
    for (const char* start = field; *start;) {
        const char* text = start;
        token = begin;
        uint32_t wanted;
        while ((wanted = catalogSearchNext(token, end)) != 0) {
            if (catalogSearchNext(text) != wanted) break;
        }
        if (!wanted) return true;
        catalogSearchCodepoint(start);
    }
    return false;
}

// Every word must occur in at least one searchable field. Fields are streamed
// without truncating long catalog names or Content IDs.
inline bool catalogSearchMatches(const char* name, const char* id,
                                  const char* contentId, const char* query) {
    if (!query) return true;
    const char* cursor = query;
    while (*cursor) {
        const char* begin = cursor;
        uint32_t code = catalogSearchCodepoint(cursor);
        if (catalogSearchWhitespace(code)) continue;
        while (*cursor) {
            const char* next = cursor;
            if (catalogSearchWhitespace(catalogSearchCodepoint(next))) break;
            cursor = next;
        }
        if (!catalogSearchContains(name, begin, cursor) &&
            !catalogSearchContains(id, begin, cursor) &&
            !catalogSearchContains(contentId, begin, cursor)) return false;
    }
    return true;
}

inline const char* catalogSearchKeys() { return "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._/"; }

enum CatalogSearchAction {
    SEARCH_LEFT, SEARCH_RIGHT, SEARCH_UP, SEARCH_DOWN,
    SEARCH_CHARACTER, SEARCH_ERASE, SEARCH_SPACE, SEARCH_APPLY, SEARCH_CANCEL
};

struct CatalogSearchState {
    char query[CATALOG_SEARCH_MAX_BYTES + 1];
    char draft[CATALOG_SEARCH_MAX_BYTES + 1];
    bool open;
    int key;

    CatalogSearchState() : open(false), key(0) {
        memset(query, 0, sizeof(query));
        memset(draft, 0, sizeof(draft));
    }

    void begin() {
        memcpy(draft, query, sizeof(draft));
        draft[CATALOG_SEARCH_MAX_BYTES] = 0;
        key = 0;
        open = true;
    }

    bool append(char character) {
        size_t used = 0;
        while (used < CATALOG_SEARCH_MAX_BYTES && draft[used]) ++used;
        if (used == CATALOG_SEARCH_MAX_BYTES) return false;
        draft[used] = character;
        draft[used + 1] = 0;
        return true;
    }

    bool input(CatalogSearchAction action) {
        if (!open) return false;
        const int rows = CATALOG_SEARCH_KEY_COUNT / CATALOG_SEARCH_COLUMNS;
        int row = key / CATALOG_SEARCH_COLUMNS, column = key % CATALOG_SEARCH_COLUMNS;
        switch (action) {
        case SEARCH_LEFT: key = row * CATALOG_SEARCH_COLUMNS + (column + CATALOG_SEARCH_COLUMNS - 1) % CATALOG_SEARCH_COLUMNS; return true;
        case SEARCH_RIGHT: key = row * CATALOG_SEARCH_COLUMNS + (column + 1) % CATALOG_SEARCH_COLUMNS; return true;
        case SEARCH_UP: key = ((row + rows - 1) % rows) * CATALOG_SEARCH_COLUMNS + column; return true;
        case SEARCH_DOWN: key = ((row + 1) % rows) * CATALOG_SEARCH_COLUMNS + column; return true;
        case SEARCH_CHARACTER: return append(catalogSearchKeys()[key]);
        case SEARCH_SPACE: return append(' ');
        case SEARCH_ERASE: {
            size_t used = strlen(draft);
            if (!used) return false;
            --used;
            while (used && ((unsigned char)draft[used] & 0xC0) == 0x80) --used;
            draft[used] = 0;
            return true;
        }
        case SEARCH_APPLY: memcpy(query, draft, sizeof(query)); open = false; return true;
        case SEARCH_CANCEL: open = false; return true;
        }
        return false;
    }
};

#endif
