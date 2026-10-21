#include "def_scan.h"

#include <base/io/os_path.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include <base/io/crt_ftol.h>

#include <formats/def/def.h> // DEF_WEAPON_FLAG_* / DEF_ITEM_ATTRIB_* (the tables initialize from them)

#include <base/io/strutil.h>

// Split out of def.cpp (quality campaign W3-3).
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
    FILE *f = opennova::io::fopen_utf8(path, "rb");
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

void to_lower_buf(char *dst, const char *src, size_t len) {
    for (size_t i = 0; i < len; ++i)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[len] = '\0';
}

/* The CRT's atol on a token span (io::retail_atol_n: 32 bits, saturating). */
int parse_int_n(const char *s, size_t len) {
    return io::retail_atol_n(s, len);
}

/* ItemDef healthMax and the two armor classes are signed WORD stores in retail.
   Keep the normalized C ABI carrier as int, but wrap to 16 bits and sign-extend at
   parse time rather than relying on implementation-defined narrowing casts. */
int signed_i16_value(int value) {
    const unsigned int low = (unsigned int)value & 0xFFFFu;
    return low < 0x8000u ? (int)low : (int)low - 0x10000;
}

/* The CRT's atof on a token span (io::retail_atof: decimal only, an exponent
   marked e, E, d or D), narrowed to float. */
float parse_float_n(const char *s, size_t len) {
    return (float)io::retail_atof_n(s, len);
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

bool key_is(const char *key, const char *name) {
    return strutil::iequals(key, name);
}

int value_tokens(const io::ConfigTokens &tokens, Token *out, int max_tok) {
    int n = 0;
    for (int i = 1; i < tokens.count && n < max_tok; ++i) {
        out[n].s = tokens.tokens[i];
        out[n].len = strlen(tokens.tokens[i]);
        ++n;
    }
    return n;
}

void copy_token(char *dst, size_t dst_size, const io::ConfigTokens &tokens, int index) {
    const char *token = tokens.token(index);
    safe_copy(dst, dst_size, token, strlen(token));
}

/* Weapon flags table — the FULL witnessed token set, both flag dwords, each name spelled as the
   game's table spells it and in its rows' order (the writer puts both down: Armor is row 32, after
   NoSelect; OnlyFireScoped row 17, after ForceCrouch).
   [orig: the 16-B-stride {name, 0, flags1 bit, flags2 bit} table @ 0x830bf0;
   tokens compare case-insensitively]. The previous 7-entry table aliased
   whileswimming onto Underwater's 0x4 — corrected to the witnessed 0x1000000. */
static const FlagEntry flag_table[] = {
    {"Scoped",          6, DEF_WEAPON_FLAG_SCOPED, 0},
    {"Sighted",         7, DEF_WEAPON_FLAG_SIGHTED, 0},
    {"Underwater",     10, DEF_WEAPON_FLAG_UNDERWATER, 0},
    {"ShowComander",   12, DEF_WEAPON_FLAG_SHOWCOMANDER, 0},
    {"NoClipsNoDraw",  13, DEF_WEAPON_FLAG_NOCLIPSNODRAW, 0},
    {"Burst",           5, DEF_WEAPON_FLAG_BURST, 0},
    {"NotDropable",    11, DEF_WEAPON_FLAG_NOTDROPABLE, 0},
    {"Emplaced",        8, DEF_WEAPON_FLAG_EMPLACED, 0},
    {"Auto",            4, DEF_WEAPON_FLAG_AUTO, 0},
    {"norangecheck",   12, DEF_WEAPON_FLAG_NORANGECHECK, 0},
    {"ShowRange",       9, DEF_WEAPON_FLAG_SHOWRANGE, 0},
    {"ShowElevation",  13, DEF_WEAPON_FLAG_SHOWELEVATION, 0},
    {"OkWhileJumping", 14, DEF_WEAPON_FLAG_OKWHILEJUMPING, 0},
    {"LollyPop",        8, DEF_WEAPON_FLAG_LOLLYPOP, 0},
    {"AbsorbPitch",    11, DEF_WEAPON_FLAG_ABSORBPITCH, 0},
    {"NoMove",          6, DEF_WEAPON_FLAG_NOMOVE, 0},
    {"ForceCrouch",    11, DEF_WEAPON_FLAG_FORCECROUCH, 0},
    {"OnlyFireScoped", 14, DEF_WEAPON_FLAG_ONLYFIRESCOPED, 0},
    {"OnlyScoped",     10, DEF_WEAPON_FLAG_ONLYSCOPED, 0},
    {"2DImpact",        8, DEF_WEAPON_FLAG_2DIMPACT, 0},
    {"UseDesignator",  13, DEF_WEAPON_FLAG_USEDESIGNATOR, 0},
    {"UseSpreadTwo",   12, DEF_WEAPON_FLAG_USESPREADTWO, 0},
    {"ShowImpactDist", 14, DEF_WEAPON_FLAG_SHOWIMPACTDIST, 0},
    {"WhileSwimming",  13, DEF_WEAPON_FLAG_WHILESWIMMING, 0},
    {"NoCardSwitch",   12, DEF_WEAPON_FLAG_NOCARDSWITCH, 0},
    {"HandGunUp",       9, DEF_WEAPON_FLAG_HANDGUNUP, 0},
    {"QuickSwitch",    11, DEF_WEAPON_FLAG_QUICKSWITCH, 0},
    {"OnlyFireLocked", 14, DEF_WEAPON_FLAG_ONLYFIRELOCKED, 0},
    {"ForceScoped",    11, DEF_WEAPON_FLAG_FORCESCOPED, 0},
    {"LaserBeam",       9, DEF_WEAPON_FLAG_LASERBEAM, 0},
    {"PowerThrow",     10, (int)DEF_WEAPON_FLAG_POWERTHROW, 0},
    {"NoSelect",        8, 0, DEF_WEAPON_FLAG2_NOSELECT},
    {"Armor",           5, DEF_WEAPON_FLAG_ARMOR, 0},
    {"Parachute",       9, 0, DEF_WEAPON_FLAG2_PARACHUTE},
    {"Thermal",         7, 0, DEF_WEAPON_FLAG2_THERMAL},
    {"Monitor",         7, 0, DEF_WEAPON_FLAG2_MONITOR},
    {"ViewLock",        8, 0, DEF_WEAPON_FLAG2_VIEWLOCK},
    {"onlylockscoped", 14, 0, DEF_WEAPON_FLAG2_ONLYLOCKSCOPED},
    {"NoAmmoTypes",    11, 0, DEF_WEAPON_FLAG2_NOAMMOTYPES},
    {"showhudpip",     10, 0, DEF_WEAPON_FLAG2_SHOWHUDPIP},
    {"FixVerticalOfst",15, 0, DEF_WEAPON_FLAG2_FIXVERTICALOFST},
    {"Inset",           5, 0, DEF_WEAPON_FLAG2_INSET},
    {"NoAutoZero",     10, 0, DEF_WEAPON_FLAG2_NOAUTOZERO},
    {"Invisible",       9, 0, DEF_WEAPON_FLAG2_INVISIBLE},
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
    // What the game makes of the input (the writer puts down what the game reads, no more).
    case DefIssueCode::UnknownProperty:
        message = detail ? "The game ignores the '" + std::string(detail) + "' token here."
                         : "The game ignores this line.";
        break;
    case DefIssueCode::Reinterpreted:
        message = "The game reads this as " + std::string(detail ? detail : "another value") + ".";
        break;
    case DefIssueCode::Unclosed: message = "No end closes the block; the game reads it as one an end closes."; break;
    case DefIssueCode::MalformedBlock: message = "Incomplete or misplaced block."; break;
    case DefIssueCode::InvalidValue: message = "This value is not one the game reads; correct it before saving."; break;
    case DefIssueCode::Unrepresentable: message = "The property cannot be represented without losing information."; break;
    }
    report->push_back({code, line, record ? record : "", std::string(key, end), message});
}

