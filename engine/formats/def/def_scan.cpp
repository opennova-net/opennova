#include "def_scan.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include <base/io/ascii_config.h>
#include <formats/def/def.h> // DEF_WEAPON_FLAG_* / DEF_ITEM_ATTRIB_* (the tables initialize from them)

// Split out of def.cpp (quality campaign W3-3). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// The shared .def text scanner. Its lookup tables (flags, item attributes, death
// pieces) stay private here; only what a family parser calls is declared.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace opennova::def; // the flag/attrib tables and DefHudColor, unqualified as before

namespace opennova::defscan {

/* ========================================================================= */
/* Helpers                                                                   */
/* ========================================================================= */

char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

void safe_copy(char *dst, size_t dst_size, const char *src, size_t src_len) {
    if (src_len >= dst_size) src_len = dst_size - 1;
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
}

/* Trim leading+trailing whitespace, return pointer and length (no alloc) */
const char *trim_span(const char *s, size_t len, size_t *out_len) {
    while (len > 0 && isspace((unsigned char)*s)) { ++s; --len; }
    while (len > 0 && isspace((unsigned char)s[len - 1])) --len;
    *out_len = len;
    return s;
}

/* The witnessed line tokenizer ends a line at `//` or `;` only OUTSIDE double quotes:
   a quote flips the quoted state, and an unterminated quote keeps the rest of the
   line in one token. [orig: Terrain_TokenizeConfigLine @ 0x53CB60, the break on
   `//` / `;` gated on !inQuote, `"` toggling inQuote] */
const char *trim_def_line(const char *s, size_t len, size_t *out_len) {
    int quoted = 0;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] == 0) { len = i; break; }
        if (s[i] == '"') { quoted = !quoted; continue; }
        if (quoted) continue;
        if (s[i] == ';' || (i + 1 < len && s[i] == '/' && s[i + 1] == '/')) { len = i; break; }
    }
    return trim_span(s, len, out_len);
}

void to_lower_buf(char *dst, const char *src, size_t len) {
    for (size_t i = 0; i < len; ++i)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[len] = '\0';
}

/* Extract the first quoted string from a line. Without a closing quote the token runs
   to the end of the line, as the witnessed tokenizer's quoted state never closes
   [orig: Terrain_TokenizeConfigLine @ 0x53CB60]; retail's items.def carries thirteen
   such names ("Telephone pole,single pole w", ...). */
size_t extract_quoted(const char *line, size_t line_len, char *dst, size_t dst_size) {
    const char *q0 = (const char *)memchr(line, '"', line_len);
    if (!q0) { dst[0] = '\0'; return 0; }
    size_t rem = line_len - (size_t)(q0 - line) - 1;
    const char *q1 = (const char *)memchr(q0 + 1, '"', rem);
    if (!q1) q1 = line + line_len;
    if (q1 <= q0 + 1) { dst[0] = '\0'; return 0; }
    size_t slen = (size_t)(q1 - q0 - 1);
    safe_copy(dst, dst_size, q0 + 1, slen);
    return slen;
}

/* consume_value: skip key_len chars, trim, strip quotes, strip // comment */
const char *consume_value_span(const char *line, size_t line_len, size_t key_len, size_t *out_len) {
    if (key_len >= line_len) { *out_len = 0; return line; }
    size_t vlen;
    const char *v = trim_span(line + key_len, line_len - key_len, &vlen);
    /* Strip surrounding quotes */
    if (vlen >= 2 && v[0] == '"' && v[vlen - 1] == '"') {
        ++v; vlen -= 2;
    }
    /* Strip trailing comment */
    for (size_t i = 0; i + 1 < vlen; ++i) {
        if (v[i] == '/' && v[i + 1] == '/') {
            vlen = i;
            /* re-trim */
            while (vlen > 0 && isspace((unsigned char)v[vlen - 1])) --vlen;
            break;
        }
    }
    *out_len = vlen;
    return v;
}

void consume_value_str(const char *line, size_t line_len, size_t key_len, char *dst, size_t dst_size) {
    size_t vlen;
    const char *v = consume_value_span(line, line_len, key_len, &vlen);
    safe_copy(dst, dst_size, v, vlen);
}

/* atol as the game's CRT reads it [orig: _atol @0x76ab0a = strtol(s, NULL, 10) @0x76ab12 ->
   strtoxl @0x76b0ae]: white space skipped (@0x76b119..0x76b14e), one '-' or '+' (@0x76b158..
   0x76b165), then decimal digits into an unsigned 32-bit word up to the first other character,
   a digit past 0xFFFFFFFF marking overflow (@0x76b219 / 0x76b21b; with no end pointer the read
   stops there @0x76b223). An overflow, or a magnitude past 0x7FFFFFFF (0x80000000 when
   negative), saturates to 0x7FFFFFFF, or 0x80000000 when negative (@0x76b26f..0x76b295), which
   the sign then negates (@0x76b2a7): 2147483647 or -2147483648. No digits read 0 (@0x76b236).
   A host strtol saturates at its own long instead (64 bits on Linux), so the read is ported,
   the same on every platform, over the whole token. */
