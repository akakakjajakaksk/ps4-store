// Standalone response-header and range-identity checks; no native mocks/network.
#include "../../http_range.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

using peppyHttpRange::Metadata;

static const char* GOLDEN =
    "HTTP/1.1 206 Partial Content\r\n"
    "Content-Range: bytes 0-1079/33554432\r\n"
    "Content-Length: 1080\r\n"
    "ETag: \"fixture-abc-2000000\"\r\n"
    "Content-Encoding: identity\r\n"
    "X-Archive-Orig: fixture\r\n\r\n";

static bool parse(const std::string& value, Metadata* metadata) {
    return peppyHttpRange::parseHeaders(value.data(), value.size(), metadata);
}

static void refused(const std::string& value) {
    Metadata metadata = {};
    metadata.hasContentRange = metadata.hasContentLength = metadata.hasStrongEtag = true;
    metadata.first = metadata.last = metadata.total = metadata.contentLength = 99;
    strcpy(metadata.etag, "\"stale\"");
    assert(!parse(value, &metadata));
    // Failed parsing cannot leave a prior valid range/tag visible to a caller.
    assert(!metadata.hasContentRange && !metadata.hasContentLength && !metadata.hasStrongEtag);
    assert(!metadata.first && !metadata.last && !metadata.total && !metadata.contentLength && !metadata.etag[0]);
}

static void goldenAndBindings() {
    Metadata metadata = {};
    assert(parse(GOLDEN, &metadata));
    assert(metadata.hasContentRange && metadata.hasContentLength && metadata.hasStrongEtag);
    assert(metadata.first == 0 && metadata.last == 1079 && metadata.total == 33554432);
    assert(metadata.contentLength == 1080 && !strcmp(metadata.etag, "\"fixture-abc-2000000\""));
    assert(peppyHttpRange::matchesRange(metadata, 0, 1079, 33554432));
    assert(!peppyHttpRange::matchesRange(metadata, 1, 1079, 33554432));
    assert(!peppyHttpRange::matchesRange(metadata, 0, 1078, 33554432));
    assert(!peppyHttpRange::matchesRange(metadata, 0, 1079, 33554433));

    // Header extraction from the initial full GET is useful before range setup.
    Metadata full = {};
    assert(parse("HTTP/1.1 200 OK\r\nContent-Length: 33554432\r\nETag: \"fixture-abc-2000000\"\r\n", &full));
    assert(!full.hasContentRange && full.hasContentLength && full.hasStrongEtag);
    assert(!peppyHttpRange::matchesRange(full, 0, 1079, 33554432));
    assert(peppyHttpRange::sameStrongEtag(metadata, full));
    Metadata changed = full;
    strcpy(changed.etag, "\"FIXTURE-abc-2000000\"");
    assert(!peppyHttpRange::sameStrongEtag(metadata, changed));
    changed = full; changed.hasStrongEtag = false;
    assert(!peppyHttpRange::sameStrongEtag(metadata, changed));
    changed = full; memset(changed.etag, 'x', sizeof(changed.etag));
    assert(!peppyHttpRange::sameStrongEtag(changed, changed));

    Metadata missing = {};
    assert(parse("Content-Range: bytes 0-1079/33554432\r\n", &missing));
    assert(peppyHttpRange::matchesRange(missing, 0, 1079, 33554432));
    assert(!missing.hasStrongEtag && !missing.hasContentLength);
    assert(!peppyHttpRange::sameStrongEtag(metadata, missing));

    assert(parse("content-range:\t BYTES 00-01079/033554432 \t\r\n"
                 "cOnTeNt-LeNgTh: 01080\r\neTAG:\t\"fixture-abc-2000000\" \t\r\n"
                 "content-encoding: IDENTITY\r\nX-Repeated: first\r\nX-Repeated: second\r\n", &changed));
    assert(peppyHttpRange::matchesRange(changed, 0, 1079, 33554432));
    assert(peppyHttpRange::sameStrongEtag(metadata, changed));
}

