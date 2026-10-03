// ASCII case-insensitive string helpers.
//
// The shared home for the to_lower/iequals helpers the format libraries used
// to inline per-file. ASCII-only on purpose: NovaLogic asset names and keys
// are ASCII, and locale-dependent tolower would change matching behavior.

#pragma once

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova {
namespace strutil {

inline char ascii_tolower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

inline std::string to_lower(std::string_view s)
{
    std::string out(s);
    for (char &c : out)
        c = ascii_tolower(c);
    return out;
}

inline char ascii_toupper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

inline std::string to_upper(std::string_view s)
{
    std::string out(s);
    for (char &c : out)
        c = ascii_toupper(c);
    return out;
}

inline bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (ascii_tolower(a[i]) != ascii_tolower(b[i]))
            return false;
    return true;
}

// Strict weak ordering for case-insensitive map/set keys. Bytes compare
// UNSIGNED, as the CRT `_stricmp` the original sorts with does (a
// high-bit lead byte of a localized label sorts after every ASCII byte,
// never before it).
inline bool iless(std::string_view a, std::string_view b)
{
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = static_cast<unsigned char>(ascii_tolower(a[i]));
        const unsigned char cb = static_cast<unsigned char>(ascii_tolower(b[i]));
        if (ca != cb)
            return ca < cb;
    }
    return a.size() < b.size();
}

// Both-ends ASCII whitespace trim (the C-locale isspace set, spelled out so
// behavior never depends on the process locale).
inline std::string_view trim_view(std::string_view s)
{
    const char *ws = " \t\r\n\v\f";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string_view::npos)
        return std::string_view();
    const size_t e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

inline std::string trim(std::string_view s)
{
    return std::string(trim_view(s));
}

inline bool ends_with_icase(std::string_view s, std::string_view suffix)
{
    if (suffix.size() > s.size())
        return false;
    return iequals(s.substr(s.size() - suffix.size()), suffix);
}

inline bool starts_with_icase(std::string_view s, std::string_view prefix)
{
    if (prefix.size() > s.size())
        return false;
    return iequals(s.substr(0, prefix.size()), prefix);
}

// The text inside one pair of surrounding double quotes; anything else (no
// quotes, a quote on one side only, a lone '"') comes back unchanged. The
// .env and .trn value readers strip a quoted value this way.
inline std::string unquote(std::string_view s)
{
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return std::string(s.substr(1, s.size() - 2));
    return std::string(s);
}

// The text held in a fixed-width, NUL-padded on-disk field: up to max_len
// bytes, cut at the first NUL (a full-width field carries no terminator).
inline std::string fixed_string(const char *data, size_t max_len)
{
    size_t len = 0;
    while (len < max_len && data[len] != '\0')
        ++len;
    return std::string(data, len);
}

// Decode an even-length ASCII hex string (either case) into bytes; false on
// an odd length or a non-hex digit. The .hexcap capture format and the
// packet pretty-printer both read this shape.
inline bool hex_to_bytes(std::string_view hex, std::vector<uint8_t> &out)
{
    if (hex.size() % 2 != 0)
        return false;
    out.clear();
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

// Non-throwing std::stoi / std::stoul / std::stof: the same strtol / strtoul /
// strtof parse (leading whitespace, optional sign, longest numeric prefix, so
// "12abc" is 12), with nullopt exactly where the std:: form throws (no digits,
// or out of range). Parsers use these instead of try/catch around the std::
// form, which a build without exception catching (the web target) cannot run.
inline std::optional<int> parse_int(const std::string &s, int base = 10)
{
    const char *begin = s.c_str();
    char *end = nullptr;
    errno = 0;
    const long v = std::strtol(begin, &end, base);
    if (end == begin || errno == ERANGE || v < INT_MIN || v > INT_MAX)
        return std::nullopt;
    return static_cast<int>(v);
}

inline std::optional<unsigned long> parse_ulong(const std::string &s, int base = 10)
{
    const char *begin = s.c_str();
    char *end = nullptr;
    errno = 0;
    const unsigned long v = std::strtoul(begin, &end, base);
    if (end == begin || errno == ERANGE)
        return std::nullopt;
    return v;
}

inline std::optional<float> parse_float(const std::string &s)
{
    const char *begin = s.c_str();
    char *end = nullptr;
    errno = 0;
    const float v = std::strtof(begin, &end);
    if (end == begin || errno == ERANGE)
        return std::nullopt;
    return v;
}

// The 64-bit and double forms: std::stoll / std::stoull / std::stod without the throw.
inline std::optional<long long> parse_llong(const std::string &s, int base = 10)
{
    const char *begin = s.c_str();
    char *end = nullptr;
    errno = 0;
    const long long v = std::strtoll(begin, &end, base);
    if (end == begin || errno == ERANGE)
        return std::nullopt;
    return v;
}

inline std::optional<unsigned long long> parse_ullong(const std::string &s, int base = 10)
{
    const char *begin = s.c_str();
    char *end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(begin, &end, base);
    if (end == begin || errno == ERANGE)
        return std::nullopt;
    return v;
}

inline std::optional<double> parse_double(const std::string &s)
{
    const char *begin = s.c_str();
    char *end = nullptr;
    errno = 0;
    const double v = std::strtod(begin, &end);
    if (end == begin || errno == ERANGE)
        return std::nullopt;
    return v;
}

// Whether `s` is a whole number written in decimal digits alone (no sign, no blank, nothing
// after): the form an index or a count in a name or a locator takes.
inline bool all_digits(std::string_view s)
{
    if (s.empty())
        return false;
    for (const char c : s)
        if (c < '0' || c > '9')
            return false;
    return true;
}

} // namespace strutil
} // namespace opennova