int parse_int_n(const char *s, size_t len) {
    size_t i = 0;
    while (i < len && isspace((unsigned char)s[i])) ++i;
    bool negative = false;
    if (i < len && (s[i] == '-' || s[i] == '+')) negative = s[i++] == '-';
    uint32_t value = 0;
    bool overflow = false;
    for (; i < len && s[i] >= '0' && s[i] <= '9'; ++i) {
        const uint32_t digit = (uint32_t)(s[i] - '0');
        if (value > 0xFFFFFFFFu / 10 || (value == 0xFFFFFFFFu / 10 && digit > 0xFFFFFFFFu % 10)) {
            overflow = true;
            break;
        }
        value = value * 10 + digit;
    }
    if (overflow || value > (negative ? 0x80000000u : 0x7FFFFFFFu)) return negative ? INT32_MIN : INT32_MAX;
    return negative ? (int)(-(int64_t)value) : (int)value;
}

/* ItemDef healthMax and the two armor classes are signed WORD stores in retail.
   Keep the normalized C ABI carrier as int, but wrap to 16 bits and sign-extend at
   parse time rather than relying on implementation-defined narrowing casts. */
int signed_i16_value(int value) {
    const unsigned int low = (unsigned int)value & 0xFFFFu;
    return low < 0x8000u ? (int)low : (int)low - 0x10000;
}

float parse_float_n(const char *s, size_t len) {
    char buf[64];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, s, len);
    buf[len] = '\0';
    return (float)strtod(buf, NULL);
}

/* Tokenize by whitespace, returns count. Stores start+len pairs. Max tokens. */

int tokenize(const char *s, size_t len, Token *tokens, int max_tok) {
    int n = 0;
    size_t i = 0;
    while (i < len && n < max_tok) {
        while (i < len && isspace((unsigned char)s[i])) ++i;
        if (i >= len) break;
        size_t start = i;
        while (i < len && !isspace((unsigned char)s[i])) ++i;
        tokens[n].s = s + start;
        tokens[n].len = i - start;
        ++n;
    }
    return n;
}

/* The engine debris-type table row names, in table order — index = the byte
   items.def 'husk_sub_part_types' stores per husk sub-part. Mirrors the 13 named
   rows of the 80-B static table; the full row data (velocities/effects/sounds)
   lives in engine/runtime/world/destruction.cpp, both citing the same original.
   [orig: g_DeathPieceTypes @ 0x8404f0; DeathPieceType_FindByName @ 0x57b310] */
static const char *const k_death_piece_type_names[13] = {
    "HULL",      "WHEEL",     "CHUNK_S",   "CHUNK_M",   "CHUNK_L",
    "ROCK_S",    "ROCK_M",    "ROCK_L",    "CHUNKNP_S", "CHUNKNP_M",
    "CHUNKNP_L", "CACTUS_",   "CHUNKSF_M",
};

const char *death_piece_keyword(size_t index) {
    return index < 13 ? k_death_piece_type_names[index] : nullptr;
}

int death_piece_type_lookup(const char *name, size_t len) {
    for (int i = 0; i < 13; ++i) {
        const char *t = k_death_piece_type_names[i];
        size_t j = 0;
        while (j < len && t[j] != '\0' &&
               tolower((unsigned char)t[j]) == tolower((unsigned char)name[j]))
            ++j;
        if (j == len && t[j] == '\0') return i;
    }
    return -1;
}

int death_piece_type_index(const char *name, size_t len) {
    const int row = death_piece_type_lookup(name, len);
    return row < 0 ? 0 : row; /* unknown -> HULL, the engine's zero-init read */
}

/* Split on commas and/or whitespace */
int split_values(const char *s, size_t len, Token *tokens, int max_tok) {
    int n = 0;
    size_t i = 0;
    while (i < len && n < max_tok) {
        while (i < len && (s[i] == ',' || isspace((unsigned char)s[i]))) ++i;
        if (i >= len) break;
        size_t start = i;
        while (i < len && s[i] != ',' && !isspace((unsigned char)s[i])) ++i;
        tokens[n].s = s + start;
        tokens[n].len = i - start;
        ++n;
    }
    return n;
}

/* starts_with for known-length prefix against lowercase buffer */
int lower_starts_with(const char *lower, size_t lower_len, const char *prefix, size_t prefix_len) {
    if (lower_len < prefix_len) return 0;
    return memcmp(lower, prefix, prefix_len) == 0;
}

/* Check prefix + next char is whitespace or end */
int lower_match_key(const char *lower, size_t lower_len, const char *prefix, size_t prefix_len) {
    if (!lower_starts_with(lower, lower_len, prefix, prefix_len)) return 0;
    if (lower_len == prefix_len) return 1;
    return isspace((unsigned char)lower[prefix_len]);
}



int next_line(LineIter *it, const char **out, size_t *out_len) {
    if (it->pos >= it->buf_len) return 0;
    ++it->line;
    const char *start = it->buf + it->pos;
    const char *nl = (const char *)memchr(start, '\n', it->buf_len - it->pos);
    size_t len;
    if (nl) {
        len = (size_t)(nl - start);
        it->pos += len + 1;
    } else {
        len = it->buf_len - it->pos;
        it->pos = it->buf_len;
    }
    /* Strip \r */
    if (len > 0 && start[len - 1] == '\r') --len;
    *out = start;
    *out_len = len;
    return 1;
}

/* Weapon flags table — the FULL witnessed token set, both flag dwords.
   [orig: the 16-B-stride {name, 0, flags1 bit, flags2 bit} table @ 0x830bf0;
   tokens compare case-insensitively]. The previous 7-entry table aliased
   whileswimming onto Underwater's 0x4 — corrected to the witnessed 0x1000000. */