std::string line_as_read(const io::ConfigTokens &tokens) {
    std::string text;
    for (int i = 0; i < tokens.count; ++i) {
        const char *s = tokens.tokens[i];
        const size_t at = static_cast<size_t>(s - tokens.buffer);
        size_t n = strlen(s);
        if (at + n > tokens.cut) n = tokens.cut - at;
        const std::string token(s, n);
        if (i) text += ' ';
        if (token.find_first_of(" ,\t;") != std::string::npos || token.find("//") != std::string::npos)
            text += '"' + token + '"';
        else text += token;
    }
    return text;
}

const FlagEntry *lookup_flag(const char *name, size_t len) {
    for (int i = 0; i < flag_table_count; ++i) {
        if (len == flag_table[i].name_len && strutil::iequals(std::string_view(name, len), flag_table[i].name))
            return &flag_table[i];
    }
    return NULL;
}

/* items.def `attrib:` tokens -> ItemDefAttrib (+0x54) bits, each spelled as the game's _stricmp chain
   spells it (the writer puts that spelling down; the compare ignores case) [orig: the strings pushed @
   0x4A06F3 Aidata .. @ 0x4A0C8D Door]. Full witnessed map: docs/world/itemdef-re.md:147-155.
   `Door` also defaults the door count and `Parent` is a byte, not a bit: both side
   effects live in def_items.cpp's attrib arm. [orig: ItemDef_ParseProperty @0x49eb00] */