static void malformedRangesAndFraming() {
    const char* values[] = {
        "", "bytes", "bytes ", "items 0-1079/33554432", "bytes\t0-1079/33554432",
        "bytes  0-1079/33554432", "bytes */33554432", "bytes 0-1079/*",
        "bytes 0-1079/0", "bytes 0-1079/1079", "bytes 1080-1079/33554432",
        "bytes -1-1079/33554432", "bytes +0-1079/33554432", "bytes 0--1/33554432",
        "bytes 0-1079/+33554432", "bytes 0-1079/-33554432", "bytes 0-1079/33554432tail",
        "bytes 0-1079/33554432,1080-2000/33554432", "bytes 0-1079/33554432 0",
        "bytes 0 -1079/33554432", "bytes 0-1079 /33554432", "bytes 0-1079/ 33554432",
        "bytes 18446744073709551616-18446744073709551617/18446744073709551618",
        "bytes 0-18446744073709551616/18446744073709551617",
        "bytes 0-1079/18446744073709551616", "bytes 0-18446744073709551615/18446744073709551615",
    };
    for (const char* value : values) refused(std::string("Content-Range: ") + value + "\r\n");
    const char* lengths[] = { "", "-1", "+1080", "1080tail", "1 080", "1080,1080", "18446744073709551616" };
    for (const char* value : lengths) refused(std::string("Content-Length: ") + value + "\r\n");
    refused("Content-Range: bytes 0-1079/33554432\r\nContent-Length: 1079\r\n");
    refused("Content-Range: bytes 0-1079/33554432\r\nContent-Length: 1081\r\n");
    refused("Content-Range: bytes 0-1079/33554432\r\ncontent-range: bytes 0-1079/33554432\r\n");
    refused("Content-Range: bytes 0-1079/33554432\r\nContent-Range: bytes 1080-2000/33554432\r\n");
    refused("Content-Length: 1080\r\ncontent-length: 1080\r\n");
    refused("Content-Encoding: identity\r\nContent-Encoding: identity\r\n");
    for (const char* encoding : { "gzip", "br", "identity, gzip", "", "identity extra" })
        refused(std::string("Content-Encoding: ") + encoding + "\r\n");
    for (const char* encoding : { "chunked", "identity", "" })
        refused(std::string("Transfer-Encoding: ") + encoding + "\r\n");
}

static void strongTags() {
    Metadata metadata = {};
    for (const char* tag : { "\"\"", "\"!#$%&'()*+,-./0123456789:;<=>?@ABC[]^_`abc{|}~\"", "\"a\\b\"", "\"W/inside\"" }) {
        assert(parse(std::string("ETag: ") + tag + "\r\n", &metadata));
        assert(metadata.hasStrongEtag && !strcmp(metadata.etag, tag));
        assert(peppyHttpRange::sameStrongEtag(metadata, metadata));
    }
    for (const char* tag : { "", "opaque", "W/\"opaque\"", "w/\"opaque\"", "\"unterminated", "no-start\"",
                             "\"a b\"", "\"a\tb\"", "\"a\"b\"", "\"a\"extra", "\"a\" \"b\"" })
        refused(std::string("ETag: ") + tag + "\r\n");
    refused("ETag: \"same\"\r\netag: \"same\"\r\n");
    refused("ETag: \"same\"\r\nETag: \"other\"\r\n");
    refused(std::string("ETag: \"") + std::string(1, '\x7f') + "\"\r\n");
    refused(std::string("ETag: \"") + std::string(1, '\x80') + "\"\r\n");
    const std::string largest = "\"" + std::string(peppyHttpRange::ETAG_CAP - 3, 'a') + "\"";
    assert(largest.size() == peppyHttpRange::ETAG_CAP - 1);
    assert(parse("ETag: " + largest + "\r\n", &metadata));
    assert(strlen(metadata.etag) == largest.size());
    refused("ETag: \"" + std::string(peppyHttpRange::ETAG_CAP - 2, 'a') + "\"\r\n");
}

