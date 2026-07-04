// ASCII case-insensitive string helpers.
//
// The shared home for the to_lower/iequals helpers the format libraries used
// to inline per-file. ASCII-only on purpose: NovaLogic asset names and keys
// are ASCII, and locale-dependent tolower would change matching behavior.

#ifndef OPENNOVA_IO_STRUTIL_H
#define OPENNOVA_IO_STRUTIL_H

#include <string>
#include <string_view>

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

inline bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (ascii_tolower(a[i]) != ascii_tolower(b[i]))
            return false;
    return true;
}

// Strict weak ordering for case-insensitive map/set keys.
inline bool iless(std::string_view a, std::string_view b)
{
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) {
        const char ca = ascii_tolower(a[i]);
        const char cb = ascii_tolower(b[i]);
        if (ca != cb)
            return ca < cb;
    }
    return a.size() < b.size();
}

inline bool ends_with_icase(std::string_view s, std::string_view suffix)
{
    if (suffix.size() > s.size())
        return false;
    return iequals(s.substr(s.size() - suffix.size()), suffix);
}

} // namespace strutil
} // namespace opennova

#endif // OPENNOVA_IO_STRUTIL_H
