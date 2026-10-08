#include "../../user_catalog.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>
#ifdef PEPPY_USER_CATALOG_NATIVE_FILE_ABI
extern "C" void peppyTestCatalogFstatFailure(bool enabled);
#endif

namespace {
unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); abort(); } } while (0)
const char* URL = "https://files.example.com/game.pkg";
const char* CID = "UP0000-CUSA12345_00-USERGAME00000000";
void be32(std::vector<unsigned char>& data, size_t off, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) data[off + i] = (unsigned char)(v >> (24 - 8 * i));
}
void be64(std::vector<unsigned char>& data, size_t off, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) data[off + i] = (unsigned char)(v >> (56 - 8 * i));
}
void le16(std::vector<unsigned char>& data, size_t off, uint16_t v) {
    data[off] = (unsigned char)v; data[off + 1] = (unsigned char)(v >> 8);
}
void le32(std::vector<unsigned char>& data, size_t off, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) data[off + i] = (unsigned char)(v >> (8 * i));
}
uint32_t digest(const std::vector<unsigned char>& data, size_t start) {
    uint32_t hash = 2166136261U;
    for (size_t i = start; i < data.size(); ++i) hash = (hash ^ data[i]) * 16777619U;
    return hash;
}
std::vector<unsigned char> makeSfo(const char* title, const char* version, const char* cid = CID) {
    const char* keys[] = { "TITLE", "APP_VER", "CONTENT_ID", "TITLE_ID" };
    const char* values[] = { title, version, cid, "CUSA12345" };
    size_t keysOff = 20 + 4 * 16, dataOff = keysOff;
    for (unsigned i = 0; i < 4; ++i) dataOff += strlen(keys[i]) + 1;
    dataOff = (dataOff + 3) & ~size_t(3);
    size_t total = dataOff;
    for (unsigned i = 0; i < 4; ++i) total += strlen(values[i]) + 1;
    std::vector<unsigned char> sfo(total);
    memcpy(sfo.data(), "\0PSF", 4);
    le32(sfo, 4, 0x00000101); le32(sfo, 8, (uint32_t)keysOff); le32(sfo, 12, (uint32_t)dataOff); le32(sfo, 16, 4);
    size_t keyOffset = 0, dataOffset = 0;
    for (unsigned i = 0; i < 4; ++i) {
        size_t n = strlen(values[i]) + 1, off = 20 + i * 16;
        le16(sfo, off, (uint16_t)keyOffset); le16(sfo, off + 2, 0x0204);
        le32(sfo, off + 4, (uint32_t)n); le32(sfo, off + 8, (uint32_t)n); le32(sfo, off + 12, (uint32_t)dataOffset);
        memcpy(sfo.data() + keysOff + keyOffset, keys[i], strlen(keys[i]) + 1);
        memcpy(sfo.data() + dataOff + dataOffset, values[i], n);
        keyOffset += strlen(keys[i]) + 1; dataOffset += n;
    }
    return sfo;
}
struct Reader {
    std::vector<unsigned char> pkg;
    unsigned calls;
    size_t downloaded;
    bool fail, shortRead, changingTotal, badEffective;
    Reader() : pkg(4096), calls(0), downloaded(0), fail(false), shortRead(false), changingTotal(false), badEffective(false) {
        memcpy(pkg.data(), "\x7f" "CNT", 4); memcpy(pkg.data() + 0x40, CID, 36);
        be32(pkg, 0x74, 0x1A); be32(pkg, 0x78, 0x0A000000); be64(pkg, 0x430, pkg.size());
    }
    void sfo(const std::vector<unsigned char>& bytes, bool encrypted = false) {
        be32(pkg, 0x10, 1); be32(pkg, 0x18, 0x500);
        be32(pkg, 0x500, 0x1000); be32(pkg, 0x508, encrypted ? 0x80000000U : 0);
        be32(pkg, 0x510, 0x600); be32(pkg, 0x514, (uint32_t)bytes.size());
        if (pkg.size() < 0x600 + bytes.size()) pkg.resize(0x600 + bytes.size());
        memcpy(pkg.data() + 0x600, bytes.data(), bytes.size()); be64(pkg, 0x430, pkg.size());
    }
};
bool readRange(void* context, const char* url, uint64_t offset, size_t requested,
               unsigned char* output, UserCatalogRangeInfo* info) {
    Reader& r = *(Reader*)context;
    ++r.calls; r.downloaded += requested;
    if (r.fail || offset > r.pkg.size() || requested > r.pkg.size() - offset) return false;
    memcpy(output, r.pkg.data() + (size_t)offset, requested);
    info->received = r.shortRead ? requested - 1 : requested;
    info->totalBytes = r.pkg.size() + (r.changingTotal && r.calls > 1 ? 1 : 0);
    snprintf(info->effectiveUrl, sizeof(info->effectiveUrl), "%s", r.badEffective ? "https://127.0.0.1/a.pkg" : url);
    return true;
}
UserCatalogEntry entryFor(const char* url = URL, int kind = USER_PACKAGE_BASE, const char* version = "01.00") {
    UserCatalogEntry entry = {};
    snprintf(entry.url, sizeof(entry.url), "%s", url); snprintf(entry.name, sizeof(entry.name), "Jogo português");
    snprintf(entry.contentId, sizeof(entry.contentId), "%s", CID);
    entry.sizeBytes = 4096; entry.kind = kind; entry.titleKnown = true;
    entry.contentType = kind == USER_PACKAGE_DLC ? 0x1B : 0x1A;
    entry.contentFlags = kind == USER_PACKAGE_UPDATE ? 0x6A700000 : 0x0A000000;
    if (version) { snprintf(entry.version, sizeof(entry.version), "%s", version); entry.versionKnown = true; }
    CHECK(userCatalogPrepareEntry(&entry));
    return entry;
}
void checkUrls() {
    const char* valid[] = { URL, "https://cdn.real-host.com:443/Name%20Here.PKG?token=abc", "https://ia800705.us.archive.org/path/pkg", "https://github.com/author/repository/releases/download/tag/game.pkg" };
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) CHECK(userCatalogPublicHttpsUrl(valid[i]));
    const char* invalid[] = {
        "", "http://example.com/a.pkg", "HTTPS://example.com/a.pkg", "https://example.com/", "https://example.com/?x=1",
        "https://example.com:80/a.pkg", "https://user:pw@example.com/a.pkg", "https://example.com/a.pkg#fragment",
        "https://127.0.0.1/a.pkg", "https://2130706433/a.pkg", "https://0x7f000001/a.pkg", "https://[::1]/a.pkg",
        "https://localhost/a.pkg", "https://host.local/a.pkg", "https://host.internal/a.pkg", "https://host.test/a.pkg",
        "https://foo..com/a.pkg", "https://-foo.com/a.pkg", "https://foo-.com/a.pkg", "https://example.com./a.pkg",
        "https://example.com/a.pkg\nHost: other", "https://example.com/a\\x.pkg", "https://example.com/%0d.pkg",
        "https://example.com/%00.pkg", "https://example.com/%2fetc", "https://example.com/a/../game.pkg",
        "https://example.com/a/%2e%2e/game.pkg", "https://example.com/a/./game.pkg", "https://example.com/a%ZZ.pkg"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) CHECK(!userCatalogPublicHttpsUrl(invalid[i]));
    std::string tooLong = "https://files.example.com/" + std::string(2048, 'a');
    CHECK(!userCatalogPublicHttpsUrl(tooLong.c_str()));
    CHECK(userCatalogSourceForUrl("https://github.com/bucanero/apollo-ps4/releases/download/v2/a.pkg") == USER_SOURCE_RECOGNIZED_AUTHOR_RELEASE);
    CHECK(userCatalogSourceForUrl("https://github.com/bucanero/apollo-ps4-other/releases/download/v2/a.pkg") == USER_SOURCE_COMMUNITY_UNKNOWN);
    CHECK(userCatalogSourceForUrl("https://github.com/bucanero/apollo-ps4/releases/download/v2/../../a.pkg") == USER_SOURCE_COMMUNITY_UNKNOWN);
    CHECK(userCatalogSourceForUrl("https://github.com/a/a.pkg") == USER_SOURCE_COMMUNITY_UNKNOWN);
    CHECK(userCatalogSourceForUrl("https://github.com.evil.com/bucanero/apollo-ps4/releases/download/v2/a.pkg") == USER_SOURCE_COMMUNITY_UNKNOWN);
}
void checkHeaders() {
    Reader r;
    CHECK(userPackageKind(r.pkg.data(), r.pkg.size()) == USER_PACKAGE_BASE);
    CHECK(userPackageHeaderMatches(r.pkg.data(), r.pkg.size(), r.pkg.size(), CID, USER_PACKAGE_BASE));
    CHECK(!userPackageHeaderMatches(r.pkg.data(), r.pkg.size(), r.pkg.size() + 1, CID, USER_PACKAGE_BASE));
    CHECK(!userPackageHeaderMatches(r.pkg.data(), 100, r.pkg.size(), CID, USER_PACKAGE_BASE));
    be32(r.pkg, 0x78, 0x0E000000); CHECK(userPackageKind(r.pkg.data(), r.pkg.size()) == USER_PACKAGE_BASE);
    be32(r.pkg, 0x78, 0x0A700000); CHECK(userPackageKind(r.pkg.data(), r.pkg.size()) == USER_PACKAGE_UPDATE);
    be32(r.pkg, 0x74, 0x1E); CHECK(userPackageKind(r.pkg.data(), r.pkg.size()) == USER_PACKAGE_UPDATE);
    be32(r.pkg, 0x74, 0x1B); CHECK(userPackageKind(r.pkg.data(), r.pkg.size()) == USER_PACKAGE_UNKNOWN);
    be32(r.pkg, 0x78, 0x0A000000); CHECK(userPackageKind(r.pkg.data(), r.pkg.size()) == USER_PACKAGE_DLC);
    be32(r.pkg, 0x98, 2); CHECK(userPackageIsTheme(r.pkg.data(), r.pkg.size()));
    be32(r.pkg, 0x98, 3); CHECK(!userPackageIsTheme(r.pkg.data(), r.pkg.size()));
    be32(r.pkg, 0x74, 0x1C); CHECK(userPackageKind(r.pkg.data(), r.pkg.size()) == USER_PACKAGE_DLC);
    be32(r.pkg, 0x98, 1); CHECK(!userPackageIsTheme(r.pkg.data(), r.pkg.size()));
    be32(r.pkg, 0x98, 2); CHECK(!userPackageIsTheme(r.pkg.data(), r.pkg.size()));
    be32(r.pkg, 0x74, 0x1A); be32(r.pkg, 0x78, 0x0A700000);
    CHECK(userPackageKind(r.pkg.data(), r.pkg.size()) == USER_PACKAGE_UPDATE);
    CHECK(!userPackageIsTheme(r.pkg.data(), r.pkg.size()));
    be32(r.pkg, 0x74, 0x1A); be32(r.pkg, 0x78, 0x400); CHECK(userPackageKind(r.pkg.data(), r.pkg.size()) == USER_PACKAGE_UNKNOWN);
    memcpy(r.pkg.data() + 0x47, "PPSA12345", 9); CHECK(userPackageKind(r.pkg.data(), r.pkg.size()) == USER_PACKAGE_UNKNOWN);
    memcpy(r.pkg.data() + 0x47, "BREW00001", 9); CHECK(userPackageKind(r.pkg.data(), r.pkg.size()) == USER_PACKAGE_UNKNOWN);
}
void checkProbe() {
    UserCatalogEntry out = {};
    Reader r;
    CHECK(probeUserPackage(URL, readRange, &r, &out) == USER_CATALOG_OK);
    CHECK(!strcmp(out.name, "CUSA12345") && !out.titleKnown && !out.versionKnown && !out.version[0]);
    CHECK(!strcmp(out.filename, "UP0000-CUSA12345_00-USERGAME00000000-base-unknown.pkg"));
    CHECK(r.calls == 1 && r.downloaded == USER_PACKAGE_HEADER_BYTES);
    Reader meta; meta.sfo(makeSfo("Jogo em português", "01.23"));
    CHECK(probeUserPackage(URL, readRange, &meta, &out) == USER_CATALOG_OK);
    CHECK(!strcmp(out.name, "Jogo em português") && !strcmp(out.version, "01.23") && out.titleKnown && out.versionKnown);
    CHECK(meta.calls == 3 && meta.downloaded <= USER_CATALOG_MAX_METADATA_BYTES);
    Reader signedSfo;
    std::vector<unsigned char> wrapped(0x800); memcpy(wrapped.data(), "SCEC", 4);
    std::vector<unsigned char> sfo = makeSfo("Signed metadata", "02.00"); wrapped.insert(wrapped.end(), sfo.begin(), sfo.end());
    signedSfo.sfo(wrapped); CHECK(probeUserPackage(URL, readRange, &signedSfo, &out) == USER_CATALOG_OK);
    CHECK(!strcmp(out.name, "Signed metadata"));
    Reader encrypted; encrypted.sfo(makeSfo("Hidden", "01.00"), true);
    CHECK(probeUserPackage(URL, readRange, &encrypted, &out) == USER_CATALOG_OK);
    CHECK(!out.titleKnown && encrypted.calls == 2);
    Reader mismatch; mismatch.sfo(makeSfo("Wrong package", "01.00", "UP0000-CUSA54321_00-USERGAME00000000"));
    CHECK(probeUserPackage(URL, readRange, &mismatch, &out) == USER_CATALOG_ERROR_METADATA);
    Reader invalidUtf8; invalidUtf8.sfo(makeSfo("Bad\xC0\xAF", "01.00"));
    CHECK(probeUserPackage(URL, readRange, &invalidUtf8, &out) == USER_CATALOG_ERROR_METADATA);
    Reader badVersion; badVersion.sfo(makeSfo("Game", "../../file"));
    CHECK(probeUserPackage(URL, readRange, &badVersion, &out) == USER_CATALOG_ERROR_METADATA);
    Reader malformed; sfo = makeSfo("Game", "01.00"); le32(sfo, 20 + 8, 0xFFFFFFFFU); malformed.sfo(sfo);
    CHECK(probeUserPackage(URL, readRange, &malformed, &out) == USER_CATALOG_ERROR_METADATA);
    Reader shortRead; shortRead.shortRead = true;
    CHECK(probeUserPackage(URL, readRange, &shortRead, &out) == USER_CATALOG_ERROR_RANGE);
    Reader changed; changed.sfo(makeSfo("Game", "01.00")); changed.changingTotal = true;
    CHECK(probeUserPackage(URL, readRange, &changed, &out) == USER_CATALOG_ERROR_RANGE);
    Reader privateRedirect; privateRedirect.badEffective = true;
    CHECK(probeUserPackage(URL, readRange, &privateRedirect, &out) == USER_CATALOG_ERROR_RANGE);
    Reader wrongSize; be64(wrongSize.pkg, 0x430, 1);
    CHECK(probeUserPackage(URL, readRange, &wrongSize, &out) == USER_CATALOG_ERROR_HEADER);
    Reader tableOutside; be32(tableOutside.pkg, 0x10, 1); be32(tableOutside.pkg, 0x18, 4090);
    CHECK(probeUserPackage(URL, readRange, &tableOutside, &out) == USER_CATALOG_ERROR_METADATA);
    Reader html; html.pkg[0] = '<';
    CHECK(probeUserPackage(URL, readRange, &html, &out) == USER_CATALOG_ERROR_HEADER);
    Reader network; network.fail = true;
    snprintf(out.name, sizeof(out.name), "unchanged");
    CHECK(probeUserPackage(URL, readRange, &network, &out) == USER_CATALOG_ERROR_NETWORK);
    CHECK(!strcmp(out.name, "unchanged"));
    Reader sharefactory; be32(sharefactory.pkg, 0x74, 0x1B); be32(sharefactory.pkg, 0x98, 1);
    CHECK(probeUserPackage(URL, readRange, &sharefactory, &out) == USER_CATALOG_OK);
    CHECK(out.isTheme && out.displayCategory == 7);
    CHECK(!strcmp(out.requiresData, "Tema SHAREfactory; requer SHAREfactory para usar."));
    strcpy(out.requiresData, "Dependências informadas pelo autor.");
    CHECK(userCatalogPrepareEntry(&out) && !strcmp(out.requiresData, "Dependências informadas pelo autor."));
}
void checkMutatedMetadata() {
    // Adversarial byte mutations exercise lengths/offsets/keys/UTF-8 without
    // trusting the mock server to provide well-formed package metadata.
    Reader original; original.sfo(makeSfo("Mutation fixture português", "01.00"));
    uint32_t random = 0x6D657461U;
    for (unsigned i = 0; i < 1500; ++i) {
        Reader mutated = original;
        unsigned changes = 1 + i % 5;
        for (unsigned j = 0; j < changes; ++j) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            size_t area = i % 3 == 0 ? 0x500 : (i % 3 == 1 ? 0x600 : 0);
            size_t length = area == 0x500 ? 32 : (area == 0x600 ? makeSfo("Mutation fixture português", "01.00").size() : USER_PACKAGE_HEADER_BYTES);
            size_t position = area + random % length;
            mutated.pkg[position] ^= (unsigned char)(1 + ((random >> 16) % 255));
        }
        UserCatalogEntry out = {};
        int result = probeUserPackage(URL, readRange, &mutated, &out);
        CHECK(mutated.calls <= 3 && mutated.downloaded <= USER_CATALOG_MAX_METADATA_BYTES);
        if (result == USER_CATALOG_OK) {
            CHECK(userCatalogEntryValid(out));
            CHECK(out.sizeBytes == mutated.pkg.size());
            CHECK(!strchr(out.filename, '/') && !strchr(out.filename, '\\'));
        }
    }
}
void checkCatalog() {
    UserCatalog catalog;
    UserCatalogEntry base = entryFor();
    CHECK(catalog.add(base) == USER_CATALOG_OK);
    UserCatalogEntry clone = entryFor("https://another.cdn.com/mirror.pkg");
    CHECK(catalog.add(clone) == USER_CATALOG_ERROR_DUPLICATE);
    UserCatalogEntry update = entryFor("https://files.example.com/update.pkg", USER_PACKAGE_UPDATE);
    CHECK(catalog.add(update) == USER_CATALOG_OK);
    UserCatalogEntry revision = entryFor("https://files.example.com/update2.pkg", USER_PACKAGE_UPDATE, "01.01");
    CHECK(catalog.add(revision) == USER_CATALOG_OK);
    UserCatalogEntry unknown = entryFor("https://files.example.com/update3.pkg", USER_PACKAGE_UPDATE, 0);
    CHECK(catalog.add(unknown) == USER_CATALOG_ERROR_DUPLICATE);
    UserCatalogEntry inconsistent = base; strcpy(inconsistent.filename, "../../pkg");
    CHECK(catalog.add(inconsistent) == USER_CATALOG_ERROR_METADATA);
    inconsistent = base; memset(inconsistent.description, 'a', sizeof(inconsistent.description));
    CHECK(!userCatalogEntryValid(inconsistent));
    inconsistent = base; strcpy(inconsistent.sha256, "not-a-hash"); CHECK(!userCatalogEntryValid(inconsistent));
    inconsistent = base; inconsistent.source = USER_SOURCE_RECOGNIZED_AUTHOR_RELEASE; CHECK(!userCatalogEntryValid(inconsistent));
    inconsistent = base; inconsistent.displayCategory = 5; CHECK(!userCatalogEntryValid(inconsistent));
    inconsistent = entryFor("https://files.example.com/al.pkg", USER_PACKAGE_DLC);
    inconsistent.contentType = 0x1C; inconsistent.iroTag = 2; inconsistent.isTheme = true;
    CHECK(!userCatalogPrepareEntry(&inconsistent));
    UserCatalogEntry labelled = entryFor("https://files.example.com/release.pkg", USER_PACKAGE_BASE, "rarch-v1.22.2-15 / PS4");
    CHECK(!strchr(labelled.filename, '/') && strlen(labelled.filename) < sizeof(labelled.filename));
    CHECK(!strcmp(labelled.version, "rarch-v1.22.2-15 / PS4"));
    inconsistent = labelled; memset(inconsistent.version, 'x', sizeof(inconsistent.version)); CHECK(!userCatalogEntryValid(inconsistent));
    CHECK(catalog.count() == 3 && !catalog.at(3));
    CHECK(catalog.remove(1) && catalog.count() == 2 && !strcmp(catalog.at(1)->version, "01.01"));
    CHECK(!catalog.remove(2)); catalog.clear(); CHECK(!catalog.count());
    Reader reader;
    const char* list = " \thttps://files.example.com/one.pkg \r\nhttps://files.example.com/two.pkg\nhttp://invalid.com/file.pkg\n\n";
    UserCatalogImportReport report = catalog.importUrlList(list, strlen(list), readRange, &reader);
    CHECK(report.requested == 3 && report.added == 1 && report.duplicates == 1 && report.rejected == 1);
    CHECK(report.firstError == USER_CATALOG_ERROR_URL);
    const char badList[] = "https://files.example.com/a.pkg\0https://files.example.com/b.pkg";
    report = catalog.importUrlList(badList, sizeof(badList) - 1, readRange, &reader);
    CHECK(report.rejected == 1 && report.firstError == USER_CATALOG_ERROR_LIST);
    UserCatalog full;
    for (size_t i = 0; i < USER_CATALOG_MAX_ITEMS; ++i) {
        UserCatalogEntry entry = base;
        snprintf(entry.url, sizeof(entry.url), "https://files.example.com/%zu.pkg", i);
        snprintf(entry.contentId, sizeof(entry.contentId), "UP0000-CUSA%05zu_00-USERGAME00000000", i);
        CHECK(userCatalogPrepareEntry(&entry)); CHECK(full.add(entry) == USER_CATALOG_OK);
    }
    CHECK(full.count() == USER_CATALOG_MAX_ITEMS);
    Reader fullReader;
    CHECK(full.importUrl(URL, readRange, &fullReader) == USER_CATALOG_ERROR_FULL && fullReader.calls == 0);
}
std::vector<unsigned char> readFile(const char* path) {
    FILE* f = fopen(path, "rb"); CHECK(f != 0); CHECK(fseek(f, 0, SEEK_END) == 0);
    long bytes = ftell(f); CHECK(bytes >= 0); CHECK(fseek(f, 0, SEEK_SET) == 0);
    std::vector<unsigned char> data((size_t)bytes);
    CHECK(fread(data.data(), 1, data.size(), f) == data.size()); CHECK(fclose(f) == 0);
    return data;
}
void writeFile(const char* path, const std::vector<unsigned char>& data) {
    FILE* f = fopen(path, "wb"); CHECK(f != 0);
    CHECK(fwrite(data.data(), 1, data.size(), f) == data.size()); CHECK(fclose(f) == 0);
}
void checkPersistence() {
    char directory[] = "/tmp/peppy-user-catalog-data.XXXXXX"; CHECK(mkdtemp(directory) != 0);
    std::string path = std::string(directory) + "/catalog.dat";
    UserCatalog catalog;
    CHECK(catalog.load(path.c_str()) == USER_CATALOG_ERROR_NOT_FOUND);
    UserCatalogEntry entry = entryFor();
    strcpy(entry.description, "Descrição\ncom duas linhas"); strcpy(entry.requiresData, "Precisa dos seus arquivos autorizados.");
    memset(entry.sha256, 'a', 64); entry.sha256[64] = 0; entry.adult = true;
    CHECK(catalog.add(entry) == USER_CATALOG_OK); CHECK(catalog.save(path.c_str()) == USER_CATALOG_OK);
    struct stat info; CHECK(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
    UserCatalog loaded; CHECK(loaded.load(path.c_str()) == USER_CATALOG_OK && loaded.count() == 1);
    CHECK(!strcmp(loaded.at(0)->name, entry.name) && !strcmp(loaded.at(0)->description, entry.description));
    CHECK(!strcmp(loaded.at(0)->sha256, entry.sha256) && loaded.at(0)->adult);
#ifdef PEPPY_USER_CATALOG_NATIVE_FILE_ABI
    peppyTestCatalogFstatFailure(true);
    CHECK(loaded.load(path.c_str()) == USER_CATALOG_ERROR_FILE && loaded.count() == 1);
    CHECK(catalog.save(path.c_str()) == USER_CATALOG_ERROR_FILE);
    peppyTestCatalogFstatFailure(false);
    CHECK(loaded.load(path.c_str()) == USER_CATALOG_OK && loaded.count() == 1);
#endif
    std::vector<unsigned char> valid = readFile(path.c_str()), bad = valid;
    bad.back() ^= 1; writeFile(path.c_str(), bad);
    CHECK(loaded.load(path.c_str()) == USER_CATALOG_ERROR_FILE && loaded.count() == 1);
    bad = valid; bad[24] = USER_PACKAGE_UPDATE; le32(bad, 20, digest(bad, 24)); writeFile(path.c_str(), bad);
    CHECK(loaded.load(path.c_str()) == USER_CATALOG_ERROR_METADATA && loaded.count() == 1);
    bad = valid; le32(bad, 12, 1025); writeFile(path.c_str(), bad);
    CHECK(loaded.load(path.c_str()) == USER_CATALOG_ERROR_FILE && loaded.count() == 1);
    bad = valid; bad.push_back(0); writeFile(path.c_str(), bad);
    CHECK(loaded.load(path.c_str()) == USER_CATALOG_ERROR_FILE && loaded.count() == 1);
    std::string link = std::string(directory) + "/link.dat";
    CHECK(symlink(path.c_str(), link.c_str()) == 0);
    CHECK(catalog.save(link.c_str()) == USER_CATALOG_ERROR_FILE);
    CHECK(loaded.load(link.c_str()) == USER_CATALOG_ERROR_FILE);
    CHECK(catalog.save("/tmp/../escape.dat") == USER_CATALOG_ERROR_FILE);
    CHECK(unlink(link.c_str()) == 0); CHECK(unlink(path.c_str()) == 0); CHECK(rmdir(directory) == 0);
}
void checkUrlListFiles() {
    char directory[] = "/tmp/peppy-user-url-file.XXXXXX"; CHECK(mkdtemp(directory) != 0);
    std::string path = std::string(directory) + "/urls.txt";
    std::string link = std::string(directory) + "/link.txt";
    std::string fifo = std::string(directory) + "/pipe.txt";
    const char* text = "https://files.example.com/a.pkg\nhttps://files.example.com/b.pkg\n";
    std::vector<unsigned char> data(text, text + strlen(text));
    writeFile(path.c_str(), data);
    char* output = 0; size_t bytes = 0;
    CHECK(userCatalogReadUrlListFile(path.c_str(), &output, &bytes) == USER_CATALOG_OK);
    CHECK(bytes == strlen(text) && !strcmp(output, text)); free(output);
    CHECK(symlink(path.c_str(), link.c_str()) == 0);
    CHECK(userCatalogReadUrlListFile(link.c_str(), &output, &bytes) == USER_CATALOG_ERROR_FILE && !output && !bytes);
    CHECK(mkfifo(fifo.c_str(), 0600) == 0);
    CHECK(userCatalogReadUrlListFile(fifo.c_str(), &output, &bytes) == USER_CATALOG_ERROR_FILE && !output && !bytes);
    CHECK(userCatalogReadUrlListFile(directory, &output, &bytes) == USER_CATALOG_ERROR_FILE);
    data[4] = 0; writeFile(path.c_str(), data);
    CHECK(userCatalogReadUrlListFile(path.c_str(), &output, &bytes) == USER_CATALOG_ERROR_LIST && !output && !bytes);
    data.clear(); writeFile(path.c_str(), data);
    CHECK(userCatalogReadUrlListFile(path.c_str(), &output, &bytes) == USER_CATALOG_ERROR_LIST);
    int descriptor = open(path.c_str(), O_WRONLY); CHECK(descriptor >= 0);
    CHECK(ftruncate(descriptor, USER_CATALOG_MAX_LIST_BYTES + 1) == 0); CHECK(close(descriptor) == 0);
    CHECK(userCatalogReadUrlListFile(path.c_str(), &output, &bytes) == USER_CATALOG_ERROR_LIST && !output && !bytes);
    CHECK(unlink(fifo.c_str()) == 0); CHECK(unlink(link.c_str()) == 0); CHECK(unlink(path.c_str()) == 0); CHECK(rmdir(directory) == 0);
}
}
int main() {
    checkUrls(); checkHeaders(); checkProbe(); checkMutatedMetadata(); checkCatalog(); checkPersistence(); checkUrlListFiles();
    printf("user catalog: %u checks passed\n", checks);
    return 0;
}