static const FlagEntry flag_table[] = {
    {"scoped",          6, DEF_WEAPON_FLAG_SCOPED, 0},
    {"sighted",         7, DEF_WEAPON_FLAG_SIGHTED, 0},
    {"underwater",     10, DEF_WEAPON_FLAG_UNDERWATER, 0},
    {"showcomander",   12, DEF_WEAPON_FLAG_SHOWCOMANDER, 0},
    {"noclipsnodraw",  13, DEF_WEAPON_FLAG_NOCLIPSNODRAW, 0},
    {"burst",           5, DEF_WEAPON_FLAG_BURST, 0},
    {"notdropable",    11, DEF_WEAPON_FLAG_NOTDROPABLE, 0},
    {"emplaced",        8, DEF_WEAPON_FLAG_EMPLACED, 0},
    {"auto",            4, DEF_WEAPON_FLAG_AUTO, 0},
    {"norangecheck",   12, DEF_WEAPON_FLAG_NORANGECHECK, 0},
    {"showrange",       9, DEF_WEAPON_FLAG_SHOWRANGE, 0},
    {"showelevation",  13, DEF_WEAPON_FLAG_SHOWELEVATION, 0},
    {"armor",           5, DEF_WEAPON_FLAG_ARMOR, 0},
    {"okwhilejumping", 14, DEF_WEAPON_FLAG_OKWHILEJUMPING, 0},
    {"onlyfirescoped", 14, DEF_WEAPON_FLAG_ONLYFIRESCOPED, 0},
    {"lollypop",        8, DEF_WEAPON_FLAG_LOLLYPOP, 0},
    {"absorbpitch",    11, DEF_WEAPON_FLAG_ABSORBPITCH, 0},
    {"nomove",          6, DEF_WEAPON_FLAG_NOMOVE, 0},
    {"forcecrouch",    11, DEF_WEAPON_FLAG_FORCECROUCH, 0},
    {"onlyscoped",     10, DEF_WEAPON_FLAG_ONLYSCOPED, 0},
    {"2dimpact",        8, DEF_WEAPON_FLAG_2DIMPACT, 0},
    {"usedesignator",  13, DEF_WEAPON_FLAG_USEDESIGNATOR, 0},
    {"usespreadtwo",   12, DEF_WEAPON_FLAG_USESPREADTWO, 0},
    {"showimpactdist", 14, DEF_WEAPON_FLAG_SHOWIMPACTDIST, 0},
    {"whileswimming",  13, DEF_WEAPON_FLAG_WHILESWIMMING, 0},
    {"nocardswitch",   12, DEF_WEAPON_FLAG_NOCARDSWITCH, 0},
    {"handgunup",       9, DEF_WEAPON_FLAG_HANDGUNUP, 0},
    {"quickswitch",    11, DEF_WEAPON_FLAG_QUICKSWITCH, 0},
    {"onlyfirelocked", 14, DEF_WEAPON_FLAG_ONLYFIRELOCKED, 0},
    {"forcescoped",    11, DEF_WEAPON_FLAG_FORCESCOPED, 0},
    {"laserbeam",       9, DEF_WEAPON_FLAG_LASERBEAM, 0},
    {"powerthrow",     10, (int)DEF_WEAPON_FLAG_POWERTHROW, 0},
    {"noselect",        8, 0, DEF_WEAPON_FLAG2_NOSELECT},
    {"parachute",       9, 0, DEF_WEAPON_FLAG2_PARACHUTE},
    {"thermal",         7, 0, DEF_WEAPON_FLAG2_THERMAL},
    {"monitor",         7, 0, DEF_WEAPON_FLAG2_MONITOR},
    {"viewlock",        8, 0, DEF_WEAPON_FLAG2_VIEWLOCK},
    {"onlylockscoped", 14, 0, DEF_WEAPON_FLAG2_ONLYLOCKSCOPED},
    {"noammotypes",    11, 0, DEF_WEAPON_FLAG2_NOAMMOTYPES},
    {"showhudpip",     10, 0, DEF_WEAPON_FLAG2_SHOWHUDPIP},
    {"fixverticalofst",15, 0, DEF_WEAPON_FLAG2_FIXVERTICALOFST},
    {"inset",           5, 0, DEF_WEAPON_FLAG2_INSET},
    {"noautozero",     10, 0, DEF_WEAPON_FLAG2_NOAUTOZERO},
    {"invisible",       9, 0, DEF_WEAPON_FLAG2_INVISIBLE},
};
static const int flag_table_count = sizeof(flag_table) / sizeof(flag_table[0]);

const FlagEntry *weapon_flag_at(size_t index) {
    return index < static_cast<size_t>(flag_table_count) ? &flag_table[index] : nullptr;
}

