#ifndef PEPPY_HUB_JSON_H
#define PEPPY_HUB_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// A bounded JSON cursor, used for the compact hub protocol. No allocations,
// permissive numbers, embedded NULs or trailing data; nesting is capped at 16.
namespace peppyHubJson {
struct Cursor {
    const char* p;
    const char* end;
    Cursor(const char* data, size_t bytes) : p(data), end(data + bytes) {}
    void spaces() { while (p < end && (*p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')) ++p; }
    bool take(char ch) { spaces(); if (p == end || *p != ch) return false; ++p; return true; }
    bool done() { spaces(); return p == end; }
    static int hex(char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    }
    bool codeUnit(uint32_t& n) {
        if (end - p < 4) return false;
        n = 0;
        for (int i = 0; i < 4; ++i) { int h = hex(*p++); if (h < 0) return false; n = (n << 4) | uint32_t(h); }
        return true;
    }
    static bool append(char* output, size_t capacity, size_t& used, unsigned char ch) {
        if (output) { if (used + 1 >= capacity) return false; output[used] = char(ch); }
        ++used; return true;
    }
    bool string(char* output, size_t capacity) {
        if (!take('"')) return false;
        size_t used = 0;
        while (p < end) {
            unsigned char ch = (unsigned char)*p++;
            if (ch == '"') { if (output) output[used] = 0; return true; }
            if (ch < 32) return false;
            if (ch != '\\') {
                if (ch < 128) { if (!append(output, capacity, used, ch)) return false; continue; }
                // Validate shortest-form UTF-8 including surrogate/upper bounds.
                unsigned count = ch >= 0xc2 && ch <= 0xdf ? 1 : ch >= 0xe0 && ch <= 0xef ? 2 : ch >= 0xf0 && ch <= 0xf4 ? 3 : 0;
                if (!count || size_t(end - p) < count) return false;
                uint32_t code = ch & (count == 1 ? 0x1f : count == 2 ? 0x0f : 0x07);
                const char* tail = p;
                for (unsigned i = 0; i < count; ++i) {
                    unsigned char c = (unsigned char)*p++;
                    if ((c & 0xc0) != 0x80) return false;
                    code = (code << 6) | (c & 63);
                }
                if (code < (count == 1 ? 0x80U : count == 2 ? 0x800U : 0x10000U) ||
                    code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
                if (!append(output, capacity, used, ch)) return false;
                for (unsigned i = 0; i < count; ++i)
                    if (!append(output, capacity, used, (unsigned char)tail[i])) return false;
                continue;
            }
            if (p == end) return false;
            char esc = *p++;
            if (esc == '"' || esc == '\\' || esc == '/') ch = (unsigned char)esc;
            else if (esc == 'b') ch = '\b'; else if (esc == 'f') ch = '\f';
            else if (esc == 'n') ch = '\n'; else if (esc == 'r') ch = '\r'; else if (esc == 't') ch = '\t';
            else if (esc == 'u') {
                uint32_t code = 0;
                if (!codeUnit(code) || !code) return false;
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
                    p += 2; uint32_t low = 0;
                    if (!codeUnit(low) || low < 0xdc00 || low > 0xdfff) return false;
                    code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                } else if (code >= 0xdc00 && code <= 0xdfff) return false;
                if (code < 128) { if (!append(output, capacity, used, (unsigned char)code)) return false; }
                else {
                    if (code < 0x800) { if (!append(output, capacity, used, 0xc0 | (code >> 6))) return false; }
                    else if (code < 0x10000) {
                        if (!append(output, capacity, used, 0xe0 | (code >> 12)) ||
                            !append(output, capacity, used, 0x80 | ((code >> 6) & 63))) return false;
                    } else {
                        if (!append(output, capacity, used, 0xf0 | (code >> 18)) ||
                            !append(output, capacity, used, 0x80 | ((code >> 12) & 63)) ||
                            !append(output, capacity, used, 0x80 | ((code >> 6) & 63))) return false;
                    }
                    if (!append(output, capacity, used, 0x80 | (code & 63))) return false;
                }
                continue;
            } else return false;
            if (!ch || !append(output, capacity, used, ch)) return false;
        }
        return false;
    }
    bool number(uint64_t& output) {
        spaces(); if (p == end || *p < '0' || *p > '9') return false;
        bool zero = *p == '0'; output = 0; const char* start = p;
        while (p < end && *p >= '0' && *p <= '9') {
            uint64_t digit = uint64_t(*p++ - '0');
            if (output > (UINT64_MAX - digit) / 10) return false;
            output = output * 10 + digit;
        }
        return !zero || p - start == 1;
    }
    bool literal(const char* value) {
        spaces(); size_t n = strlen(value); if (size_t(end - p) < n || memcmp(p, value, n)) return false;
        p += n; return true;
    }
    bool boolean(bool& output) { if (literal("true")) { output = true; return true; } if (literal("false")) { output = false; return true; } return false; }
    bool skip(unsigned depth = 0) {
        spaces(); if (p == end || depth > 16) return false;
        if (*p == '"') return string(0, 0);
        if (*p == '{' || *p == '[') {
            bool object = *p++ == '{'; char close = object ? '}' : ']';
            if (take(close)) return true;
            do {
                if (object && (!string(0, 0) || !take(':'))) return false;
                if (!skip(depth + 1)) return false;
                if (take(close)) return true;
            } while (take(','));
            return false;
        }
        if (*p == 't') return literal("true");
        if (*p == 'f') return literal("false");
        if (*p == 'n') return literal("null");
        // Unknown JSON numeric fields may contain signed/fractional values.
        if (*p == '-') ++p;
        uint64_t n = 0; if (!number(n)) return false;
        if (p < end && *p == '.') { ++p; if (p == end || *p < '0' || *p > '9') return false; while (p < end && *p >= '0' && *p <= '9') ++p; }
        if (p < end && (*p == 'e' || *p == 'E')) { ++p; if (p < end && (*p == '+' || *p == '-')) ++p; if (p == end || *p < '0' || *p > '9') return false; while (p < end && *p >= '0' && *p <= '9') ++p; }
        return true;
    }
};

inline bool escape(const char* input, size_t bytes, char* output, size_t capacity, size_t& used) {
    const char* digits = "0123456789abcdef";
    for (size_t i = 0; i < bytes; ++i) {
        unsigned char ch = (unsigned char)input[i];
        if (!ch) return false;
        if (ch == '"' || ch == '\\') {
            if (used + 2 >= capacity) return false;
            output[used++] = '\\'; output[used++] = char(ch);
        } else if (ch < 32) {
            if (used + 6 >= capacity) return false;
            memcpy(output + used, "\\u00", 4); used += 4;
            output[used++] = digits[ch >> 4]; output[used++] = digits[ch & 15];
        } else { if (used + 1 >= capacity) return false; output[used++] = char(ch); }
    }
    output[used] = 0; return true;
}
} // namespace peppyHubJson
#endif
