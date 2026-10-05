#ifndef PEPPY_HTTP_RANGE_H
#define PEPPY_HTTP_RANGE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Bounded parsing of libSceHttp's response-header span. This establishes HTTP
// range framing and validator equality; it does not authenticate PKG contents.
namespace peppyHttpRange {

static const size_t HEADER_CAP = 64 * 1024;
static const size_t ETAG_CAP = 256;

struct Metadata {
    uint64_t first, last, total, contentLength;
    char etag[ETAG_CAP];
    bool hasContentRange, hasContentLength, hasStrongEtag;
};

namespace detail {

inline char lower(char value) {
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

template <size_t N>
inline bool equalNoCase(const char* value, size_t length, const char (&literal)[N]) {
    if (length != N - 1) return false;
    for (size_t i = 0; i < length; ++i)
        if (lower(value[i]) != lower(literal[i])) return false;
    return true;
}

inline bool tokenCharacter(unsigned char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9') || value == '!' || value == '#' ||
        value == '$' || value == '%' || value == '&' || value == '\'' ||
        value == '*' || value == '+' || value == '-' || value == '.' ||
        value == '^' || value == '_' || value == '`' || value == '|' || value == '~';
}

inline bool safeValue(const char* value, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = static_cast<unsigned char>(value[i]);
        if (byte != '\t' && (byte < 32 || byte >= 127)) return false;
    }
    return true;
}

inline bool decimal(const char* value, size_t length, size_t* cursor, uint64_t* output) {
    size_t start = *cursor;
    uint64_t parsed = 0;
    while (*cursor < length && value[*cursor] >= '0' && value[*cursor] <= '9') {
        uint64_t digit = static_cast<uint64_t>(value[*cursor] - '0');
        if (parsed > (UINT64_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
        ++*cursor;
    }
    if (*cursor == start) return false;
    *output = parsed;
    return true;
}

inline bool contentRange(const char* value, size_t length, Metadata* output) {
    if (length < 6 || !equalNoCase(value, 5, "bytes") || value[5] != ' ') return false;
    size_t cursor = 6;
    uint64_t first = 0, last = 0, total = 0;
    if (!decimal(value, length, &cursor, &first) || cursor >= length || value[cursor++] != '-' ||
        !decimal(value, length, &cursor, &last) || cursor >= length || value[cursor++] != '/' ||
        !decimal(value, length, &cursor, &total) || cursor != length || first > last || last >= total)
        return false;
    output->first = first;
    output->last = last;
    output->total = total;
    output->hasContentRange = true;
    return true;
}

inline bool strongTag(const char* value, size_t length) {
    // RFC 9110's ASCII etagc subset: quoted opaque bytes, not a quoted-string
    // escape sequence. Preserve backslashes literally for exact If-Range use.
    if (length < 2 || length >= ETAG_CAP || value[0] != '"' || value[length - 1] != '"')
        return false;
    for (size_t i = 1; i + 1 < length; ++i) {
        unsigned char byte = static_cast<unsigned char>(value[i]);
        if (byte != 0x21 && (byte < 0x23 || byte > 0x7e)) return false;
    }
    return true;
}

inline bool statusLine(const char* value, size_t length) {
    return length >= 13 && (!memcmp(value, "HTTP/1.0 ", 9) || !memcmp(value, "HTTP/1.1 ", 9)) &&
        value[9] >= '1' && value[9] <= '5' && value[10] >= '0' && value[10] <= '9' &&
        value[11] >= '0' && value[11] <= '9' && value[12] == ' ' && safeValue(value + 13, length - 13);
}

inline size_t tagLength(const char* value) {
    size_t length = 0;
    while (length < ETAG_CAP && value[length]) ++length;
    return length;
}

} // namespace detail

inline bool parseHeaders(const char* headers, size_t length, Metadata* output) {
    if (!output) return false;
    *output = Metadata{};
    if (!headers || !length || length > HEADER_CAP) return false;
    // Native header lengths can include their terminating NUL. Only one NUL at
    // the end is tolerated; an embedded NUL or following frame is rejected.
    if (headers[length - 1] == '\0') --length;
    if (!length) return false;
    Metadata parsed = {};
    bool encodingSeen = false;
    size_t cursor = 0;
    while (cursor < length) {
        size_t start = cursor;
        while (cursor < length && headers[cursor] != '\r' && headers[cursor] != '\n') ++cursor;
        if (cursor == length || headers[cursor] != '\r' || length - cursor < 2 || headers[cursor + 1] != '\n')
            return false;
        size_t end = cursor;
        cursor += 2;
        if (start == end) {
            if (cursor != length) return false;
            break;
        }
        if (headers[start] == ' ' || headers[start] == '\t') return false;
        if (start == 0 && end - start >= 5 && !memcmp(headers + start, "HTTP/", 5)) {
            if (!detail::statusLine(headers + start, end - start)) return false;
            continue;
        }
        size_t colon = start;
        while (colon < end && headers[colon] != ':') {
            if (!detail::tokenCharacter(static_cast<unsigned char>(headers[colon]))) return false;
            ++colon;
        }
        if (colon == start || colon == end || !detail::safeValue(headers + colon + 1, end - colon - 1))
            return false;
        size_t nameLength = colon - start;
        size_t valueStart = colon + 1;
        while (valueStart < end && (headers[valueStart] == ' ' || headers[valueStart] == '\t')) ++valueStart;
        while (end > valueStart && (headers[end - 1] == ' ' || headers[end - 1] == '\t')) --end;
        const char* value = headers + valueStart;
        size_t valueLength = end - valueStart;
        if (detail::equalNoCase(headers + start, nameLength, "Content-Range")) {
            if (parsed.hasContentRange || !detail::contentRange(value, valueLength, &parsed)) return false;
        } else if (detail::equalNoCase(headers + start, nameLength, "ETag")) {
            if (parsed.hasStrongEtag || !detail::strongTag(value, valueLength)) return false;
            memcpy(parsed.etag, value, valueLength);
            parsed.etag[valueLength] = '\0';
            parsed.hasStrongEtag = true;
        } else if (detail::equalNoCase(headers + start, nameLength, "Content-Length")) {
            size_t numberCursor = 0;
            if (parsed.hasContentLength || !detail::decimal(value, valueLength, &numberCursor, &parsed.contentLength) ||
                numberCursor != valueLength) return false;
            parsed.hasContentLength = true;
        } else if (detail::equalNoCase(headers + start, nameLength, "Content-Encoding")) {
            if (encodingSeen || !detail::equalNoCase(value, valueLength, "identity")) return false;
            encodingSeen = true;
        } else if (detail::equalNoCase(headers + start, nameLength, "Transfer-Encoding")) {
            return false;
        }
    }
    if (parsed.hasContentRange && parsed.hasContentLength &&
        parsed.contentLength != parsed.last - parsed.first + 1) return false;
    *output = parsed;
    return true;
}

inline bool matchesRange(const Metadata& metadata, uint64_t first, uint64_t last, uint64_t total) {
    if (!metadata.hasContentRange || metadata.first > metadata.last || metadata.last >= metadata.total ||
        metadata.first != first || metadata.last != last || metadata.total != total) return false;
    return !metadata.hasContentLength || metadata.contentLength == metadata.last - metadata.first + 1;
}

inline bool sameStrongEtag(const Metadata& left, const Metadata& right) {
    if (!left.hasStrongEtag || !right.hasStrongEtag) return false;
    size_t leftLength = detail::tagLength(left.etag), rightLength = detail::tagLength(right.etag);
    return leftLength == rightLength && detail::strongTag(left.etag, leftLength) &&
        detail::strongTag(right.etag, rightLength) && !memcmp(left.etag, right.etag, leftLength);
}

} // namespace peppyHttpRange

#endif // PEPPY_HTTP_RANGE_H