void authoring_issue(size_t &count, opennova::def::DefParseReport *report,
                     size_t line, const char *record, const char *key, size_t key_len,
                     opennova::def::DefIssueCode code, const char *detail) {
    using opennova::def::DefIssueCode;
    if (opennova::def::def_issue_blocks(code)) ++count;
    if (!report) return;
    size_t end = 0;
    while (end < key_len && !isspace(static_cast<unsigned char>(key[end]))) ++end;
    std::string message;
    switch (code) {
    case DefIssueCode::UnknownProperty:
        message = detail ? "The game ignores the '" + std::string(detail) + "' token here; saving drops it."
                         : "The game ignores this line; saving drops it.";
        break;
    case DefIssueCode::Reinterpreted:
        message = "The game reads this as " + std::string(detail ? detail : "another value") + "; saving writes " +
                  std::string(detail ? detail : "that") + ".";
        break;
    case DefIssueCode::MalformedBlock: message = "Incomplete or misplaced block."; break;
    case DefIssueCode::InvalidValue: message = "This value is not one the game reads; correct it before saving."; break;
    case DefIssueCode::Unrepresentable: message = "The property cannot be represented without losing information."; break;
    }
    report->push_back({code, line, record ? record : "", std::string(key, end), message});
}

const FlagEntry *lookup_flag(const char *name, size_t len) {
    for (int i = 0; i < flag_table_count; ++i) {
        if (len == flag_table[i].name_len && memcmp(name, flag_table[i].name, len) == 0)
            return &flag_table[i];
    }
    return NULL;
}

/* items.def `attrib:` tokens -> ItemDefAttrib (+0x54) bits. Token names lowercased (the
   parser lowercases before compare). Full witnessed map: docs/world/itemdef-re.md:147-155.
   `Door` also defaults the door count and `Parent` is a byte, not a bit: both side
   effects live in def_items.cpp's attrib arm. [orig: ItemDef_ParseProperty @0x49eb00] */
static const FlagEntry item_attrib_table[] = {
    {"movecb",       6, DEF_ITEM_ATTRIB_MOVECB},
    {"powerup",      7, DEF_ITEM_ATTRIB_POWERUP},
    {"nomoveshoot",  11, DEF_ITEM_ATTRIB_NOMOVESHOOT},
    {"notool",       6, DEF_ITEM_ATTRIB_NOTOOL},
    {"snap",         4, DEF_ITEM_ATTRIB_SNAP},
    {"eweap",        5, DEF_ITEM_ATTRIB_EWEAP},
    {"playercontrol",13, DEF_ITEM_ATTRIB_PLAYERCONTROL},
    {"door",         4, DEF_ITEM_ATTRIB_DOOR},
    {"notarget",     8, DEF_ITEM_ATTRIB_NOTARGET},
    {"landable",     8, DEF_ITEM_ATTRIB_LANDABLE},
    {"missile",      7, DEF_ITEM_ATTRIB_MISSILE},
    {"tire",         4, DEF_ITEM_ATTRIB_TIRE},
    {"fastrope",     8, DEF_ITEM_ATTRIB_FASTROPE},
    {"takeable",     8, DEF_ITEM_ATTRIB_TAKEABLE},
    {"easy",         4, DEF_ITEM_ATTRIB_EASY},
    {"s&d",          3, DEF_ITEM_ATTRIB_SD},      /* the S&D/A&D objective target [orig: @0x4a084e..0x4a086d] */
    {"4team",        5, DEF_ITEM_ATTRIB_4TEAM},
    {"changeteam",   10, DEF_ITEM_ATTRIB_CHANGETEAM},
    {"spawnpoint",   10, DEF_ITEM_ATTRIB_SPAWNPOINT},
    {"armory",       6, DEF_ITEM_ATTRIB_ARMORY},
    {"aidata",       6, DEF_ITEM_ATTRIB_AIDATA},   /* the §5.6 AI-class flag — gates the 0x0D AI-trailer */
    {"leavecorpse",  11, DEF_ITEM_ATTRIB_LEAVECORPSE},
    {"nodismember",  11, DEF_ITEM_ATTRIB_NODISMEMBER},
    {"noweapon",     8, DEF_ITEM_ATTRIB_NOWEAPON},
    {"reflect",      7, DEF_ITEM_ATTRIB_REFLECT},
    {"noshadow",     8, DEF_ITEM_ATTRIB_NOSHADOW},
    {"concave",      7, DEF_ITEM_ATTRIB_CONCAVE},
    {"noscar",       6, DEF_ITEM_ATTRIB_NOSCAR},
    {"nohud",        5, DEF_ITEM_ATTRIB_NOHUD},
    {"nodie",        5, DEF_ITEM_ATTRIB_NODIE},
};
static const int item_attrib_table_count =
    sizeof(item_attrib_table) / sizeof(item_attrib_table[0]);

/* items.def `attrib:` tokens -> ItemDefAttrib2 (+0x58) bits. docs/world/itemdef-re.md:157-160. */
static const FlagEntry item_attrib2_table[] = {
    {"vehiclebay",      10, DEF_ITEM_ATTRIB2_VEHICLEBAY},
    {"autoinheritteam", 15, DEF_ITEM_ATTRIB2_AUTOINHERITTEAM},
    {"vehiclespawn",    12, DEF_ITEM_ATTRIB2_VEHICLESPAWN},
    {"dynamicshadow",   13, DEF_ITEM_ATTRIB2_DYNAMICSHADOW},
    {"staticshadow",    12, DEF_ITEM_ATTRIB2_STATICSHADOW},
    {"tunnelpiece",     11, DEF_ITEM_ATTRIB2_TUNNELPIECE},
    {"usevk",           5, DEF_ITEM_ATTRIB2_USEVK},
    {"staticdeath",     11, DEF_ITEM_ATTRIB2_STATICDEATH},
    {"onturret",        8, DEF_ITEM_ATTRIB2_ONTURRET},
    {"hasturret",       9, DEF_ITEM_ATTRIB2_HASTURRET},
    {"isturret",        8, DEF_ITEM_ATTRIB2_ISTURRET},
    {"farp",            4, DEF_ITEM_ATTRIB2_FARP},
    {"landmine",        8, DEF_ITEM_ATTRIB2_LANDMINE},
};
static const int item_attrib2_table_count =
    sizeof(item_attrib2_table) / sizeof(item_attrib2_table[0]);