static void boundedHeadersAndSyntax() {
    for (const char* headers : {
        "Content-Range: bytes 0-1079/33554432\n", "Content-Range: bytes 0-1079/33554432\r",
        "Content-Range: bytes 0-1079/33554432", "Content-Range : bytes 0-1079/33554432\r\n",
        " Content-Range: bytes 0-1079/33554432\r\n", "\tContent-Range: bytes 0-1079/33554432\r\n",
        "X-Test: first\r\n continuation\r\n", "X-Test: first\r\n\tcontinuation\r\n",
        "Missing-Colon\r\n", ": empty-name\r\n", "Bad(Name): value\r\n", "X-Test: a\nb\r\n",
        "HTTP/1.1 206 Partial Content\r\nHTTP/1.1 206 Other Frame\r\n",
        "HTTP/1.2 206 Partial Content\r\n", "HTTP/1.1 20 Partial Content\r\n",
        "HTTP/1.1 206Partial Content\r\n", "HTTP/1.1 999 Wrong\r\n",
        "Content-Range: bytes 0-1079/33554432\r\n\r\nETag: \"cross-frame\"\r\n",
        "Content-Range: bytes 0-1079/33554432\r\n\r\nbody", "\r\nHTTP/1.1 206 Other Frame\r\n",
    }) refused(headers);
    for (char bad : { '\0', '\x01', '\x1f', '\x7f', static_cast<char>(0x80), static_cast<char>(0xff) })
        refused(std::string("X-Unknown: a") + bad + "b\r\n");
    std::string nul = GOLDEN;
    nul += '\0';
    Metadata metadata = {};
    assert(parse(nul, &metadata) && peppyHttpRange::matchesRange(metadata, 0, 1079, 33554432));
    refused(nul + '\0');
    refused(std::string(GOLDEN) + '\0' + "X-Later: value\r\n");
    assert(!peppyHttpRange::parseHeaders(0, 10, &metadata));
    assert(!peppyHttpRange::parseHeaders(GOLDEN, strlen(GOLDEN), 0));
    refused("");

    std::string exact = "Content-Range: bytes 0-1079/33554432\r\nETag: \"range-boundary\"\r\nX-Pad: ";
    exact += std::string(peppyHttpRange::HEADER_CAP - exact.size() - 2, 'a') + "\r\n";
    assert(exact.size() == peppyHttpRange::HEADER_CAP);
    assert(parse(exact, &metadata) && peppyHttpRange::matchesRange(metadata, 0, 1079, 33554432));
    refused(exact.substr(0, exact.size() - 2) + "a\r\n");
    // The parser respects a caller-supplied span and never needs an adjacent NUL.
    std::string bounded = "Content-Range: bytes 0-0/1\r\nUNREAD suffix";
    const size_t span = bounded.find("UNREAD");
    assert(peppyHttpRange::parseHeaders(bounded.data(), span, &metadata));
    assert(peppyHttpRange::matchesRange(metadata, 0, 0, 1));
    const std::string rangeLine = "Content-Range: bytes 0-1079/33554432\r\n";
    for (size_t cut = 0; cut < rangeLine.size(); ++cut) {
        assert(!peppyHttpRange::parseHeaders(rangeLine.data(), cut, &metadata));
        assert(!metadata.hasContentRange && !metadata.hasStrongEtag);
    }
}

static void fullWidthAndAdjacentParts() {
    Metadata high = {};
    assert(parse("Content-Range: bytes 4294967296-5368709242/549755813888\r\n"
                 "Content-Length: 1073741947\r\nETag: \"wide\"\r\n", &high));
    assert(peppyHttpRange::matchesRange(high, UINT64_C(4294967296), UINT64_C(5368709242), UINT64_C(549755813888)));
    assert(parse("Content-Range: bytes 18446744073709551613-18446744073709551614/18446744073709551615\r\n"
                 "Content-Length: 2\r\n", &high));
    assert(peppyHttpRange::matchesRange(high, UINT64_MAX - 2, UINT64_MAX - 1, UINT64_MAX));

    // Odd-sized two-part fixtures cover every byte exactly once. Response tags
    // are bound independently of package CID/size and must compare exactly.
    Metadata first = {}, second = {};
    assert(parse("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-16777215/33554433\r\n"
                 "Content-Length: 16777216\r\nETag: \"one-revision\"\r\n\r\n", &first));
    assert(parse("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 16777216-33554432/33554433\r\n"
                 "Content-Length: 16777217\r\nETag: \"one-revision\"\r\n\r\n", &second));
    assert(peppyHttpRange::matchesRange(first, 0, 16777215, 33554433));
    assert(peppyHttpRange::matchesRange(second, 16777216, 33554432, 33554433));
    assert(first.last + 1 == second.first && second.last + 1 == first.total);
    assert(peppyHttpRange::sameStrongEtag(first, second));
    assert(!peppyHttpRange::matchesRange(second, first.last, second.last, second.total));
    strcpy(second.etag, "\"other-revision\"");
    assert(!peppyHttpRange::sameStrongEtag(first, second));
}

int main() {
    goldenAndBindings();
    malformedRangesAndFraming();
    strongTags();
    boundedHeadersAndSyntax();
    fullWidthAndAdjacentParts();
    puts("HTTP range parser tests passed");
}