static const FlagEntry item_attrib_table[] = {
    {"Movecb",       6, DEF_ITEM_ATTRIB_MOVECB},
    {"Powerup",      7, DEF_ITEM_ATTRIB_POWERUP},
    {"NoMoveShoot",  11, DEF_ITEM_ATTRIB_NOMOVESHOOT},
    {"NoTool",       6, DEF_ITEM_ATTRIB_NOTOOL},
    {"Snap",         4, DEF_ITEM_ATTRIB_SNAP},
    {"EWeap",        5, DEF_ITEM_ATTRIB_EWEAP},
    {"PlayerControl",13, DEF_ITEM_ATTRIB_PLAYERCONTROL},
    {"Door",         4, DEF_ITEM_ATTRIB_DOOR},
    {"NoTarget",     8, DEF_ITEM_ATTRIB_NOTARGET},
    {"Landable",     8, DEF_ITEM_ATTRIB_LANDABLE},
    {"Missile",      7, DEF_ITEM_ATTRIB_MISSILE},
    {"Tire",         4, DEF_ITEM_ATTRIB_TIRE},
    {"FastRope",     8, DEF_ITEM_ATTRIB_FASTROPE},
    {"Takeable",     8, DEF_ITEM_ATTRIB_TAKEABLE},
    {"Easy",         4, DEF_ITEM_ATTRIB_EASY},
    {"S&D",          3, DEF_ITEM_ATTRIB_SD},      /* the S&D/A&D objective target [orig: @0x4a084e..0x4a086d] */
    {"4Team",        5, DEF_ITEM_ATTRIB_4TEAM},
    {"ChangeTeam",   10, DEF_ITEM_ATTRIB_CHANGETEAM},
    {"SpawnPoint",   10, DEF_ITEM_ATTRIB_SPAWNPOINT},
    {"Armory",       6, DEF_ITEM_ATTRIB_ARMORY},
    {"Aidata",       6, DEF_ITEM_ATTRIB_AIDATA},   /* the §5.6 AI-class flag — gates the 0x0D AI-trailer */
    {"LeaveCorpse",  11, DEF_ITEM_ATTRIB_LEAVECORPSE},
    {"NoDismember",  11, DEF_ITEM_ATTRIB_NODISMEMBER},
    {"NoWeapon",     8, DEF_ITEM_ATTRIB_NOWEAPON},
    {"Reflect",      7, DEF_ITEM_ATTRIB_REFLECT},
    {"NoShadow",     8, DEF_ITEM_ATTRIB_NOSHADOW},
    {"Concave",      7, DEF_ITEM_ATTRIB_CONCAVE},
    {"NoScar",       6, DEF_ITEM_ATTRIB_NOSCAR},
    {"NoHud",        5, DEF_ITEM_ATTRIB_NOHUD},
    {"NoDie",        5, DEF_ITEM_ATTRIB_NODIE},
};
static const int item_attrib_table_count =
    sizeof(item_attrib_table) / sizeof(item_attrib_table[0]);

/* items.def `attrib:` tokens -> ItemDefAttrib2 (+0x58) bits, spelled as the chain spells them [orig: the
   strings pushed @ 0x4A0CEC VehicleBay .. @ 0x4A0F18 LandMine]. docs/world/itemdef-re.md:157-160. */
static const FlagEntry item_attrib2_table[] = {
    {"VehicleBay",      10, DEF_ITEM_ATTRIB2_VEHICLEBAY},
    {"AutoInheritTeam", 15, DEF_ITEM_ATTRIB2_AUTOINHERITTEAM},
    {"VehicleSpawn",    12, DEF_ITEM_ATTRIB2_VEHICLESPAWN},
    {"DynamicShadow",   13, DEF_ITEM_ATTRIB2_DYNAMICSHADOW},
    {"StaticShadow",    12, DEF_ITEM_ATTRIB2_STATICSHADOW},
    {"TunnelPiece",     11, DEF_ITEM_ATTRIB2_TUNNELPIECE},
    {"UseVK",           5, DEF_ITEM_ATTRIB2_USEVK},
    {"StaticDeath",     11, DEF_ITEM_ATTRIB2_STATICDEATH},
    {"OnTurret",        8, DEF_ITEM_ATTRIB2_ONTURRET},
    {"HasTurret",       9, DEF_ITEM_ATTRIB2_HASTURRET},
    {"IsTurret",        8, DEF_ITEM_ATTRIB2_ISTURRET},
    {"Farp",            4, DEF_ITEM_ATTRIB2_FARP},
    {"LandMine",        8, DEF_ITEM_ATTRIB2_LANDMINE},
};
static const int item_attrib2_table_count =
    sizeof(item_attrib2_table) / sizeof(item_attrib2_table[0]);

int lookup_item_attrib(const char *name, size_t len) {
    for (int i = 0; i < item_attrib_table_count; ++i) {
        if (len == item_attrib_table[i].name_len &&
            strutil::iequals(std::string_view(name, len), item_attrib_table[i].name))
            return item_attrib_table[i].bit;
    }
    return 0;
}

int lookup_item_attrib2(const char *name, size_t len) {
    for (int i = 0; i < item_attrib2_table_count; ++i) {
        if (len == item_attrib2_table[i].name_len &&
            strutil::iequals(std::string_view(name, len), item_attrib2_table[i].name))
            return item_attrib2_table[i].bit;
    }
    return 0;
}


