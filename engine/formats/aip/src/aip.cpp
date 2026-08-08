#include "aip/aip.h"

#include <string>

namespace opennova::aip {

ProfileSpeeds parse_profile_speeds(const uint8_t *text, size_t size) {
    // Line-oriented "<key> <value>" with tabs as spaces, keys compared
    // case-insensitively — the two witnessed AIProfile fields only.
    // [orig: AIProfile_ParseProperty "patrol_speed" @0x45E6DF..0x45E717 /
    //  "combat_speed" -> profile+0xC4]
    ProfileSpeeds row;
    const char *p = reinterpret_cast<const char *>(text);
    const std::size_t n = size;
    std::size_t i = 0;
    while (i < n) {
        std::size_t end = i;
        while (end < n && p[end] != '\n') ++end;
        // Tokenize the line on spaces/tabs/CR.
        std::string key;
        std::string value;
        std::size_t t = i;
        auto skip_ws = [&] {
            while (t < end && (p[t] == ' ' || p[t] == '\t' || p[t] == '\r')) ++t;
        };
        auto take_token = [&] {
            std::string tok;
            while (t < end && p[t] != ' ' && p[t] != '\t' && p[t] != '\r')
                tok.push_back(p[t++]);
            return tok;
        };
        skip_ws();
        key = take_token();
        skip_ws();
        value = take_token();
        if (!key.empty() && !value.empty()) {
            for (char &c : key)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            // atoi-shape numeric read: leading sign + digits, junk tails
            // ignored (the shell resolver's int(String) behaved the same for
            // the authored corpus).
            const auto parse_int = [](const std::string &s) {
                int32_t v = 0;
                bool neg = false;
                std::size_t k = 0;
                if (k < s.size() && (s[k] == '-' || s[k] == '+')) {
                    neg = s[k] == '-';
                    ++k;
                }
                for (; k < s.size() && s[k] >= '0' && s[k] <= '9'; ++k)
                    v = v * 10 + (s[k] - '0');
                return neg ? -v : v;
            };
            if (key == "patrol_speed") row.patrol_speed = parse_int(value);
            else if (key == "combat_speed") row.combat_speed = parse_int(value);
        }
        i = end + 1;
    }
    return row;
}

}  // namespace opennova::aip
