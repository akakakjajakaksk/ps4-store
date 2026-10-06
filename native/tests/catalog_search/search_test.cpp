#include "../../catalog_search.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static unsigned checks = 0;

static void check(bool value, const char* expression, int line) {
    ++checks;
    if (!value) {
        std::fprintf(stderr, "catalog search test failed at line %d: %s\n", line, expression);
        std::exit(1);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

static void matching() {
    CHECK(catalogSearchMatches("God of War", "CUSA07408", "EP9000-CUSA07408_00-GODOFWAR000000000", ""));
    CHECK(catalogSearchMatches(NULL, NULL, NULL, NULL));
    CHECK(catalogSearchMatches(NULL, NULL, NULL, " \t\r\n "));
    CHECK(!catalogSearchMatches(NULL, NULL, NULL, "god"));
    CHECK(catalogSearchMatches("God of War", NULL, NULL, "gOd WaR"));
    CHECK(catalogSearchMatches("God of War", NULL, NULL, " \tGod\r\nWar  "));
    CHECK(catalogSearchMatches("God of War", NULL, NULL, "God\v\fWar"));
    CHECK(catalogSearchMatches("God of War", NULL, NULL, "God\u00a0War"));
    CHECK(catalogSearchMatches(NULL, NULL, NULL, "\u00a0\t\u00a0"));
    CHECK(!catalogSearchMatches("God of War", NULL, NULL, "god ragnarok"));
    CHECK(!catalogSearchMatches("God of War", "CUSA07408", "EP9000", "god unknown"));

    // Every query word may match a different catalog field, in any order.
    CHECK(catalogSearchMatches("God of War", "CUSA07408", "EP9000-CUSA07408_00-GODOFWAR000000000", "war cusa07408 ep9000"));
    CHECK(catalogSearchMatches("God of War", "CUSA07408", "EP9000-CUSA07408_00-GODOFWAR000000000", "EP9000 WAR CUSA07408"));
    CHECK(catalogSearchMatches(NULL, "CUSA07408", NULL, "cusa07408"));
    CHECK(catalogSearchMatches(NULL, NULL, "EP9000-CUSA07408_00-GODOFWAR000000000", "ep9000-cusa07408_00"));
    CHECK(catalogSearchMatches("Version 1.2/HD", NULL, NULL, "1.2/hd"));
    CHECK(!catalogSearchMatches("God of War", "CUSA07408", "EP9000", "cusa07409"));
    CHECK(!catalogSearchMatches(NULL, NULL, "EP9000-CUSA07408_00", "ep9001"));

    const char* const accented[] = {
        "á", "à", "â", "ã", "Á", "À", "Â", "Ã",
        "é", "ê", "É", "Ê", "í", "Í",
        "ó", "ô", "õ", "Ó", "Ô", "Õ", "ú", "Ú", "ç", "Ç"
    };
    const char* const plain[] = {
        "a", "a", "a", "a", "a", "a", "a", "a",
        "e", "e", "e", "e", "i", "i",
        "o", "o", "o", "o", "o", "o", "u", "u", "c", "c"
    };
    for (std::size_t i = 0; i < sizeof(accented) / sizeof(accented[0]); ++i) {
        CHECK(catalogSearchMatches(accented[i], NULL, NULL, plain[i]));
        CHECK(catalogSearchMatches(plain[i], NULL, NULL, accented[i]));
    }
    CHECK(catalogSearchMatches("AÇÃO E AVENTURA: Edição Ótima", NULL, NULL, "acao edicao otima"));
    CHECK(catalogSearchMatches("acao e aventura", NULL, NULL, "AÇÃO AVENTURA"));
    CHECK(catalogSearchMatches("São João", "ID-ÚNICO", "CONTEÚDO-Ç", "sao unico conteudo"));
    CHECK(catalogSearchMatches("EDIÇÃO", NULL, NULL, "edicao")); // Accent at the word end.
    CHECK(catalogSearchMatches("São", NULL, NULL, "sao"));
    CHECK(catalogSearchMatches("AÇAÍ", NULL, NULL, "acai"));
    CHECK(catalogSearchMatches("Ñandú", NULL, NULL, "nandu"));
    CHECK(catalogSearchMatches("Ac\u0327a\u0303o", NULL, NULL, "ação"));
    CHECK(catalogSearchMatches("AÇÃO", NULL, NULL, "ac\u0327a\u0303o"));
    CHECK(catalogSearchMatches("Edição", NULL, NULL, "EDIC\u0327A\u0303O"));
    CHECK(!catalogSearchMatches("Ação", NULL, NULL, "acao corrida"));

    // Search all of the original fields rather than a fixed-size display copy.
    const std::string longName = std::string(8192, 'x') + " Aventura Ção";
    const std::string longId = std::string(8192, 'y') + " CUSA99999";
    const std::string longContent = std::string(8192, 'z') + " EP9999-TAIL";
    CHECK(catalogSearchMatches(longName.c_str(), longId.c_str(), longContent.c_str(), "aventura cao cusa99999 ep9999-tail"));
    CHECK(!catalogSearchMatches(longName.c_str(), longId.c_str(), longContent.c_str(), "aventura missing"));
    const std::string longWord = std::string(80, 'a') + std::string(80, 'b');
    CHECK(catalogSearchMatches(longWord.c_str(), NULL, NULL, longWord.c_str()));
    const std::string mismatchAfterEditorLimit = std::string(80, 'a') + std::string(80, 'c');
    CHECK(!catalogSearchMatches(longWord.c_str(), NULL, NULL, mismatchAfterEditorLimit.c_str()));
    const std::string longQuery = std::string(100, ' ') + "aventura cusa99999 ep9999-tail";
    CHECK(catalogSearchMatches(longName.c_str(), longId.c_str(), longContent.c_str(), longQuery.c_str()));
}

static void malformedUtf8() {
    // Exact-size buffers let ASan detect lookahead beyond a truncated sequence.
    const char truncatedTwo[] = {static_cast<char>(0xc3), '\0'};
    const char truncatedThree[] = {static_cast<char>(0xe2), static_cast<char>(0x82), '\0'};
    const char truncatedFour[] = {static_cast<char>(0xf0), static_cast<char>(0x9f), static_cast<char>(0x92), '\0'};
    const char strayContinuation[] = {static_cast<char>(0x80), '\0'};
    const char invalidLead[] = {static_cast<char>(0xff), '\0'};
    const char overlong[] = {static_cast<char>(0xe0), static_cast<char>(0x80), static_cast<char>(0xaf), '\0'};
    const char surrogate[] = {static_cast<char>(0xed), static_cast<char>(0xa0), static_cast<char>(0x80), '\0'};
    const char outsideUnicode[] = {static_cast<char>(0xf4), static_cast<char>(0x90), static_cast<char>(0x80), static_cast<char>(0x80), '\0'};
    const char* const malformed[] = {
        truncatedTwo, truncatedThree, truncatedFour, strayContinuation, invalidLead,
        overlong, surrogate, outsideUnicode
    };
    for (std::size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        CHECK(catalogSearchMatches(malformed[i], NULL, NULL, ""));
        CHECK(!catalogSearchMatches(malformed[i], NULL, NULL, "war"));
        CHECK(catalogSearchMatches(malformed[i], "CUSA12345", NULL, "cusa12345"));
        // Invalid query bytes have no prescribed transliteration, but must scan safely.
        (void)catalogSearchMatches("God of War", "CUSA12345", "EP9000", malformed[i]);
        const std::string embedded = std::string("prefix ") + malformed[i] + " suffix";
        CHECK(catalogSearchMatches(embedded.c_str(), NULL, NULL, "prefix suffix"));
    }
    const char badContinuation[] = {static_cast<char>(0xc3), 'x', ' ', 'w', 'a', 'r', '\0'};
    CHECK(catalogSearchMatches(badContinuation, NULL, NULL, "war"));
}

static void editing() {
    CatalogSearchState state;
    CHECK(CATALOG_SEARCH_MAX_BYTES == 64);
    CHECK(CATALOG_SEARCH_KEY_COUNT == 40);
    CHECK(CATALOG_SEARCH_COLUMNS == 10);
    CHECK(std::strcmp(catalogSearchKeys(), "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._/") == 0);
    CHECK(!state.open && state.key == 0);
    CHECK(state.query[0] == '\0' && state.draft[0] == '\0');
    const CatalogSearchAction actions[] = {
        SEARCH_LEFT, SEARCH_RIGHT, SEARCH_UP, SEARCH_DOWN, SEARCH_CHARACTER,
        SEARCH_ERASE, SEARCH_SPACE, SEARCH_APPLY, SEARCH_CANCEL
    };
    for (std::size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i)
        CHECK(!state.input(actions[i]));
    CHECK(!state.open && state.key == 0 && state.query[0] == '\0' && state.draft[0] == '\0');

    std::strcpy(state.query, "GOD OF WAR");
    state.key = 17;
    state.begin();
    CHECK(state.open && state.key == 0);
    CHECK(std::strcmp(state.draft, "GOD OF WAR") == 0);
    CHECK(state.input(SEARCH_ERASE));
    CHECK(std::strcmp(state.draft, "GOD OF WA") == 0);
    state.key = 17; // R.
    CHECK(state.input(SEARCH_CHARACTER));
    CHECK(std::strcmp(state.draft, "GOD OF WAR") == 0);
    CHECK(state.input(SEARCH_SPACE));
    state.key = 1; // B.
    CHECK(state.input(SEARCH_CHARACTER));
    CHECK(std::strcmp(state.draft, "GOD OF WAR B") == 0);
    CHECK(std::strcmp(state.query, "GOD OF WAR") == 0);
    CHECK(state.input(SEARCH_CANCEL));
    CHECK(!state.open && std::strcmp(state.query, "GOD OF WAR") == 0);
    CHECK(!state.input(SEARCH_CANCEL));

    state.begin();
    CHECK(std::strcmp(state.draft, "GOD OF WAR") == 0);
    CHECK(state.input(SEARCH_ERASE));
    CHECK(state.input(SEARCH_APPLY));
    CHECK(!state.open && std::strcmp(state.query, "GOD OF WA") == 0);
    CHECK(!state.input(SEARCH_APPLY));
    state.begin();
    CHECK(state.input(SEARCH_APPLY)); // Closing the editor changes state even without an edit.
    CHECK(!state.open && std::strcmp(state.query, "GOD OF WA") == 0);

    state.query[0] = '\0';
    state.begin();
    CHECK(!state.input(SEARCH_ERASE));
    for (int i = 0; i < CATALOG_SEARCH_KEY_COUNT; ++i) {
        state.key = i;
        CHECK(state.input(SEARCH_CHARACTER));
    }
    CHECK(std::strcmp(state.draft, catalogSearchKeys()) == 0);
    CHECK(state.input(SEARCH_APPLY));
    CHECK(std::strcmp(state.query, catalogSearchKeys()) == 0);
}

static void navigation() {
    CatalogSearchState state;
    state.begin();
    for (int row = 0; row < CATALOG_SEARCH_KEY_COUNT / CATALOG_SEARCH_COLUMNS; ++row) {
        state.key = row * CATALOG_SEARCH_COLUMNS;
        CHECK(state.input(SEARCH_LEFT));
        CHECK(state.key == row * CATALOG_SEARCH_COLUMNS + CATALOG_SEARCH_COLUMNS - 1);
        CHECK(state.input(SEARCH_RIGHT));
        CHECK(state.key == row * CATALOG_SEARCH_COLUMNS);
    }
    for (int column = 0; column < CATALOG_SEARCH_COLUMNS; ++column) {
        state.key = column;
        CHECK(state.input(SEARCH_UP));
        CHECK(state.key == CATALOG_SEARCH_KEY_COUNT - CATALOG_SEARCH_COLUMNS + column);
        CHECK(state.input(SEARCH_DOWN));
        CHECK(state.key == column);
    }
    state.key = 14;
    CHECK(state.input(SEARCH_LEFT) && state.key == 13);
    CHECK(state.input(SEARCH_RIGHT) && state.key == 14);
    CHECK(state.input(SEARCH_UP) && state.key == 4);
    CHECK(state.input(SEARCH_DOWN) && state.key == 14);
    CHECK(state.query[0] == '\0' && state.draft[0] == '\0');
}

static void byteLimitAndUtf8Erase() {
    CatalogSearchState state;
    state.begin();
    state.key = 0; // A.
    for (std::size_t i = 0; i < CATALOG_SEARCH_MAX_BYTES; ++i)
        CHECK(state.input(SEARCH_CHARACTER));
    CHECK(std::strlen(state.draft) == CATALOG_SEARCH_MAX_BYTES);
    CHECK(state.draft[CATALOG_SEARCH_MAX_BYTES] == '\0');
    CHECK(!state.input(SEARCH_CHARACTER));
    CHECK(!state.input(SEARCH_SPACE));
    CHECK(std::strlen(state.draft) == CATALOG_SEARCH_MAX_BYTES);
    CHECK(state.input(SEARCH_ERASE));
    CHECK(state.input(SEARCH_SPACE));
    CHECK(std::strlen(state.draft) == CATALOG_SEARCH_MAX_BYTES);
    CHECK(state.draft[CATALOG_SEARCH_MAX_BYTES - 1] == ' ');
    CHECK(state.input(SEARCH_APPLY));
    CHECK(std::strlen(state.query) == CATALOG_SEARCH_MAX_BYTES);
    CHECK(state.query[CATALOG_SEARCH_MAX_BYTES] == '\0');
    state.begin();
    CHECK(!state.input(SEARCH_CHARACTER));
    CHECK(state.input(SEARCH_CANCEL));
    CHECK(std::strlen(state.query) == CATALOG_SEARCH_MAX_BYTES);

    // Existing queries can contain UTF-8 even though the keyboard inserts ASCII.
    std::strcpy(state.query, "Aç€😀");
    state.begin();
    CHECK(state.input(SEARCH_ERASE));
    CHECK(std::strcmp(state.draft, "Aç€") == 0);
    CHECK(state.input(SEARCH_ERASE));
    CHECK(std::strcmp(state.draft, "Aç") == 0);
    CHECK(state.input(SEARCH_ERASE));
    CHECK(std::strcmp(state.draft, "A") == 0);
    CHECK(state.input(SEARCH_ERASE));
    CHECK(state.draft[0] == '\0');
    CHECK(!state.input(SEARCH_ERASE));
    CHECK(state.input(SEARCH_APPLY));
    CHECK(state.query[0] == '\0');

    const std::string boundary = std::string(CATALOG_SEARCH_MAX_BYTES - 2, 'B') + "ç";
    std::strcpy(state.query, boundary.c_str());
    state.begin();
    CHECK(!state.input(SEARCH_CHARACTER));
    CHECK(state.input(SEARCH_ERASE));
    CHECK(std::strlen(state.draft) == CATALOG_SEARCH_MAX_BYTES - 2);
    CHECK(state.input(SEARCH_CHARACTER));
    CHECK(state.input(SEARCH_CHARACTER));
    CHECK(!state.input(SEARCH_CHARACTER));
    CHECK(state.draft[CATALOG_SEARCH_MAX_BYTES] == '\0');
}

int main() {
    matching();
    malformedUtf8();
    editing();
    navigation();
    byteLimitAndUtf8Erase();
    std::printf("Catalog matching, UTF-8 safety, editor transitions and bounds passed (%u checks).\n", checks);
    return 0;
}
