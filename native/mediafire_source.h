#ifndef PEPPY_MEDIAFIRE_SOURCE_H
#define PEPPY_MEDIAFIRE_SOURCE_H

#include <stddef.h>

// Pure, bounded helpers. No network access, allocation, or JavaScript evaluation.
namespace peppyMediafire {

static constexpr size_t URL_CAP = 4096;
static constexpr size_t HTML_CAP = 1024 * 1024;
static const size_t kMaxUrlLength = URL_CAP - 1;
static const size_t kMaxHtmlLength = HTML_CAP;

namespace detail {
inline bool alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
inline bool digit(char c) { return c >= '0' && c <= '9'; }
inline bool alnum(char c) { return alpha(c) || digit(c); }
inline char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; }
inline bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}
inline int hex(char c) {
    if (digit(c)) return c - '0';
    c = lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
inline bool same(const char* s, size_t n, const char* literal, size_t m,
                 bool insensitive = false) {
    if (n != m) return false;
    for (size_t i = 0; i < n; ++i)
        if ((insensitive ? lower(s[i]) : s[i]) != literal[i]) return false;
    return true;
}
inline bool starts(const char* s, size_t n, size_t at, const char* literal, size_t m,
                   bool insensitive = false) {
    return at <= n && m <= n - at && same(s + at, m, literal, m, insensitive);
}
inline bool urlByte(unsigned char c) {
    return c >= 0x21 && c <= 0x7e && c != '#' && c != '\\' &&
           c != '"' && c != '<' && c != '>' && c != '`';
}
inline bool authority(const char* url, size_t n, size_t* end) {
    if (!url || n > kMaxUrlLength || !starts(url, n, 0, "https://", 8)) return false;
    size_t i = 8;
    for (; i < n && url[i] != '/'; ++i)
        if (!alnum(url[i]) && url[i] != '.' && url[i] != '-') return false;
    if (i == 8 || i == n) return false;
    for (size_t j = i; j < n; ++j) {
        if (!urlByte(static_cast<unsigned char>(url[j]))) return false;
        if (url[j] == '%') {
            if (n - j < 3 || hex(url[j + 1]) < 0 || hex(url[j + 2]) < 0) return false;
            const unsigned int decoded = unsigned(hex(url[j + 1]) * 16 + hex(url[j + 2]));
            if (decoded < 0x20 || decoded == 0x7f || decoded == '\\') return false;
            j += 2;
        }
    }
    *end = i;
    return true;
}
inline bool attrNameByte(char c) {
    return alnum(c) || c == '-' || c == '_' || c == ':';
}
inline bool tagNameByte(char c) { return attrNameByte(c); }

// Attribute values are kept as spans so only the selected href is decoded.
struct Anchor {
    const char* id;
    size_t idLength;
    const char* href;
    size_t hrefLength;
    bool idQuoted;
    bool hrefQuoted;
    Anchor() : id(0), idLength(0), href(0), hrefLength(0),
               idQuoted(false), hrefQuoted(false) {}
};

inline bool tagEnd(const char* html, size_t n, size_t at, size_t* after) {
    char quote = 0;
    for (size_t i = at; i < n; ++i) {
        const char c = html[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            *after = i + 1;
            return true;
        } else if (c == '<') {
            return false;
        }
    }
    return false;
}

inline bool anchorAttrs(const char* html, size_t n, size_t at, size_t* after,
                        Anchor* anchor) {
    size_t i = at;
    bool idSeen = false, hrefSeen = false;
    while (i < n) {
        const size_t beforeSpace = i;
        while (i < n && space(html[i])) ++i;
        if (i == n) return false;
        if (html[i] == '>') { *after = i + 1; return true; }
        if (html[i] == '/' && i + 1 < n && html[i + 1] == '>') {
            *after = i + 2;
            return true;
        }
        // HTML attributes need a separator; do not reinterpret glued tokens.
        if (i == beforeSpace) return false;
        const size_t nameStart = i;
        while (i < n && attrNameByte(html[i])) ++i;
        if (nameStart == i) return false;
        const bool isId = same(html + nameStart, i - nameStart, "id", 2, true);
        const bool isHref = same(html + nameStart, i - nameStart, "href", 4, true);
        if ((isId && idSeen) || (isHref && hrefSeen)) return false;
        if (isId) idSeen = true;
        if (isHref) hrefSeen = true;
        const size_t afterName = i;
        while (i < n && space(html[i])) ++i;
        const char* value = html + i;
        size_t valueLength = 0;
        bool quoted = false;
        if (i < n && html[i] == '=') {
            ++i;
            while (i < n && space(html[i])) ++i;
            if (i == n) return false;
            const char quote = html[i];
            if (quote == '"' || quote == '\'') {
                quoted = true;
                const size_t valueStart = ++i;
                while (i < n && html[i] != quote) {
                    if (html[i] == '<') return false;
                    ++i;
                }
                if (i == n) return false;
                value = html + valueStart;
                valueLength = i - valueStart;
                ++i;
            } else {
                const size_t valueStart = i;
                while (i < n && !space(html[i]) && html[i] != '>') {
                    if (html[i] == '<' || html[i] == '"' || html[i] == '\'' || html[i] == '=' || html[i] == '`')
                        return false;
                    ++i;
                }
                value = html + valueStart;
                valueLength = i - valueStart;
                if (!valueLength) return false;
            }
        } else {
            i = afterName;
        }
        if (isId) { anchor->id = value; anchor->idLength = valueLength; anchor->idQuoted = quoted; }
        if (isHref) { anchor->href = value; anchor->hrefLength = valueLength; anchor->hrefQuoted = quoted; }
    }
    return false;
}

inline bool decodeHref(const char* s, size_t n, char* out, size_t* written) {
    size_t used = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (c == '&') {
            if (starts(s, n, i, "&amp;", 5)) { c = '&'; i += 4; }
            else {
                // Raw query separators such as &token= are allowed. Other
                // entities, including numeric and semicolonless entities,
                // are not interpreted as URL bytes.
                if (i + 1 < n && s[i + 1] == '#') return false;
                size_t j = i + 1;
                while (j < n && (alnum(s[j]) || s[j] == '-' || s[j] == '_' ||
                                s[j] == '.' || s[j] == '~' || s[j] == '%')) ++j;
                if (j > i + 1 && (j == n || s[j] != '=')) return false;
            }
        }
        if (used == kMaxUrlLength) return false;
        out[used++] = c;
    }
    out[used] = 0;
    *written = used;
    return true;
}