void validate_header(const char *line, size_t length, size_t key_length, size_t cut,
                     size_t &issues, DefParseReport *report, size_t number, const char *record) {
    // The keyword and the name are the line's first two tokens as the retail
    // tokenizer cuts them, so a comma or a quote ends the keyword as a space
    // does, and no token follows the name [orig: Config_TokenizeLine
    // @0x53CB60's tokens, the delimiters @0x53CC33..0x53CC70].
    const std::string copy(line, length);
    io::ConfigTokens tokens;
    io::tokenize_config_line(copy.c_str(), tokens);
    const bool valid = tokens.count == 2 && strlen(tokens.token(0)) == key_length;
    if (!valid) {
        authoring_issue(issues, report, number, record, line, key_length, DefIssueCode::MalformedBlock);
        return;
    }
    const std::string name = tokens.token(1);
    if (name.size() >= cut)
        authoring_issue(issues, report, number, record, line, key_length, DefIssueCode::Reinterpreted,
                        name.substr(0, cut - 1).c_str());
}

namespace {

// How a family's arm reads a number off its line, the reading the record holds: the CRT's atol, its atof,
// or the engine's digit walker [orig: Math_ParseFixedPoint16 @ 0x6131F0] (parse_fixed16_digits_n;
// DigitsWhole its integer part alone, an ammo's heat_det_range, >> 16). AtolOrAtof: one or the other as an
// earlier line decides (an item's husk_swap_at, its percent's atol before a husk_swap_at_sec, its seconds'
// atof after one [orig: ItemDef_ParseProperty @ 0x49F242..0x49F2C2]).
enum class NumberRead { Atol, Atof, Digits, DigitsWhole, AtolOrAtof };

// The shortest decimal the CRT's atof reads as `value` again.
std::string atof_words(double value) {
    char text[64] = "0";
    for (int precision = 1; precision <= 17; ++precision) {
        std::snprintf(text, sizeof(text), "%.*g", precision, value);
        for (char *c = text; *c; ++c) if (*c == ',') *c = '.'; // the point, whatever the locale's
        if (io::retail_atof(text) == value) break;
    }
    return text;
}

// The shortest decimal the digit walker reads as `word` again (its unsigned 16.16 quotient).
std::string digits_words(int32_t word) {
    char text[64] = "0";
    for (int places = 0; places <= 10; ++places) {
        std::snprintf(text, sizeof(text), "%.*f", places, double(static_cast<uint32_t>(word)) / 65536.0);
        for (char *c = text; *c; ++c) if (*c == ',') *c = '.';
        if (parse_fixed16_digits_n(text, strlen(text)) == word) break;
    }
    return text;
}

// Whether the reading takes the whole of `token` (an atol's number within the 32 bits it saturates at, an
// atof's a finite one) and, where it takes only its start, what it reads it as in `reads_as` (left empty
// where the token's readings disagree, or one is no finite number: the record cannot hold what the game
// reads).
bool whole_number(NumberRead read, const std::string &token, std::string &reads_as) {
    const char *s = token.c_str();
    const size_t n = token.size();
    size_t i = 0, digits = 0;
    const auto digit_run = [&] {
        while (i < n && s[i] >= '0' && s[i] <= '9') ++i, ++digits;
    };
    switch (read) {
    case NumberRead::Atol: {
        // [orig: _atol @0x76AB0A: an optional sign, decimal digits, saturating at the 32 bits]
        if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
        digit_run();
        const int32_t value = io::retail_atol_n(s, n);
        if (i == n && digits && io::retail_atoi64_n(s, n) == value) return true;
        // A decimal of the whole number atol reads ("3000.0") is that number still.
        std::string decimal;
        if (whole_number(NumberRead::Atof, token, decimal) && io::retail_atof_n(s, n) == double(value)) return true;
        reads_as = std::to_string(value);
        return false;
    }
    case NumberRead::Atof: {
        // [orig: _atof @0x76B6A1: an optional sign, digits with at most one '.', an exponent marked e, E, d or D]
        if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
        digit_run();
        if (i < n && s[i] == '.') ++i, digit_run();
        if (digits && i < n && (s[i] == 'e' || s[i] == 'E' || s[i] == 'd' || s[i] == 'D')) {
            size_t at = i + 1;
            if (at < n && (s[at] == '+' || s[at] == '-')) ++at;
            const size_t start = at;
            while (at < n && s[at] >= '0' && s[at] <= '9') ++at;
            if (at > start) i = at;
        }
        const double value = io::retail_atof_n(s, n);
        if (!std::isfinite(value)) return false;
        if (i == n && digits) return true;
        reads_as = atof_words(value);
        return false;
    }
    case NumberRead::Digits:
    case NumberRead::DigitsWhole: {
        // [orig: Math_ParseFixedPoint16 @0x6131F0: digits, then '.' and digits; no sign, no exponent]
        digit_run();
        if (i < n && s[i] == '.') ++i, digit_run();
        const int32_t word = parse_fixed16_digits_n(s, n);
        if (read == NumberRead::Digits) {
            if (i == n && digits) return true;
            reads_as = digits_words(word);
        } else {
            if (i == n && digits && (word & 0xFFFF) == 0) return true;
            reads_as = std::to_string(static_cast<int16_t>(word >> 16));
        }
        return false;
    }
    case NumberRead::AtolOrAtof: {
        std::string as_atol, as_atof;
        const bool whole = whole_number(NumberRead::Atof, token, as_atof);
        if (whole) return true;
        whole_number(NumberRead::Atol, token, as_atol);
        if (!as_atof.empty() && io::retail_atof(as_atof.c_str()) == double(io::retail_atol(as_atol.c_str()))) reads_as = as_atol;
        return false;
    }
    }
    return false;
}

// The reading of column `column` of `property`'s line, its member `field`: by the arm's encoding, else by
// the member's type.
NumberRead number_read(opennova::def::DefRecordKind kind, const std::string &key, const opennova::def::DefProperty &property,
                       const opennova::def::DefField &field, size_t column) {
    using namespace opennova::def;
    switch (property.encoding) {
    case DefEncoding::Fixed16:
    case DefEncoding::FixedSeconds:
    case DefEncoding::TurnRate:
    case DefEncoding::Heat: return NumberRead::Digits;
    // The radius by the walker, the colour's components by atol [orig: AmmoDef_ParseProperty, light_move
    // @0x40AF3A; packed_rgb's atol]; the view's position by atof, its turn by the walker [orig:
    // WeaponDefs_ParseLineCallback @0x54476B..0x5447FF].
    case DefEncoding::LightMove:
    case DefEncoding::LightImpact: return column == 0 ? NumberRead::Digits : NumberRead::Atol;
    case DefEncoding::Pose: return column < 3 ? NumberRead::Atof : NumberRead::Digits;
    case DefEncoding::FloatFixed:
    case DefEncoding::ScaledReal:
    case DefEncoding::DoorOpenRate:
    case DefEncoding::DoorMaxAngle:
    case DefEncoding::HuskSeconds:
    case DefEncoding::ScopeParallax:
    case DefEncoding::ShotTiming: return NumberRead::Atof;
    case DefEncoding::HuskSwap: return NumberRead::AtolOrAtof;
    case DefEncoding::Percent: return NumberRead::Atol; // atoi, clamped [orig: ItemDef_ParseProperty @0x4A1A12]
    default: break;
    }
    // [orig: AmmoDef_ParseProperty @0x40a2d0, heat_det_range: the walker's word >> 16]
    if (kind == DefRecordKind::Ammo && key == "heat_det_range") return NumberRead::DigitsWhole;
    // A weapon's two plain reals, its view's field and its scope's most zoom, are atol's [orig:
    // WeaponDefs_ParseLineCallback, renderfov @0x544840..0x54484C, scope_max_mag @0x544F1E..0x544F29].
    return field.type == DefFieldType::Real && kind != DefRecordKind::Weapon ? NumberRead::Atof : NumberRead::Atol;
}

} // namespace