int lookup_item_attrib(const char *name, size_t len) {
    for (int i = 0; i < item_attrib_table_count; ++i) {
        if (len == item_attrib_table[i].name_len &&
            memcmp(name, item_attrib_table[i].name, len) == 0)
            return item_attrib_table[i].bit;
    }
    return 0;
}

int lookup_item_attrib2(const char *name, size_t len) {
    for (int i = 0; i < item_attrib2_table_count; ++i) {
        if (len == item_attrib2_table[i].name_len &&
            memcmp(name, item_attrib2_table[i].name, len) == 0)
            return item_attrib2_table[i].bit;
    }
    return 0;
}


// A quoted header may lack its closing quote: the witnessed tokenizer then keeps the
// rest of the line as the name [orig: Terrain_TokenizeConfigLine @ 0x53CB60], and the
// writer emits the closed form, which reads back identically.
void validate_header(const char *line, size_t length, size_t key_length, size_t capacity, DefHeaderName form,
                     size_t &issues, DefParseReport *report, size_t number, const char *record) {
    bool valid = key_length < length;
    if (form == DefHeaderName::Token) {
        // The keyword and the name are the line's first two tokens as the
        // retail tokenizer cuts them, so a comma or a quote ends the keyword as
        // a space does (`ACTION,SCOPEUP` and `ACTION"SCOPEUP"` read as `ACTION
        // SCOPEUP`), and no token follows the name [orig:
        // WeaponDefs_ParseLineCallback @0x543680 over Terrain_TokenizeConfigLine
        // @0x53CB60's tokens, the delimiters @0x53CC33..0x53CC70].
        const std::string copy(line, length);
        io::ConfigTokens tokens;
        io::tokenize_config_line(copy.c_str(), tokens);
        valid &= strlen(tokens.token(0)) == key_length && tokens.count <= 2 && strlen(tokens.token(1)) < capacity;
    } else {
        valid = valid && isspace(static_cast<unsigned char>(line[key_length]));
        size_t size;
        const char *value = trim_span(line + key_length, length - key_length, &size);
        if (form == DefHeaderName::Quoted) {
            valid &= size >= 1 && value[0] == '"';
            if (valid) {
                ++value; --size;
                if (size && value[size - 1] == '"') --size;
            }
        }
        valid &= size < capacity && memchr(value, '"', size) == nullptr;
    }
    if (!valid) authoring_issue(issues, report, number, record, line, key_length, DefIssueCode::MalformedBlock);
}