inline bool rawTag(const char* name, size_t n) {
    return same(name, n, "script", 6, true) || same(name, n, "style", 5, true) ||
           same(name, n, "textarea", 8, true) || same(name, n, "title", 5, true) ||
           same(name, n, "xmp", 3, true) || same(name, n, "iframe", 6, true) ||
           same(name, n, "noembed", 7, true) || same(name, n, "noframes", 8, true) ||
           same(name, n, "noscript", 8, true);
}
inline bool skipRaw(const char* html, size_t n, size_t at,
                    const char* name, size_t nameLength, size_t* after) {
    const bool script = same(name, nameLength, "script", 6, true);
    // HTML script data can enter a double-escaped state after <!-- <script.
    // Its first </script> then changes state without closing the element.
    int scriptState = 0; // normal, escaped, double escaped
    for (size_t i = at; i < n; ++i) {
        if (script) {
            if (scriptState && starts(html, n, i, "-->", 3)) {
                scriptState = 0;
                i += 2;
                continue;
            }
            if (!scriptState && starts(html, n, i, "<!--", 4)) {
                scriptState = 1;
                i += 3;
                continue;
            }
            if (scriptState == 1 && starts(html, n, i, "<script", 7, true) &&
                n - i > 7 && (space(html[i + 7]) || html[i + 7] == '/' || html[i + 7] == '>')) {
                scriptState = 2;
                i += 6;
                continue;
            }
        }
        if (html[i] != '<' || n - i < nameLength + 3 || html[i + 1] != '/') continue;
        bool match = true;
        for (size_t j = 0; j < nameLength; ++j)
            if (lower(html[i + 2 + j]) != lower(name[j])) { match = false; break; }
        if (!match) continue;
        const size_t end = i + 2 + nameLength;
        if (html[end] != '>' && !space(html[end])) continue;
        if (scriptState == 2) {
            scriptState = 1;
            i = end - 1;
            continue;
        }
        return tagEnd(html, n, end, after);
    }
    return false;
}
} // namespace detail

inline bool isPageUrl(const char* url, size_t length, size_t* originLength = 0) {
    if (originLength) *originLength = 0;
    size_t origin = 0;
    if (!detail::authority(url, length, &origin)) return false;
    const char* host = url + 8;
    const size_t hostLength = origin - 8;
    if (!detail::same(host, hostLength, "mediafire.com", 13, true) &&
        !detail::same(host, hostLength, "www.mediafire.com", 17, true)) return false;
    if (!detail::starts(url, length, origin, "/file/", 6)) return false;
    size_t i = origin + 6;
    const size_t keyStart = i;
    while (i < length && detail::alnum(url[i])) ++i;
    if (i == keyStart || i == length || url[i] != '/') return false;
    const size_t filenameStart = ++i;
    char tail[4] = {0, 0, 0, 0};
    size_t decodedLength = 0;
    bool onlyDots = true;
    for (; i < length && url[i] != '/'; ++i) {
        unsigned char c = static_cast<unsigned char>(url[i]);
        if (c == '%') {
            // authority() has already checked percent-triplet bounds.
            c = static_cast<unsigned char>(detail::hex(url[i + 1]) * 16 + detail::hex(url[i + 2]));
            i += 2;
        } else if (!detail::alnum(char(c)) && c != '.' && c != '-' && c != '_' &&
                   c != '~' && c != '[' && c != ']') {
            return false;
        }
        if (c < 0x20 || c >= 0x7f || c == '/' || c == '\\') return false;
        if (c != '.') onlyDots = false;
        tail[decodedLength % 4] = detail::lower(char(c));
        ++decodedLength;
    }
    if (i == filenameStart || onlyDots || decodedLength <= 4 ||
        !detail::starts(url, length, i, "/file", 5) || length - i != 5) return false;
    const char extension[4] = {'.', 'p', 'k', 'g'};
    for (size_t j = 0; j < 4; ++j)
        if (tail[(decodedLength - 4 + j) % 4] != extension[j]) return false;
    if (originLength) *originLength = origin;
    return true;
}