// Authoring checks supplement the permissive runtime parser. The data still comes
// exclusively from that parser; this function records input that cannot be saved, and
// input the game reads as another value, which the record holds and the writer writes.
// Every family reads its lines through the retail tokenizer
// (defscan::for_each_def_line), and the line here is as the parser read it
// (line_as_read), so its values split the same way: a quoted run is one token
// [orig: Config_TokenizeLine @0x53CB60, quote @0x53CC4E..0x53CC70]. The tokenizer resets
// the line's first two value slots to "" and leaves the rest where an earlier line put
// them [orig: Config_TokenizeLine @0x53CB71..0x53CB81]: a value a line leaves out there
// reads as 0 by every reading, one past them a slot no file states.
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
    // The line read as `value`, which the record holds.
    auto reads_as = [&](const std::string &value) {
        authoring_issue(issues, report, number, record, line, key_length, DefIssueCode::Reinterpreted, value.c_str());
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
    Token tokens[io::kConfigMaxTokens];
    const std::string rest(line + key_length, length - key_length);
    io::ConfigTokens config;
    io::tokenize_config_line(rest.c_str(), config);
    int count = 0;
    for (; count < config.count; ++count) tokens[count] = {config.tokens[count], strlen(config.tokens[count])};
    auto text = [&](int i) { return std::string(tokens[i].s, tokens[i].len); };
    // A token the arm reads for nothing.
    auto ignored = [&](int i) {
        authoring_issue(issues, report, number, record, line, key_length, DefIssueCode::UnknownProperty, text(i).c_str());
    };
    // The number in column `i` as the arm reads it: a token its reading takes whole is the number; one it takes
    // only the start of reads as what that start is, which the record holds and the writer writes (a weapon's
    // `ammobucket 9mm`, atol's 9 [orig: WeaponDefs_ParseLineCallback @0x54402B..0x544045]).
    auto read_number = [&](int i, NumberRead read) {
        std::string value;
        if (whole_number(read, text(i), value)) return;
        if (value.empty()) invalid();
        else reads_as(value);
    };
    auto word = [&](int i) {
        std::string value = text(i);
        for (char &c : value) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return value;
    };
    if (!property) {
        // The aliases' readings, and the values each reads [orig: ItemDef_ParseProperty @0x49EB00: the squib arms'
        // atof @0x49F06F, num_doors / first_door / first_subobject's atol @0x49F766 / @0x49F7BA / @0x49F9B0,
        // door_dir's atol of every token @0x49F872..0x49F8EE, rotor_parts / aux_parts' of tokens 1..4
        // @0x49EF5D..0x49EFB6 / @0x49EFF3..0x49F04C, particletesttime's two atof seconds @0x49FAB0]. A line of
        // rotor_parts or aux_parts with no value reads two slots an earlier line left.
        const bool real = key == "sqb_rate" || key == "sqb_distance" || key == "sqb_error" || key == "particletesttime";
        const bool parts = key == "rotor_parts" || key == "aux_parts";
        const int reads = key == "door_dir" ? count : parts ? 4 : key == "particletesttime" ? 2 : 1;
        if (!count) {
            if (parts) invalid();
            else reads_as("0");
            return;
        }
        for (int i = 0; i < count; ++i) {
            if (i < reads) read_number(i, real ? NumberRead::Atof : NumberRead::Atol);
            else ignored(i);
        }
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
            const auto name = word(i);
            if (!lookup_item_attrib(name.data(), name.size()) && !lookup_item_attrib2(name.data(), name.size()) &&
                name != "parent" && name != "good" && name != "evil")
                ignored(i);
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
            if (i > 1 || word(i) != "nocheckdepth") ignored(i);
        return;
    }
    if (encoding == DefEncoding::AmmoFlags || encoding == DefEncoding::WeaponFlags ||
        encoding == DefEncoding::AmmoKillZone || encoding == DefEncoding::ItemType) {
        // Each arm reads its first value alone, through its table; a name the table lacks stores nothing (an ammo's
        // flag logs "unrecognized flag", its kill zone "bad kill zone type", rounds_kz_null's included, and a
        // weapon's flag "unrecognized flag"; an item's type returns): the line is read for nothing, and so is
        // every token after the first [orig: AmmoDef_ParseProperty @0x40B025..0x40B06F (flag),
        // @0x40AC90..0x40ACD2 (kztype, the range test @0x40ACB0); WeaponDefs_ParseLineCallback
        // @0x543DBD..0x543E26 (flags); ItemDef_ParseProperty @0x4A02FA..0x4A04B7 (type, a miss to the plain
        // return @0x4A1CA6)].
        bool known = false;
        if (count) {
            const auto name = word(0);
            if (encoding == DefEncoding::WeaponFlags) known = lookup_flag(name.data(), name.size()) != nullptr;
            if (encoding == DefEncoding::AmmoFlags)
                for (size_t i = 0; const char *flag = def_ammo_flag_keyword(i); ++i) known |= strutil::iequals(name, flag);
            if (encoding == DefEncoding::AmmoKillZone)
                for (size_t i = 1; const char *zone = def_ammo_kz_keyword(i); ++i) known |= strutil::iequals(name, zone);
            if (encoding == DefEncoding::ItemType) {
                for (const auto &choice : def_field(kind, "type")->choices) known |= name == choice.name;
                known |= name == "foliage" || name == "object";
            }
        }
        if (!known) {
            if (count) ignored(0);
            else invalid(DefIssueCode::UnknownProperty);
        }
        for (int i = 1; i < count; ++i) ignored(i);
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
            const auto piece = word(i);
            const size_t split = piece.find('_');
            const long slot = split == std::string::npos ? 0 : io::retail_atol(piece.substr(0, split).c_str());
            if (split == std::string::npos || slot < 1 || slot > 16 || stored >= 16) {
                authoring_issue(issues, report, number, record, line, key_length, DefIssueCode::UnknownProperty, piece.c_str());
                continue;
            }
            ++stored;
            const std::string name = piece.substr(split + 1);
            if (death_piece_type_lookup(name.data(), name.size()) < 0)
                authoring_issue(issues, report, number, record, line, key_length, DefIssueCode::UnknownProperty, piece.c_str());
        }
        return;
    }
    if (kind == DefRecordKind::Ammo && key == "tracer_type") {
        // Each of the first two values by its name, any other by atol; a line with none stores 0 in both, one value
        // both slots; a third is read by nothing [orig: AmmoDef_ParseProperty @0x40A79D..0x40A7FA over
        // AmmoDef_ParseTypeName @0x40A7A6 / @0x40A7D1, its atol tail].
        if (!count) reads_as("0");
        for (int i = 0; i < count; ++i) {
            if (i >= 2) {
                ignored(i);
                continue;
            }
            bool named = false;
            for (size_t j = 0; const char *name = def_ammo_tracer_keyword(j); ++j) named |= word(i) == name;
            if (!named) read_number(i, NumberRead::Atol);
        }
        return;
    }
    if (encoding == DefEncoding::SpawnMask || encoding == DefEncoding::DoorType) {
        // Every token by atol [orig: ItemDef_ParseProperty, pcvehicle_spawnlist @0x4A0255..0x4A02DE, door_type
        // @0x49F7F0..0x49F86C].
        for (int i = 0; i < count; ++i) read_number(i, NumberRead::Atol);
        return;
    }
    if (encoding == DefEncoding::ClassRounds) {
        // The class by the table walk, which runs seven rows over its five, so a class it does not name (or none)
        // reaches past the table, the CLASS_MANA string's bytes taken for a name's pointer (D-ITEMDEF-12): no file
        // states what that reads. The count is atol's of the next slot, a reset one where the line stops at the
        // class; nothing past it is read [orig: WeaponDefs_ParseLineCallback @0x543AC2..0x543AE2 over the table
        // @0x830EE8, atol of tokens[3] @0x543B03..0x543B07].
        const std::string name = count ? word(0) : std::string();
        if (name != "medic" && name != "sniper" && name != "gunner" && name != "rifleman" && name != "engineer") {
            invalid();
            return;
        }
        if (count == 1) reads_as("0");
        else read_number(1, NumberRead::Atol);
        for (int i = 2; i < count; ++i) ignored(i);
        return;
    }
    // A particle slot takes its effect alone: the arm copies the line's second and third
    // tokens whatever the count, and the tokenizer resets the first three to "" for every
    // line, so a missing userpoint reads as none (jox01's `particlefx fx_Mosquitos_2m_L`)
    // [orig: ItemDef_ParseProperty @ 0x4A13BF..0x4A13FC; Config_TokenizeLine
    // @ 0x53CB71..0x53CB81].
    const int minimum = kind == DefRecordKind::Effect ? 4 : kind == DefRecordKind::Carry ? 2 : encoding == DefEncoding::Pose ? 6 : encoding == DefEncoding::Sight ? 5 :
        encoding == DefEncoding::Attachment ? 2 : 1;
    if (count < minimum) {
        // An empty string remains a serializable draft; semantic validation can
        // require a symbol.
        const auto *first = def_field(property_kind, property->fields.front());
        if (first && first->type == DefFieldType::Text && minimum == 1) return;
        // A line the arm needs more of is read for nothing: an effects row of fewer than four columns ("wrong
        // number of columns" [orig: AmmoDef_ParseProperty @0x40A323..0x40A338]), a view pose of fewer than six
        // values ("too few params" [orig: WeaponDefs_ParseLineCallback @0x5445EE..0x544604, @0x544735..0x54474B]).
        if (kind == DefRecordKind::Effect || encoding == DefEncoding::Pose) {
            invalid(DefIssueCode::UnknownProperty);
            return;
        }
        // A value left out in the first two slots reads as 0: a number key's with no value (its line reading no
        // slot past the second; a light's reads four or five), a carry limit's after its class [orig: abs(atol(
        // tokens[3])) @0x543862..0x543873], an attachment's item id after its user point [orig:
        // ItemDef_ParseProperty, atol of tokens[3] @0x4A1B8E..0x4A1BAD]. Any other reads a slot no file states.
        // (An action's dupsound is not one: its arm reads both values only on a line of both, the parser's own
        // finding, def_weapons.cpp, which this one does not contradict.)
        const bool two_slots = property->fields.size() <= 2 && encoding != DefEncoding::LightMove &&
                               encoding != DefEncoding::LightImpact &&
                               !(kind == DefRecordKind::Action && key == "dupsound");
        if ((count == 0 && first && first->type != DefFieldType::Text && two_slots) ||
            (count == 1 && (kind == DefRecordKind::Carry || encoding == DefEncoding::Attachment)))
            reads_as("0");
        else invalid();
        return;
    }
    // A table row's, a carry limit's and a view pose's values past the ones the arm reads are read by nothing
    // [orig: AmmoDef_ParseProperty, the row's columns @0x40A470..0x40A587; WeaponDefs_ParseLineCallback, the
    // carry's tokens[2..3] @0x5437FE..0x543862, the pose's six @0x54475B..0x5447FF].
    if (kind == DefRecordKind::Effect || kind == DefRecordKind::Carry || encoding == DefEncoding::Pose)
        for (int i = minimum; i < count; ++i) ignored(i);
    if (encoding == DefEncoding::Sight) {
        // By position, as the sights arm reads a row: the blend mode is the sixth value (a name the
        // material maker does not know reads as `blend`), the seventh the one `scale` or `slide` flag
        // and the eighth `slide`'s frame count, read only on a line of 8 tokens or more; any other
        // token is read by nothing [orig: the sights arm @0x544AC8 -- WeaponDef_CreateBlendNamedMaterial
        // @0x540190 over tokens[7] (@0x544B3F), the `cmp [esi],8; jl` @0x544B7A, "scale" @0x544B86 and
        // "slide" @0x544BA2 against tokens[8], atol(tokens[9]) @0x544BC3]. A `slide` with no count
        // reads a slot an earlier line left, a value no file states.
        if (count > 5) {
            const auto mode = word(5);
            if (mode != "blend" && mode != "add" && mode != "multiply" && mode != "blendat" && mode != "addat" &&
                mode != "multiplyat")
                reads_as("blend");
        }
        int read = 6;
        if (count > 6) {
            const auto flag = word(6);
            if (flag == "slide") {
                read = 8;
                if (count > 7) read_number(7, NumberRead::Atol);
                else invalid();
            } else if (flag == "scale") {
                read = 7;
            }
        }
        for (int i = read; i < count; ++i) ignored(i);
    }
    // The addeweap arm reads the userpoint and the id, and the four angles only when the
    // line has more than three tokens, key included; nothing past the sixth argument is
    // read, so a trailing token (jox01's `<down angle> <up angle> ...` placeholders) is
    // ignored. One to three angles read the slots an earlier line left (the tokenizer
    // resets only the first three), a value no file states: invalid. [orig:
    // ItemDef_ParseProperty @ 0x4A1B42..0x4A1C9F, count > 3 @ 0x4A1BB4; Config_TokenizeLine
    // @ 0x53CB71..0x53CB81]
    if (encoding == DefEncoding::Attachment) {
        if (count > 2 && count < 6) invalid();
        for (int i = 6; i < count; ++i) ignored(i);
    }
    size_t columns = property->fields.size();
    if (encoding == DefEncoding::FloatFixed) columns /= 2;
    if (encoding == DefEncoding::Attachment) columns = 6;
    if (encoding == DefEncoding::Sight) columns = 5;
    for (size_t i = 0; i < std::min(columns, size_t(count)); ++i) {
        const auto *field = def_field(property_kind, property->fields[i]);
        if (!field) continue;
        if (field->type == DefFieldType::Text) {
            // A text past what the reader copies: read as its first characters where
            // the reader cuts it (DefField::cut), else past what the record holds.
            if (tokens[i].len >= field->width) {
                if (field->cut) reads_as(std::string(tokens[i].s, field->width - 1));
                else invalid();
            }
        } else if (encoding == DefEncoding::Delay && word(int(i)) == "auto") {
        } else if (encoding == DefEncoding::FloatFixed) {
            // A weight's or an error's number is read twice, by atof into the float and by the digit walker into
            // the 16.16 word beside it [orig: WeaponDefs_ParseLineCallback, weaponweight @0x54410D, clipweight
            // @0x5440DB, error @0x543B21..0x543BB5]: a token both take only the start of reads as the number where
            // one decimal gives both their readings, which the writer puts down.
            std::string value;
            if (whole_number(NumberRead::Atof, text(int(i)), value)) continue;
            if (!value.empty() &&
                parse_fixed16_digits_n(value.data(), value.size()) == parse_fixed16_digits_n(tokens[i].s, tokens[i].len))
                reads_as(value);
            else invalid();
        } else {
            // A ranged member's arm reports a reading outside its range itself, as what it keeps (a weapon's
            // category or rank, an item's unit_type: WeaponDefs_ParseLineCallback @0x543997..0x543A1B,
            // ItemDef_ParseProperty @0x49EE47).
            const NumberRead read = number_read(kind, key, *property, *field, i);
            if (field->ranged) {
                std::string value;
                const int64_t at = io::retail_atol_n(tokens[i].s, tokens[i].len);
                if (whole_number(read, text(int(i)), value) || at < field->min || at > field->max) continue;
            }
            read_number(int(i), read);
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

/* Parse HudColor from RGB values (hudpos only; each channel is atof then
   ftol, e.g. hud_textcolor @0x5A0F4D..0x5A0F82) */
DefHudColor parse_hud_color(Token *vals, int n) {
    DefHudColor c = {0, 0, 0, 255};
    if (n >= 3) {
        c.r = hud_number(vals[0].s, vals[0].len);
        c.g = hud_number(vals[1].s, vals[1].len);
        c.b = hud_number(vals[2].s, vals[2].len);
        if (n >= 4) c.a = hud_number(vals[3].s, vals[3].len);
    }
    return c;
}

/* Parse HudColor from ARGB values */
DefHudColor parse_hud_color_argb(Token *vals, int n) {
    DefHudColor c = {0, 0, 0, 255};
    if (n >= 4) {
        c.a = hud_number(vals[0].s, vals[0].len);
        c.r = hud_number(vals[1].s, vals[1].len);
        c.g = hud_number(vals[2].s, vals[2].len);
        c.b = hud_number(vals[3].s, vals[3].len);
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
    if (n >= 1) out[0] = hud_number(vals[0].s, vals[0].len);
    if (n >= 2) out[1] = hud_number(vals[1].s, vals[1].len);
    if (n >= 3) out[2] = hud_number(vals[2].s, vals[2].len);
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
    if (n >= 1) out[0] = hud_number(vals[0].s, vals[0].len);
    if (n >= 2) out[1] = hud_number(vals[1].s, vals[1].len);
    if (n >= 3) {
        char low[16];
        size_t ll = vals[2].len < 15 ? vals[2].len : 15;
        to_lower_buf(low, vals[2].s, ll);
        out[2] = parse_alignment(low, ll);
    }
}


double hud_double(const char *s, size_t len) {
    return io::retail_atof_n(s, len);
}

int hud_number(const char *s, size_t len) {
    return io::retail_ftol_sse2(hud_double(s, len));
}

int parse_fixed16_digits_n(const char *s, size_t len) {
    size_t i = 0;
    // 32-bit arithmetic that wraps, as the original's imul/lea do on a long
    // digit run.
    uint32_t integer_part = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        integer_part = static_cast<uint32_t>(s[i]) + 10u * integer_part - '0';
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
    return static_cast<int>((integer_part << 16) + static_cast<uint32_t>(frac_accum >> 8));
}

}  // namespace opennova::defscan