// Authoring checks supplement the permissive runtime parser. The data still comes
// exclusively from that parser; this function records input that cannot be saved.
void validate_property(opennova::def::DefRecordKind kind, const char *line, size_t length,
                       size_t &issues, opennova::def::DefParseReport *report,
                       size_t number, const char *record) {
    using namespace opennova::def;
    size_t key_length = 0;
    while (kind != DefRecordKind::Effect && key_length < length && !isspace(static_cast<unsigned char>(line[key_length]))) ++key_length;
    std::string key(line, key_length);
    for (char &c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    auto invalid = [&](DefIssueCode code = DefIssueCode::InvalidValue) {
        authoring_issue(issues, report, number, record, line, key_length, code);
    };
    DefRecordKind property_kind = kind;
    if (kind == DefRecordKind::Weapon && key == "sights") property_kind = DefRecordKind::Sight;
    if (kind == DefRecordKind::Item && (key == "addeweap" || key == "addeweapg" || key == "addeweapc"))
        property_kind = DefRecordKind::Attachment;
    const DefProperty *property = nullptr;
    for (const auto &p : def_properties(property_kind))
        if (p.key == key || (property_kind == DefRecordKind::Attachment && p.key == "addeweap") ||
            (kind == DefRecordKind::Action && key == "delay" && p.key == "delayend") ||
            // `animcal` fills the one anim-map buffer `animadm` does [orig: @ 0x543D77 / 0x543D47]
            (kind == DefRecordKind::Weapon && key == "animcal" && p.key == "animadm")) { property = &p; break; }
    const bool alias = kind == DefRecordKind::Item &&
        (key == "sqb_rate" || key == "sqb_distance" || key == "sqb_error" || key == "num_doors" ||
         key == "first_door" || key == "first_subobject" || key == "door_dir" || key == "rotor_parts" ||
         key == "aux_parts" ||
         key == "particletesttime");
    if (!property && !alias) { invalid(DefIssueCode::UnknownProperty); return; }
    size_t value_length;
    const char *value = consume_value_span(line, length, key_length, &value_length);
    Token tokens[128];
    // weapon.def's lines arrive as the retail tokenizer read them (a token holding a
    // delimiter quoted again), so its values split the same way: a quoted run is one
    // token [orig: Terrain_TokenizeConfigLine @0x53CB60, quote @0x53CC4E..0x53CC70].
    const bool tokenized = kind == DefRecordKind::Weapon || kind == DefRecordKind::Action ||
                           kind == DefRecordKind::Carry;
    io::ConfigTokens config;
    int count = 0;
    if (tokenized) {
        const std::string rest(line + key_length, length - key_length);
        io::tokenize_config_line(rest.c_str(), config);
        for (; count < config.count; ++count) tokens[count] = {config.tokens[count], strlen(config.tokens[count])};
    } else {
        count = split_values(value, value_length, tokens, 128);
    }
    auto numeric = [&](int i) {
        if (i >= count) return false;
        const std::string text(tokens[i].s, tokens[i].len);
        char *end = nullptr;
        const double result = strtod(text.c_str(), &end);
        return end != text.c_str() && *end == 0 && std::isfinite(result);
    };
    auto word = [&](int i) {
        std::string text(tokens[i].s, tokens[i].len);
        for (char &c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return text;
    };
    if (!property) {
        if (!count) { invalid(); return; }
        for (int i = 0; i < count; ++i) if (!numeric(i)) { invalid(); break; }
        return;
    }
    const auto encoding = property->encoding;
    if (encoding == DefEncoding::ItemAttrib) {
        // A token outside the witnessed chain is skipped by the game, not an error:
        // retail's own items.def carries `exp1`, `Good`, `Evil`, `forceasset`,
        // `neutral`, `PilotOnly`, `Train`, `NoCTool`, `LFP`, `fo`, `pfoil`.
        // [orig: ItemDef_ParseProperty @ 0x49EB00, the attrib: chain has no else arm]
        // A bare `attrib:` (jox01's items.def has 199) is a line the game reads and
        // returns from at once, setting nothing [orig: ItemDef_ParseProperty @ 0x4A06A8
        // `cmp [count],1; jle` -> the plain return @ 0x4A1CA6]: ignored, not invalid.
        if (!count) invalid(DefIssueCode::UnknownProperty);
        for (int i = 0; i < count; ++i) {
            const auto text = word(i);
            if (!lookup_item_attrib(text.data(), text.size()) && !lookup_item_attrib2(text.data(), text.size()) && text != "parent")
                authoring_issue(issues, report, number, record, line, key_length, DefIssueCode::UnknownProperty, text.c_str());
        }
        return;
    }
    if (encoding == DefEncoding::ModelOption) {
        // The model by its first token; the reader compares the second with `nocheckdepth`
        // and reads nothing else, so any other token is ignored input [orig:
        // WeaponDefs_ParseLineCallback @ 0x544F85..0x544FBB, tokens[2] @ 0x544F92].
        const auto *field = def_field(property_kind, property->fields.front());
        if (count >= 1 && field && tokens[0].len >= field->width) invalid();
        for (int i = 1; i < count; ++i)
            if (i > 1 || word(i) != "nocheckdepth")
                authoring_issue(issues, report, number, record, line, key_length, DefIssueCode::UnknownProperty,
                                std::string(tokens[i].s, tokens[i].len).c_str());
        return;
    }
    if (encoding == DefEncoding::AmmoFlags || encoding == DefEncoding::WeaponFlags || encoding == DefEncoding::AmmoKillZone) {
        bool valid = count == 1;
        if (valid) {
            const auto text = word(0); valid = false;
            if (encoding == DefEncoding::WeaponFlags) valid = lookup_flag(text.data(), text.size()) != nullptr;
            if (encoding == DefEncoding::AmmoFlags)
                for (size_t i = 0; const char *name = def_ammo_flag_keyword(i); ++i) if (text == name) valid = true;
            if (encoding == DefEncoding::AmmoKillZone)
                for (size_t i = 1; const char *name = def_ammo_kz_keyword(i); ++i) if (text == name) valid = true;
        }
        if (!valid) invalid();
        return;
    }
    if (encoding == DefEncoding::ItemType) {
        const auto *field = def_field(kind, "type");
        bool valid = false;
        if (count == 1) {
            const auto text = word(0);
            for (const auto &choice : field->choices) if (text == choice.name) valid = true;
            valid |= text == "foliage" || text == "object";
        }
        if (!valid) invalid();
        return;
    }
    if (encoding == DefEncoding::DeathPieces) {
        // The game's loop skips a token without `_` or with a slot outside 1..16 and
        // stops after sixteen stored pieces, so those are dropped, not refused; a
        // name outside the piece table reads as row 0 (HULL) at load time, so it is
        // reported and written as HULL. [orig: ItemDef_ParseProperty @ 0x49EB00
        // husk_sub_part_types arm; DeathPieceType_FindByName @ 0x57B310]
        int stored = 0;
        for (int i = 0; i < count; ++i) {
            const auto text = word(i);
            const size_t split = text.find('_');
            char *end = nullptr;
            const long slot = split == std::string::npos ? 0 : strtol(text.c_str(), &end, 10);
            if (split == std::string::npos || end != text.c_str() + split || slot < 1 || slot > 16 || stored >= 16) {
                authoring_issue(issues, report, number, record, line, key_length, DefIssueCode::UnknownProperty, text.c_str());
                continue;
            }
            ++stored;
            const std::string name = text.substr(split + 1);
            if (death_piece_type_lookup(name.data(), name.size()) < 0)
                authoring_issue(issues, report, number, record, line, key_length, DefIssueCode::UnknownProperty, text.c_str());
        }
        return;
    }
    if (kind == DefRecordKind::Ammo && key == "tracer_type") {
        if (count < 1 || count > 2) invalid();
        for (int i = 0; i < count; ++i) {
            bool valid = numeric(i);
            for (size_t j = 0; const char *name = def_ammo_tracer_keyword(j); ++j)
                if (word(i) == name) valid = true;
            if (!valid) invalid();
        }
        return;
    }
    if (encoding == DefEncoding::SpawnMask || encoding == DefEncoding::DoorType) {
        for (int i = 0; i < count; ++i) if (!numeric(i)) { invalid(); break; }
        return;
    }
    if (encoding == DefEncoding::ClassRounds) {
        if (count != 2 || !numeric(1)) invalid();
        else if (word(0) != "medic" && word(0) != "sniper" && word(0) != "gunner" &&
                 word(0) != "rifleman" && word(0) != "engineer") invalid();
        return;
    }
    // A particle slot takes its effect alone: the arm copies the line's second and third
    // tokens whatever the count, and the tokenizer resets the first three to "" for every
    // line, so a missing userpoint reads as none (jox01's `particlefx fx_Mosquitos_2m_L`)
    // [orig: ItemDef_ParseProperty @ 0x4A13BF..0x4A13FC; Terrain_TokenizeConfigLine
    // @ 0x53CB71..0x53CB81].
    const int minimum = kind == DefRecordKind::Effect ? 4 : kind == DefRecordKind::Carry ? 2 : encoding == DefEncoding::Pose ? 6 : encoding == DefEncoding::Sight ? 5 :
        encoding == DefEncoding::Attachment ? 2 : 1;
    if (count < minimum) {
        // An empty string remains a serializable draft; semantic validation can
        // require a symbol. Missing numbers are malformed input.
        const auto *first = def_field(property_kind, property->fields.front());
        if (!first || first->type != DefFieldType::Text || minimum != 1) invalid();
        return;
    }
    if (kind == DefRecordKind::Effect && count != 4) invalid();
    if (kind == DefRecordKind::Carry && count != 2) invalid();
    if (encoding == DefEncoding::Function && count > 5) invalid();
    if (encoding == DefEncoding::Sight) {
        for (int i = 5; i < count; ++i) {
            const auto option = word(i);
            if (option == "slide") {
                if (!numeric(++i)) invalid();
            } else if (option != "scale" && option != "blend" && option != "add" &&
                       option != "blendat" && option != "multiply" && option != "addat" && option != "multiplyat") invalid();
        }
    }
    if (encoding == DefEncoding::Pose && count != 6) invalid();
    // The addeweap arm reads the userpoint and the id, and the four angles only when the
    // line has more than three tokens, key included; nothing past the sixth argument is
    // read, so a trailing token (jox01's `<down angle> <up angle> ...` placeholders) is
    // ignored. One to three angles read the slots an earlier line left (the tokenizer
    // resets only the first three), a value no file states: invalid. [orig:
    // ItemDef_ParseProperty @ 0x4A1B42..0x4A1C9F, count > 3 @ 0x4A1BB4; Terrain_TokenizeConfigLine
    // @ 0x53CB71..0x53CB81]
    if (encoding == DefEncoding::Attachment) {
        if (count > 2 && count < 6) invalid();
        for (int i = 6; i < count; ++i)
            authoring_issue(issues, report, number, record, line, key_length, DefIssueCode::UnknownProperty,
                            std::string(tokens[i].s, tokens[i].len).c_str());
    }
    size_t columns = property->fields.size();
    if (encoding == DefEncoding::FloatFixed) columns /= 2;
    if (encoding == DefEncoding::Attachment) columns = 6;
    if (encoding == DefEncoding::Sight) columns = 5;
    for (size_t i = 0; i < std::min(columns, size_t(count)); ++i) {
        const auto *field = def_field(property_kind, property->fields[i]);
        if (!field) continue;
        if (field->type == DefFieldType::Text) {
            const size_t size = columns == 1 ? value_length : tokens[i].len;
            if (size >= field->width) invalid();
        } else if (!numeric(int(i)) && !(encoding == DefEncoding::Delay && word(int(i)) == "auto")) {
            // An attachment's angle the reader atol's: a word reads as 0 [orig:
            // ItemDef_ParseProperty @ 0x4A1BB4..0x4A1C49], reported as such, written as 0.
            if (encoding == DefEncoding::Attachment && i >= 2)
                authoring_issue(issues, report, number, record, line, key_length, DefIssueCode::Reinterpreted, "0");
            else invalid();
        }
    }
}

}  // namespace opennova::defscan

namespace opennova::def {

/* The public index projection over the two tables above (def.h): a tool naming an
   ItemDefAttrib bit reads the parser's own token list. [orig: ItemDef_ParseProperty
   @0x49eb00] */
int def_item_attrib_keyword_count(void) {
    return opennova::defscan::item_attrib_table_count;
}

const char *def_item_attrib_keyword(int index) {
    if (index < 0 || index >= opennova::defscan::item_attrib_table_count) return NULL;
    return opennova::defscan::item_attrib_table[index].name;
}

uint32_t def_item_attrib_keyword_bit(int index) {
    if (index < 0 || index >= opennova::defscan::item_attrib_table_count) return 0u;
    return (uint32_t)opennova::defscan::item_attrib_table[index].bit;
}

int def_item_attrib2_keyword_count(void) {
    return opennova::defscan::item_attrib2_table_count;
}

const char *def_item_attrib2_keyword(int index) {
    if (index < 0 || index >= opennova::defscan::item_attrib2_table_count) return NULL;
    return opennova::defscan::item_attrib2_table[index].name;
}

uint32_t def_item_attrib2_keyword_bit(int index) {
    if (index < 0 || index >= opennova::defscan::item_attrib2_table_count) return 0u;
    return (uint32_t)opennova::defscan::item_attrib2_table[index].bit;
}

/* The DefItemType names (def.h): the non-injective pairs keep both tokens. */
const char *def_item_type_name(int type) {
    switch (type) {
        case DEF_ITEM_TYPE_UNSET: return "unset";
        case DEF_ITEM_TYPE_VEHICLE: return "vehicle";
        case DEF_ITEM_TYPE_DECORATION: return "decoration/foliage";
        case DEF_ITEM_TYPE_PERSON: return "person";
        case DEF_ITEM_TYPE_MARKER: return "marker";
        case DEF_ITEM_TYPE_BUILDING: return "building";
        case DEF_ITEM_TYPE_POWERUP: return "powerup/object";
        case DEF_ITEM_TYPE_EFFECT: return "effect";
        default: return "?";
    }
}

} // namespace opennova::def

namespace opennova::defscan {

/* Alignment parser: left=0, right=1, center=2 */
static int parse_alignment(const char *s, size_t len) {
    if (len == 5 && memcmp(s, "right", 5) == 0) return 1;
    if (len == 6 && memcmp(s, "center", 6) == 0) return 2;
    return 0;
}

/* Parse N ints from value string */
int parse_ints(const char *s, size_t len, int *out, int max_n) {
    Token tok[MAX_TOKENS];
    int n = split_values(s, len, tok, max_n < MAX_TOKENS ? max_n : MAX_TOKENS);
    for (int i = 0; i < n && i < max_n; ++i)
        out[i] = parse_int_n(tok[i].s, tok[i].len);
    return n;
}

/* Parse HudColor from RGB values */
DefHudColor parse_hud_color(Token *vals, int n) {
    DefHudColor c = {0, 0, 0, 255};
    if (n >= 3) {
        c.r = parse_int_n(vals[0].s, vals[0].len);
        c.g = parse_int_n(vals[1].s, vals[1].len);
        c.b = parse_int_n(vals[2].s, vals[2].len);
        if (n >= 4) c.a = parse_int_n(vals[3].s, vals[3].len);
    }
    return c;
}

/* Parse HudColor from ARGB values */
DefHudColor parse_hud_color_argb(Token *vals, int n) {
    DefHudColor c = {0, 0, 0, 255};
    if (n >= 4) {
        c.a = parse_int_n(vals[0].s, vals[0].len);
        c.r = parse_int_n(vals[1].s, vals[1].len);
        c.g = parse_int_n(vals[2].s, vals[2].len);
        c.b = parse_int_n(vals[3].s, vals[3].len);
    }
    return c;
}

/* Parse a positioned-text token into (x, y, hidden, align) — the original's
   4-dword global layout, read strictly positionally: field 1 x, field 2 y,
   field 3 the hidden gate via numeric parse (0 = draw; a word reads 0), field 4
   the alignment word ("right"=1/"center"=2/anything else 0=left — full-string
   match; a missing field is left). Retail 2-field lines (GAMEINFO, HUDCHATTEXT)
   render visible/left in retail JO, pinning missing fields to 0.
   [orig: AMMOCOUNTPOS parse @0x59fc3d — atof->ftol x/y/hidden then
   HUD_ParseTextAlignment @0x59d6b0 on field 4; the draws gate on the hidden
   dword @0x5939f3] */
void parse_pos_aligned(Token *vals, int n, int *out) {
    if (n >= 1) out[0] = parse_int_n(vals[0].s, vals[0].len);
    if (n >= 2) out[1] = parse_int_n(vals[1].s, vals[1].len);
    if (n >= 3) out[2] = parse_int_n(vals[2].s, vals[2].len);
    if (n >= 4) {
        char low[16];
        size_t ll = vals[3].len < 15 ? vals[3].len : 15;
        to_lower_buf(low, vals[3].s, ll);
        out[3] = parse_alignment(low, ll);
    }
}

/* [orig: HUD_ParseHudposToken's BREATHTIME arm @0x59FB3B..0x59FB84 -- atof x,
   atof y, then HUD_ParseTextAlignment on the THIRD token] */
void parse_pos_align3(Token *vals, int n, int *out) {
    if (n >= 1) out[0] = parse_int_n(vals[0].s, vals[0].len);
    if (n >= 2) out[1] = parse_int_n(vals[1].s, vals[1].len);
    if (n >= 3) {
        char low[16];
        size_t ll = vals[2].len < 15 ? vals[2].len : 15;
        to_lower_buf(low, vals[2].s, ll);
        out[2] = parse_alignment(low, ll);
    }
}


int parse_fixed16_digits_n(const char *s, size_t len) {
    size_t i = 0;
    int integer_part = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        integer_part = s[i] + 10 * integer_part - '0';
        ++i;
    }
    int frac_accum = 127;
    if (i < len && s[i] == '.') {
        long long frac_scale = 0x1000000;
        ++i;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            frac_scale = (419430LL * frac_scale) >> 22;
            frac_accum += (int)(frac_scale * (s[i] - '0'));
            ++i;
        }
    }
    return (integer_part << 16) + (frac_accum >> 8);
}

}  // namespace opennova::defscan