inline bool isCdnUrl(const char* url, size_t length, size_t* originLength = 0) {
    if (originLength) *originLength = 0;
    size_t origin = 0;
    if (!detail::authority(url, length, &origin)) return false;
    const char* host = url + 8;
    const size_t hostLength = origin - 8;
    if (!detail::starts(host, hostLength, 0, "download", 8, true)) return false;
    size_t i = 8;
    while (i < hostLength && detail::digit(host[i])) ++i;
    if (i == 8 || i > 63 || !detail::same(host + i, hostLength - i, ".mediafire.com", 14, true)) return false;
    // The path itself must contain a byte before an optional query.
    if (length - origin < 2 || url[origin + 1] == '?') return false;
    if (originLength) *originLength = origin;
    return true;
}

inline bool extractUrl(const char* html, size_t length, char* out, size_t capacity) {
    if (!out || !capacity) return false;
    out[0] = 0;
    if (!html || !length || length > kMaxHtmlLength) return false;
    for (size_t j = 0; j < length; ++j) if (!html[j]) return false;
    char candidate[kMaxUrlLength + 1];
    size_t candidateLength = 0;
    bool found = false;
    size_t templateDepth = 0;
    size_t i = 0;
    while (i < length) {
        if (html[i] != '<') { ++i; continue; }
        if (detail::starts(html, length, i, "<!--", 4)) {
            i += 4;
            while (i < length && !detail::starts(html, length, i, "-->", 3)) ++i;
            if (i == length) return false;
            i += 3;
            continue;
        }
        if (i + 1 == length) return false;
        if (html[i + 1] == '/') {
            size_t closingEnd = i + 2;
            while (closingEnd < length && detail::tagNameByte(html[closingEnd])) ++closingEnd;
            if (templateDepth && detail::same(html + i + 2, closingEnd - i - 2, "template", 8, true) &&
                closingEnd < length && (html[closingEnd] == '>' || detail::space(html[closingEnd])))
                --templateDepth;
            if (!detail::tagEnd(html, length, closingEnd, &i)) return false;
            continue;
        }
        if (html[i + 1] == '!' || html[i + 1] == '?') {
            if (!detail::tagEnd(html, length, i + 2, &i)) return false;
            continue;
        }
        size_t nameStart = i + 1;
        if (!detail::alpha(html[nameStart])) { ++i; continue; }
        size_t nameEnd = nameStart;
        while (nameEnd < length && detail::tagNameByte(html[nameEnd])) ++nameEnd;
        if (nameEnd == length || (!detail::space(html[nameEnd]) && html[nameEnd] != '>' && html[nameEnd] != '/'))
            return false;
        const size_t nameLength = nameEnd - nameStart;
        if (detail::same(html + nameStart, nameLength, "a", 1, true)) {
            if (templateDepth) {
                if (!detail::tagEnd(html, length, nameEnd, &i)) return false;
                continue;
            }
            detail::Anchor anchor;
            if (!detail::anchorAttrs(html, length, nameEnd, &i, &anchor)) return false;
            if (anchor.id && detail::same(anchor.id, anchor.idLength, "downloadButton", 14)) {
                if (found || !anchor.idQuoted || !anchor.hrefQuoted || !anchor.href ||
                    !detail::decodeHref(anchor.href, anchor.hrefLength, candidate, &candidateLength) ||
                    !isCdnUrl(candidate, candidateLength)) return false;
                found = true;
            }
        } else {
            if (!detail::tagEnd(html, length, nameEnd, &i)) return false;
            if (detail::same(html + nameStart, nameLength, "template", 8, true)) ++templateDepth;
            if (detail::rawTag(html + nameStart, nameLength) &&
                !detail::skipRaw(html, length, i, html + nameStart, nameLength, &i)) return false;
            if (detail::same(html + nameStart, nameLength, "plaintext", 9, true)) return false;
        }
    }
    if (!found || templateDepth || candidateLength >= capacity) return false;
    for (size_t j = 0; j <= candidateLength; ++j) out[j] = candidate[j];
    return true;
}

} // namespace peppyMediafire

#endif
