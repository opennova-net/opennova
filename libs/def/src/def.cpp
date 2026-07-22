#include "def/def.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================= */
/* Helpers                                                                   */
/* ========================================================================= */

static char *read_file(const char *path, size_t *out_len) {
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

static const char *skip_ws(const char *s) {
    while (*s && (*s == ' ' || *s == '\t')) ++s;
    return s;
}

static const char *skip_ws_all(const char *s) {
    while (*s && isspace((unsigned char)*s)) ++s;
    return s;
}

static size_t rtrim_len(const char *s, size_t len) {
    while (len > 0 && isspace((unsigned char)s[len - 1])) --len;
    return len;
}

static void safe_copy(char *dst, size_t dst_size, const char *src, size_t src_len) {
    if (src_len >= dst_size) src_len = dst_size - 1;
    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
}

/* Trim leading+trailing whitespace, return pointer and length (no alloc) */
static const char *trim_span(const char *s, size_t len, size_t *out_len) {
    while (len > 0 && isspace((unsigned char)*s)) { ++s; --len; }
    while (len > 0 && isspace((unsigned char)s[len - 1])) --len;
    *out_len = len;
    return s;
}

static void to_lower_buf(char *dst, const char *src, size_t len) {
    for (size_t i = 0; i < len; ++i)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[len] = '\0';
}

/* Extract quoted string from line: first "..." pair */
static size_t extract_quoted(const char *line, size_t line_len, char *dst, size_t dst_size) {
    const char *q0 = (const char *)memchr(line, '"', line_len);
    if (!q0) { dst[0] = '\0'; return 0; }
    size_t rem = line_len - (size_t)(q0 - line) - 1;
    const char *q1 = (const char *)memchr(q0 + 1, '"', rem);
    if (!q1 || q1 <= q0 + 1) { dst[0] = '\0'; return 0; }
    size_t slen = (size_t)(q1 - q0 - 1);
    safe_copy(dst, dst_size, q0 + 1, slen);
    return slen;
}

/* consume_value: skip key_len chars, trim, strip quotes, strip // comment */
static const char *consume_value_span(const char *line, size_t line_len, size_t key_len, size_t *out_len) {
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

static void consume_value_str(const char *line, size_t line_len, size_t key_len, char *dst, size_t dst_size) {
    size_t vlen;
    const char *v = consume_value_span(line, line_len, key_len, &vlen);
    safe_copy(dst, dst_size, v, vlen);
}

static int parse_int_n(const char *s, size_t len) {
    char buf[32];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, s, len);
    buf[len] = '\0';
    return (int)strtol(buf, NULL, 10);
}

/* ItemDef healthMax and the two armor classes are signed WORD stores in retail.
   Keep the normalized C ABI carrier as int, but wrap to 16 bits and sign-extend at
   parse time rather than relying on implementation-defined narrowing casts. */
static int signed_i16_value(int value) {
    const unsigned int low = (unsigned int)value & 0xFFFFu;
    return low < 0x8000u ? (int)low : (int)low - 0x10000;
}

static float parse_float_n(const char *s, size_t len) {
    char buf[64];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, s, len);
    buf[len] = '\0';
    return (float)strtod(buf, NULL);
}

/* Tokenize by whitespace, returns count. Stores start+len pairs. Max tokens. */
#define MAX_TOKENS 16
typedef struct { const char *s; size_t len; } Token;

static int tokenize(const char *s, size_t len, Token *tokens, int max_tok) {
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
   lives in libs/world/destruction.cpp, both citing the same original.
   [orig: g_death_piece_types @ 0x8404f0; DeathPieceType_FindByName @ 0x57b310] */
static const char *const k_death_piece_type_names[13] = {
    "HULL",      "WHEEL",     "CHUNK_S",   "CHUNK_M",   "CHUNK_L",
    "ROCK_S",    "ROCK_M",    "ROCK_L",    "CHUNKNP_S", "CHUNKNP_M",
    "CHUNKNP_L", "CACTUS_",   "CHUNKSF_M",
};

static int death_piece_type_index(const char *name, size_t len) {
    for (int i = 0; i < 13; ++i) {
        const char *t = k_death_piece_type_names[i];
        size_t j = 0;
        while (j < len && t[j] != '\0' &&
               tolower((unsigned char)t[j]) == tolower((unsigned char)name[j]))
            ++j;
        if (j == len && t[j] == '\0') return i;
    }
    return 0; /* unknown -> HULL, the engine's zero-init read */
}

/* Split on commas and/or whitespace */
static int split_values(const char *s, size_t len, Token *tokens, int max_tok) {
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
static int lower_starts_with(const char *lower, size_t lower_len, const char *prefix, size_t prefix_len) {
    if (lower_len < prefix_len) return 0;
    return memcmp(lower, prefix, prefix_len) == 0;
}

/* Check prefix + next char is whitespace or end */
static int lower_match_key(const char *lower, size_t lower_len, const char *prefix, size_t prefix_len) {
    if (!lower_starts_with(lower, lower_len, prefix, prefix_len)) return 0;
    if (lower_len == prefix_len) return 1;
    return isspace((unsigned char)lower[prefix_len]);
}

/* Dynamic array helpers */
#define DA_PUSH(arr, count, cap, elem) do { \
    if ((count) >= (cap)) { \
        (cap) = (cap) ? (cap) * 2 : 8; \
        (arr) = (decltype(arr))realloc((arr), (cap) * sizeof(*(arr))); \
    } \
    (arr)[(count)++] = (elem); \
} while(0)

#define DA_PUSH_RAW(raw_lines, raw_count, raw_cap, line, line_len) do { \
    if ((raw_count) >= (raw_cap)) { \
        (raw_cap) = (raw_cap) ? (raw_cap) * 2 : 8; \
        (raw_lines) = (decltype(raw_lines))realloc((raw_lines), (raw_cap) * sizeof(*(raw_lines))); \
    } \
    size_t _cplen = (line_len) < 511 ? (line_len) : 511; \
    memcpy((raw_lines)[(raw_count)], (line), _cplen); \
    (raw_lines)[(raw_count)][_cplen] = '\0'; \
    (raw_count)++; \
} while(0)

/* Line iterator: walks through buf splitting on \n, stripping \r */
typedef struct {
    const char *buf;
    size_t buf_len;
    size_t pos;
} LineIter;

static int next_line(LineIter *it, const char **out, size_t *out_len) {
    if (it->pos >= it->buf_len) return 0;
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
typedef struct { const char *name; size_t name_len; int bit; int bit2; } FlagEntry;
static const FlagEntry flag_table[] = {
    {"scoped",          6, 0x00000001, 0},
    {"sighted",         7, 0x00000002, 0},
    {"underwater",     10, 0x00000004, 0},
    {"showcomander",   12, 0x00000008, 0},
    {"noclipsnodraw",  13, 0x00000010, 0},
    {"burst",           5, 0x00000020, 0},
    {"notdropable",    11, 0x00000040, 0},
    {"emplaced",        8, 0x00000080, 0},
    {"auto",            4, 0x00000100, 0},
    {"norangecheck",   12, 0x00000200, 0},
    {"showrange",       9, 0x00000400, 0},
    {"showelevation",  13, 0x00000800, 0},
    {"armor",           5, 0x00001000, 0},
    {"okwhilejumping", 14, 0x00002000, 0},
    {"onlyfirescoped", 14, 0x00004000, 0},
    {"lollypop",        8, 0x00008000, 0},
    {"absorbpitch",    11, 0x00010000, 0},
    {"nomove",          6, 0x00020000, 0},
    {"forcecrouch",    11, 0x00040000, 0},
    {"onlyscoped",     10, 0x00080000, 0},
    {"2dimpact",        8, 0x00100000, 0},
    {"usedesignator",  13, 0x00200000, 0},
    {"usespreadtwo",   12, 0x00400000, 0},
    {"showimpactdist", 14, 0x00800000, 0},
    {"whileswimming",  13, 0x01000000, 0},
    {"nocardswitch",   12, 0x02000000, 0},
    {"handgunup",       9, 0x04000000, 0},
    {"quickswitch",    11, 0x08000000, 0},
    {"onlyfirelocked", 14, 0x10000000, 0},
    {"forcescoped",    11, 0x20000000, 0},
    {"laserbeam",       9, 0x40000000, 0},
    {"powerthrow",     10, (int)0x80000000, 0},
    {"noselect",        8, 0, 0x00000001},
    {"parachute",       9, 0, 0x00000002},
    {"thermal",         7, 0, 0x00000004},
    {"monitor",         7, 0, 0x00000008},
    {"viewlock",        8, 0, 0x00000010},
    {"onlylockscoped", 14, 0, 0x00000020},
    {"noammotypes",    11, 0, 0x00000040},
    {"showhudpip",     10, 0, 0x00000080},
    {"fixverticalofst",15, 0, 0x00000100},
    {"inset",           5, 0, 0x00000200},
    {"noautozero",     10, 0, 0x00000400},
    {"invisible",       9, 0, 0x00000800},
};
static const int flag_table_count = sizeof(flag_table) / sizeof(flag_table[0]);

static const FlagEntry *lookup_flag(const char *name, size_t len) {
    for (int i = 0; i < flag_table_count; ++i) {
        if (len == flag_table[i].name_len && memcmp(name, flag_table[i].name, len) == 0)
            return &flag_table[i];
    }
    return NULL;
}

/* items.def `attrib:` tokens -> ItemDefAttrib (+0x54) bits. Token names lowercased (the
   parser lowercases before compare). Full witnessed map: docs/world/itemdef-re.md:147-155.
   [orig: ItemDef_ParseProperty @0x49eb00] */
static const FlagEntry item_attrib_table[] = {
    {"movecb",       6, 0x1},
    {"powerup",      7, 0x2},
    {"nomoveshoot",  11, 0x4},
    {"notool",       6, 0x8},
    {"snap",         4, 0x10},
    {"eweap",        5, 0x20},
    {"playercontrol",13, 0x40},
    {"door",         4, 0x80},
    {"notarget",     8, 0x100},
    {"landable",     8, 0x200},
    {"missile",      7, 0x400},
    {"tire",         4, 0x800},
    {"fastrope",     8, 0x1000},
    {"takeable",     8, 0x2000},
    {"easy",         4, 0x4000},
    {"4team",        5, 0x10000},
    {"changeteam",   10, 0x20000},
    {"spawnpoint",   10, 0x40000},
    {"armory",       6, 0x80000},
    {"aidata",       6, 0x100000},   /* the §5.6 AI-class flag — gates the 0x0D AI-trailer */
    {"leavecorpse",  11, 0x400000},
    {"nodismember",  11, 0x800000},
    {"noweapon",     8, 0x1000000},
    {"reflect",      7, 0x2000000},
    {"noshadow",     8, 0x4000000},
    {"concave",      7, 0x8000000},
    {"noscar",       6, 0x10000000},
    {"nohud",        5, 0x20000000},
    {"nodie",        5, 0x40000000},
};
static const int item_attrib_table_count =
    sizeof(item_attrib_table) / sizeof(item_attrib_table[0]);

/* items.def `attrib:` tokens -> ItemDefAttrib2 (+0x58) bits. docs/world/itemdef-re.md:157-160. */
static const FlagEntry item_attrib2_table[] = {
    {"vehiclebay",      10, 0x1},
    {"autoinheritteam", 15, 0x2},
    {"vehiclespawn",    12, 0x4},
    {"dynamicshadow",   13, 0x10},
    {"staticshadow",    12, 0x20},
    {"tunnelpiece",     11, 0x40},
    {"usevk",           5, 0x80},
    {"staticdeath",     11, 0x100},
    {"onturret",        8, 0x400},
    {"hasturret",       9, 0x800},
    {"isturret",        8, 0x1000},
    {"farp",            4, 0x2000},
    {"landmine",        8, 0x4000},
};
static const int item_attrib2_table_count =
    sizeof(item_attrib2_table) / sizeof(item_attrib2_table[0]);

static int lookup_item_attrib(const char *name, size_t len) {
    for (int i = 0; i < item_attrib_table_count; ++i) {
        if (len == item_attrib_table[i].name_len &&
            memcmp(name, item_attrib_table[i].name, len) == 0)
            return item_attrib_table[i].bit;
    }
    return 0;
}

static int lookup_item_attrib2(const char *name, size_t len) {
    for (int i = 0; i < item_attrib2_table_count; ++i) {
        if (len == item_attrib2_table[i].name_len &&
            memcmp(name, item_attrib2_table[i].name, len) == 0)
            return item_attrib2_table[i].bit;
    }
    return 0;
}

/* Alignment parser: left=0, right=1, center=2 */
static int parse_alignment(const char *s, size_t len) {
    if (len == 5 && memcmp(s, "right", 5) == 0) return 1;
    if (len == 6 && memcmp(s, "center", 6) == 0) return 2;
    return 0;
}

/* Parse N ints from value string */
static int parse_ints(const char *s, size_t len, int *out, int max_n) {
    Token tok[MAX_TOKENS];
    int n = split_values(s, len, tok, max_n < MAX_TOKENS ? max_n : MAX_TOKENS);
    for (int i = 0; i < n && i < max_n; ++i)
        out[i] = parse_int_n(tok[i].s, tok[i].len);
    return n;
}

/* Parse N floats from value string (with comma replacement) */
static int parse_floats(const char *s, size_t len, float *out, int max_n) {
    Token tok[MAX_TOKENS];
    int n = split_values(s, len, tok, max_n < MAX_TOKENS ? max_n : MAX_TOKENS);
    for (int i = 0; i < n && i < max_n; ++i)
        out[i] = parse_float_n(tok[i].s, tok[i].len);
    return n;
}

/* Parse HudColor from RGB values */
static DefHudColor parse_hud_color(Token *vals, int n) {
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
static DefHudColor parse_hud_color_argb(Token *vals, int n) {
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
static void parse_pos_aligned(Token *vals, int n, int *out) {
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

/* ========================================================================= */
/* Ammo Parsing                                                              */
/* ========================================================================= */

/* `flag <name>` -> bit, matched first-name-wins in TABLE ORDER (three 'internal' names
 * exist; a data file writing "internal" hits 0x40000 first, faithfully).
 * [orig: the 30-entry table @0x813500; AmmoDef_ParseProperty @0x40a2d0] */
static const struct { const char *name; unsigned int bit; } k_ammo_flag_names[] = {
    {"ignoredmg", 0x1u},        {"ignore", 0x2u},
    {"shrapnel", 0x4u},         {"silenced", 0x8u},
    {"water", 0x10u},           {"detonatesatchels", 0x20u},
    {"nosmoke", 0x40u},         {"nocollide", 0x80u},
    {"nogravity", 0x100u},      {"hasitem", 0x200u},
    {"instantkillzone", 0x400u},{"ownerimmune", 0x800u},
    {"useownmove", 0x2000u},    {"noage", 0x4000u},
    {"forcetracer", 0x8000u},   {"shotgun", 0x10000u},
    {"claymore", 0x20000u},     {"nooitems", 0x80000u},
    {"nomitems", 0x100000u},    {"noditems", 0x200000u},
    {"ignorfoilage", 0x4000000u},{"priority", 0x800000u},
    {"clipwater", 0x1000000u},  {"clipwaterfx", 0x20000000u},
    {"designatetarget", 0x2000000u}, {"lawr", 0x8000000u},
    {"fgrenade", 0x10000000u},  {"internal", 0x40000u},
    {"internal", 0x1000u},      {"internal", 0x400000u},
};

/* `kztype rounds_kz_<X>` -> index 1..7; 0/unknown rejected like the original's
 * "bad kill zone type" warning path. [orig: 8-name table @0x8133E0] */
static const char *k_ammo_kz_names[8] = {
    "rounds_kz_null",   "rounds_kz_knife", "rounds_kz_standard", "rounds_kz_medic",
    "rounds_kz_radiusblast", "rounds_kz_c4", "rounds_kz_bullets", "rounds_kz_slash",
};

/* `tracer_type` style name -> engine id; unknown names fall back to atol like the
 * original (note the gap at 8). [orig: AmmoDef_ParseTypeName, called from
 * AmmoDef_ParseProperty @0x40a7a6/@0x40a7d1] */
static const struct { const char *name; int id; } k_ammo_tracer_type_names[] = {
    {"stdred", 1},   {"stdgreen", 2},   {"rocket", 3},      {"at4", 4},
    {"grenade", 5},  {"rapidred", 6},   {"rapidgreen", 7},  {"sniperred", 9},
    {"snipergreen", 10}, {"df1red", 11}, {"df1green", 12},
};

static int ammo_tracer_type_from_name(const char *s, size_t len) {
    char nm[48];
    size_t n = len < sizeof(nm) - 1 ? len : sizeof(nm) - 1;
    to_lower_buf(nm, s, n);
    for (size_t i = 0; i < sizeof(k_ammo_tracer_type_names) / sizeof(k_ammo_tracer_type_names[0]);
         ++i) {
        if (strlen(k_ammo_tracer_type_names[i].name) == n &&
            memcmp(k_ammo_tracer_type_names[i].name, nm, n) == 0)
            return k_ammo_tracer_type_names[i].id;
    }
    return parse_int_n(s, len); /* atol fallback [orig: AmmoDef_ParseTypeName tail] */
}

/* Decimal string -> 16.16 fixed point (integer math; matches the values ammo.def uses:
 * "1", "0.5", ".04"). [orig: Math_ParseFixedPoint16 @0x6131f0] */
static int parse_fixed16_n(const char *s, size_t len) {
    size_t i = 0;
    int neg = 0;
    long long ip = 0, fp = 0, scale = 1;
    if (i < len && (s[i] == '-' || s[i] == '+')) neg = (s[i] == '-'), ++i;
    for (; i < len && s[i] >= '0' && s[i] <= '9'; ++i) ip = ip * 10 + (s[i] - '0');
    if (i < len && s[i] == '.') {
        for (++i; i < len && s[i] >= '0' && s[i] <= '9' && scale < 1000000; ++i) {
            fp = fp * 10 + (s[i] - '0');
            scale *= 10;
        }
    }
    long long v = (ip << 16) + (fp * 65536 + scale / 2) / scale;
    return (int)(neg ? -v : v);
}

/* The engine's decimal -> 16.16 digit walker, translated exactly rather than
   re-derived. It accumulates the integer part by digits, then walks the fractional
   digits with a scale that starts at 2^24 and is repeatedly multiplied by 419430/2^22
   (0.09999990 — a hair UNDER 1/10), seeding the accumulator with 127 so the closing
   >> 8 rounds to nearest. No sign and no exponent: a leading '-' terminates the
   integer scan and yields 0, exactly as the original does.

   This is deliberately NOT parse_fixed16_n above. That helper is a clean round-half-up
   conversion, and the two disagree by one 16.16 LSB on roughly 4% of decimal forms
   (e.g. "0.07" -> 4587 here, 4588 there) because the original's per-digit scale drifts
   low. Callers whose value is consumed as a magnitude can live with a 1-LSB shift;
   callers who divide two parsed values against each other cannot, which is why the
   heat keys use this one. The same 1-LSB gap in parse_fixed16_n's own callers (the
   ammo.def magnitudes) is tracked as D-WPN-30.
   [orig: Math_ParseFixedPoint16 @ 0x6131f0] */
static int parse_fixed16_digits_n(const char *s, size_t len) {
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

/* Parsed 16.16 seconds -> 62 Hz ticks with rounding. [orig: sub_40A0F0 @0x40a0f0 —
 * (62 * fp16 + 0x8000) >> 16] */
static int parse_age_ticks_n(const char *s, size_t len) {
    return (int)(((long long)62 * parse_fixed16_n(s, len) + 0x8000) >> 16);
}

static int parse_ammo_buffer(char *buf, size_t file_len, DefAmmoFile *out);

DEF_EXPORT int def_parse_ammo(const char *path, DefAmmoFile *out) {
    memset(out, 0, sizeof(*out));

    size_t file_len;
    char *buf = read_file(path, &file_len);
    if (!buf) return -1;
    int rc = parse_ammo_buffer(buf, file_len, out);
    free(buf);
    return rc;
}

DEF_EXPORT int def_parse_ammo_memory(const uint8_t *data, size_t size, DefAmmoFile *out) {
    memset(out, 0, sizeof(*out));
    if (!data) return -1;
    char *buf = (char *)malloc(size + 1);
    if (!buf) return -1;
    memcpy(buf, data, size);
    buf[size] = '\0';
    int rc = parse_ammo_buffer(buf, size, out);
    free(buf);
    return rc;
}

static int parse_ammo_buffer(char *buf, size_t file_len, DefAmmoFile *out) {

    size_t entries_cap = 0;
    LineIter it = {buf, file_len, 0};
    const char *line; size_t line_len;

    DefAmmoDef current;
    memset(&current, 0, sizeof(current));
    int in_block = 0, in_effects = 0;
    size_t raw_cap = 0, eff_cap = 0;

    char lower[1024];

    while (next_line(&it, &line, &line_len)) {
        size_t tlen;
        const char *trimmed = trim_span(line, line_len, &tlen);
        if (tlen == 0) continue;

        size_t ll = tlen < sizeof(lower) - 1 ? tlen : sizeof(lower) - 1;
        to_lower_buf(lower, trimmed, ll);

        /* Effects table */
        if (lower_starts_with(lower, ll, "effects_table", 13)) {
            in_effects = 1;
            continue;
        }
        if (in_effects) {
            if (ll == 3 && memcmp(lower, "end", 3) == 0) {
                in_effects = 0;
                continue;
            }
            Token tok[MAX_TOKENS];
            int n = tokenize(trimmed, tlen, tok, MAX_TOKENS);
            if (n >= 4) {
                DefEffectTableEntry e;
                memset(&e, 0, sizeof(e));
                safe_copy(e.surface_type, sizeof(e.surface_type), tok[0].s, tok[0].len);
                safe_copy(e.hit_effect, sizeof(e.hit_effect), tok[1].s, tok[1].len);
                safe_copy(e.impact_sound, sizeof(e.impact_sound), tok[2].s, tok[2].len);
                e.value = parse_int_n(tok[3].s, tok[3].len);
                DA_PUSH(current.effects_table, current.effects_table_count, eff_cap, e);
            }
            continue;
        }

        /* Block start */
        if (!in_block && lower_starts_with(lower, ll, "ammo ", 5)) {
            memset(&current, 0, sizeof(current));
            raw_cap = 0; eff_cap = 0;
            size_t nlen;
            const char *nm = trim_span(trimmed + 5, tlen - 5, &nlen);
            safe_copy(current.name, sizeof(current.name), nm, nlen);
            in_block = 1;
            continue;
        }

        if (!in_block) continue;

        if (ll == 3 && memcmp(lower, "end", 3) == 0) {
            DA_PUSH(out->entries, out->count, entries_cap, current);
            memset(&current, 0, sizeof(current));
            raw_cap = 0; eff_cap = 0;
            in_block = 0;
            continue;
        }

        int parsed = 0;
        if (lower_starts_with(lower, ll, "velocity", 8)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 8, &vl);
            current.velocity = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "min_damage", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            current.min_damage = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "max_damage", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            current.max_damage = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "penetration_impact", 18)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 18, &vl);
            current.penetration_impact = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "penetration_kz", 14)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 14, &vl);
            current.penetration_kz = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "recoil", 6)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 6, &vl);
            parse_ints(v, vl, current.recoil, 3);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "flag", 4)) {
            /* OR the named bit; first table match wins [orig: @0x813500 walk]. An
               unrecognized name is skipped (the original warns). */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 4, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1) {
                char fl[64];
                size_t fn = tok[0].len < sizeof(fl) - 1 ? tok[0].len : sizeof(fl) - 1;
                to_lower_buf(fl, tok[0].s, fn);
                for (size_t fi = 0; fi < sizeof(k_ammo_flag_names) / sizeof(k_ammo_flag_names[0]); ++fi) {
                    if (strlen(k_ammo_flag_names[fi].name) == fn &&
                        memcmp(k_ammo_flag_names[fi].name, fl, fn) == 0) {
                        current.flags |= k_ammo_flag_names[fi].bit;
                        break;
                    }
                }
            }
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "max_age", 7)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 7, &vl);
            current.max_age_ticks = parse_age_ticks_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "arm_age", 7)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 7, &vl);
            current.arm_age_ticks = parse_age_ticks_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "error", 5)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 5, &vl);
            current.error_fp16 = parse_fixed16_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "drag", 4)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 4, &vl);
            current.drag_fp16 = parse_fixed16_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "bullet_radius", 13)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 13, &vl);
            current.bullet_radius_fp16 = parse_fixed16_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "spread_count", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            current.spread_count = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "kztype", 6)) {
            /* Full-name match against the 8-entry table; only 1..7 accepted
               [orig: @0x40a2d0 rejects index 0/unknown as "bad kill zone type"]. */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 6, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1) {
                char kz[48];
                size_t kn = tok[0].len < sizeof(kz) - 1 ? tok[0].len : sizeof(kz) - 1;
                to_lower_buf(kz, tok[0].s, kn);
                for (int ki = 1; ki < 8; ++ki) {
                    if (strlen(k_ammo_kz_names[ki]) == kn &&
                        memcmp(k_ammo_kz_names[ki], kz, kn) == 0) {
                        current.kztype = ki;
                        break;
                    }
                }
            }
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "kz_damage", 9)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
            current.kz_damage = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "kz_minradius", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            current.kz_minradius_fp16 = parse_fixed16_n(v, vl); /* +52 [orig: §5.60 map] */
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "kz_maxradius", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            current.kz_maxradius_fp16 = parse_fixed16_n(v, vl); /* +56 */
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "kz_pieslice", 11)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 11, &vl);
            /* HALF-angle: retail stores (deg / 2) x 11930464 BAM — the cone
             * tests compare |angle diff| <= this half-angle.
             * [orig: atol -> cdq/sub/sar signed div 2 -> imul 0xB60B60 @0x40ad51] */
            current.kz_pieslice_bam = (parse_int_n(v, vl) / 2) * 11930464;
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "min_stable_velocity", 19)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 19, &vl);
            current.min_stable_velocity = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "tumble_error", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            current.tumble_error_fp16 = parse_fixed16_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "weight_in_grains", 16)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 16, &vl);
            current.weight_in_grains = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "tracerrate", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            current.tracer_rate = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "ai_launcheffect", 15)) {
            /* Must precede "ai_launch" in this starts_with chain (prefix collision;
               the original compares exact tokens [orig: @0x40a8fc stricmp]). */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 15, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.ai_launcheffect, sizeof(current.ai_launcheffect), tok[0].s,
                          tok[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "ai_launch", 9)) {
            /* The AI fire sound-set name; the original resolves the set pointer here
               [orig: @0x40a8c8-0x40a8ef SoundBank_FindSetByNameAnyBank -> +64]. */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.ai_launch, sizeof(current.ai_launch), tok[0].s, tok[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "mf_light", 8)) {
            /* Presence sets the +36 flag, the value lands beside it
               [orig: @0x40a81b dword +36 = 1; @0x40a826-0x40a837 +40 = atol]. */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 8, &vl);
            current.mf_light = 1;
            current.mf_light_value = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "tracer_type", 11)) {
            /* 1-2 style names; a single value fills both slots
               [orig: @0x40a79d-0x40a7fa: argc >= 2 -> +232, argc >= 3 -> +236,
               else +236 = +232]. */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 11, &vl);
            Token tok[2];
            int tn = tokenize(v, vl, tok, 2);
            if (tn >= 1) {
                current.tracer_type_friendly = ammo_tracer_type_from_name(tok[0].s, tok[0].len);
                current.tracer_type_enemy =
                        tn >= 2 ? ammo_tracer_type_from_name(tok[1].s, tok[1].len)
                                : current.tracer_type_friendly;
            }
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "notarmmedammo", 13)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 13, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.notarmmed_ammo, sizeof(current.notarmmed_ammo), tok[0].s,
                          tok[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "frndlytrcrid", 12)) {
            /* The tracer round's friendly item model, by ITEMS.DEF type id. The
               original resolves to an item index here (name fallback, warning on
               miss) [orig: @0x40a5f8-0x40a63f -> +16]; we keep the raw type id. */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            current.frndly_trcr_type_id = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "foetrcrid", 9)) {
            /* [orig: @0x40a646-0x40a68d -> +20] */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
            current.foe_trcr_type_id = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "light_move", 10)) {
            /* The in-flight round glow: radius (16.16) + packed RGB
               [orig: @0x40a2d0 'light_move' -> +120 = ParseFixedPoint16,
               +124 = (atol(r) << 16) | (atol(g) << 8) | atol(b)]. */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            Token tok[4];
            int tn = tokenize(v, vl, tok, 4);
            if (tn >= 1) current.light_move_radius_fp16 = parse_fixed16_n(tok[0].s, tok[0].len);
            if (tn >= 4) {
                /* No range clamps — the original's shifted adds bleed out-of-range
                   components upward [orig: ((r<<8)+g)<<8 + b @0x40a2d0]. */
                current.light_move_color =
                        ((parse_int_n(tok[1].s, tok[1].len) << 8) +
                         parse_int_n(tok[2].s, tok[2].len)) * 256 +
                        parse_int_n(tok[3].s, tok[3].len);
            }
            parsed = 1;
        }

        if (!parsed) {
            DA_PUSH_RAW(current.raw_lines, current.raw_lines_count, raw_cap, line, line_len);
        }
    }

    return 0;
}

DEF_EXPORT void def_free_ammo(DefAmmoFile *f) {
    if (!f) return;
    for (size_t i = 0; i < f->count; ++i) {
        free(f->entries[i].effects_table);
        free(f->entries[i].raw_lines);
    }
    free(f->entries);
    memset(f, 0, sizeof(*f));
}

/* ========================================================================= */
/* Weapons Parsing                                                           */
/* ========================================================================= */

/* Shared buffer parser for weapon.def, used by both the path and memory entry points. */
static int parse_weapons_buf(const char *buf, size_t file_len, DefWeaponsFile *out) {
    enum { ST_TOP, ST_WEAPON, ST_ACTION };
    int state = ST_TOP;

    size_t entries_cap = 0, acl_cap = 0;
    DefWeaponDef cw; memset(&cw, 0, sizeof(cw));
    DefWeaponAction ca; memset(&ca, 0, sizeof(ca));
    size_t cw_raw_cap = 0, cw_act_cap = 0, cw_sight_cap = 0;
    size_t ca_raw_cap = 0;

    LineIter it = {buf, file_len, 0};
    const char *line; size_t line_len;
    char lower[1024];

    while (next_line(&it, &line, &line_len)) {
        size_t tlen;
        const char *trimmed = trim_span(line, line_len, &tlen);
        if (tlen == 0) continue;

        size_t ll = tlen < sizeof(lower) - 1 ? tlen : sizeof(lower) - 1;
        to_lower_buf(lower, trimmed, ll);

        /* Top-level ammoclass_max_carry */
        if (lower_starts_with(lower, ll, "ammoclass_max_carry", 19)) {
            DA_PUSH_RAW(out->ammo_class_lines, out->ammo_class_lines_count, acl_cap, line, line_len);
            continue;
        }

        if (state == ST_TOP) {
            if (lower_starts_with(lower, ll, "weapon", 6)) {
                memset(&cw, 0, sizeof(cw));
                cw_raw_cap = 0; cw_act_cap = 0; cw_sight_cap = 0;
                /* [orig: AdmDef_InitEntryDefaults @ 0x53ff31 seeds renderfov = 80.0] */
                cw.renderfov = 80.0f;
                extract_quoted(trimmed, tlen, cw.weapon_name, sizeof(cw.weapon_name));
                state = ST_WEAPON;
            }
            continue;
        }

        if (state == ST_WEAPON) {
            if (lower_starts_with(lower, ll, "action", 6)) {
                memset(&ca, 0, sizeof(ca));
                ca_raw_cap = 0;
                extract_quoted(trimmed, tlen, ca.name, sizeof(ca.name));
                state = ST_ACTION;
                continue;
            }

            if (ll == 3 && memcmp(lower, "end", 3) == 0) {
                DA_PUSH(out->entries, out->count, entries_cap, cw);
                memset(&cw, 0, sizeof(cw));
                cw_raw_cap = 0; cw_act_cap = 0; cw_sight_cap = 0;
                state = ST_TOP;
                continue;
            }

            int parsed = 0;
            if (lower_match_key(lower, ll, "category", 8)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 8, &vl);
                cw.category = parse_int_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "rank", 4)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 4, &vl);
                cw.rank = parse_int_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "clipsize", 8)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 8, &vl);
                cw.clipsize = parse_int_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "startrounds", 11)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 11, &vl);
                cw.startrounds = parse_int_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "classrounds", 11)) {
                /* classrounds <class> <n> — the class token resolves through the
                   char-class VALUE table (medic=1 sniper=2 gunner=3 rifleman=5
                   engineer=6, must be <= 6) and stores at the value's index
                   [orig: handler @ 0x543ab0 -> AdmDef+0x60+value*4, table @ 0x830EE8]. */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 11, &vl);
                Token tok[MAX_TOKENS];
                int n = tokenize(v, vl, tok, MAX_TOKENS);
                if (n >= 2) {
                    char cb[16]; size_t cbl = tok[0].len < 15 ? tok[0].len : 15;
                    to_lower_buf(cb, tok[0].s, cbl);
                    int value = -1;
                    if (cbl == 5 && memcmp(cb, "medic", 5) == 0) value = 1;
                    else if (cbl == 6 && memcmp(cb, "sniper", 6) == 0) value = 2;
                    else if (cbl == 6 && memcmp(cb, "gunner", 6) == 0) value = 3;
                    else if (cbl == 8 && memcmp(cb, "rifleman", 8) == 0) value = 5;
                    else if (cbl == 8 && memcmp(cb, "engineer", 8) == 0) value = 6;
                    if (value >= 0 && value <= 6)
                        cw.classrounds[value] = parse_int_n(tok[1].s, tok[1].len);
                }
                parsed = 1;
            } else if (lower_match_key(lower, ll, "switchcategory", 14)) {
                /* switchcategory <N> — post-recoil auto-switch target category
                   [orig: handler @ 0x5445a8 -> +0x168 flag, +0x164 category]. */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 14, &vl);
                cw.has_switchcategory = 1;
                cw.switchcategory = parse_int_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "statid", 6)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 6, &vl);
                cw.statid = parse_int_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "maxclips", 8)) {
                /* [orig: parse @0x5440A9 -> AdmDef[83]+0x14C] */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 8, &vl);
                cw.maxclips = parse_int_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "ammobucket", 10)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
                cw.ammobucket = parse_int_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "ammoclass", 9)) {
                /* ammoclass <CLASS_NAME> <pool-units-per-round> [orig: parse @0x5441CB] */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
                Token tok[MAX_TOKENS];
                int n = tokenize(v, vl, tok, MAX_TOKENS);
                if (n >= 1) safe_copy(cw.ammo_class, sizeof(cw.ammo_class), tok[0].s, tok[0].len);
                if (n >= 2) cw.ammo_class_count = parse_int_n(tok[1].s, tok[1].len);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "attachtextid", 12)) {
                /* The attach-label text key; the original resolves it against the
                   Gametext "Overlays" section at parse and stores the char* at
                   AdmDef+0x3A0 [orig: @ 0x544d6c]. We keep the key for the HUD. */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
                safe_copy(cw.attach_text_id, sizeof(cw.attach_text_id), v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "loadout_selectable", 18)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 18, &vl);
                cw.loadout_selectable = parse_int_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "loadout_subclasses", 18)) {
                /* [orig: parse @0x544E43 -> AdmDef[235]+0x3AC] */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 18, &vl);
                cw.loadout_subclasses = parse_int_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "weapon_class", 12)) {
                /* Dual representation: the raw file token, plus the loadout slot the
                   original producer routes by (0=accessory 1=primary 2=secondary
                   3=grenade) [orig: WeaponDef_ParseProperty @ 0x54d730]. */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
                Token tok[MAX_TOKENS];
                int n = tokenize(v, vl, tok, MAX_TOKENS);
                if (n >= 1) safe_copy(cw.weapon_class, sizeof(cw.weapon_class), tok[0].s, tok[0].len);
                char vb[16]; size_t vbl = vl < 15 ? vl : 15; to_lower_buf(vb, v, vbl);
                if (vbl == 9 && memcmp(vb, "accessory", 9) == 0) cw.weapon_class_slot = 0;
                else if (vbl == 7 && memcmp(vb, "primary", 7) == 0) cw.weapon_class_slot = 1;
                else if (vbl == 9 && memcmp(vb, "secondary", 9) == 0) cw.weapon_class_slot = 2;
                else if (vbl == 7 && memcmp(vb, "grenade", 7) == 0) cw.weapon_class_slot = 3;
                parsed = 1;
            } else if (lower_match_key(lower, ll, "charfilter", 10)) {
                /* Repeatable, one soldier-type token per line [orig: parse @0x543F6E].
                   Also packs the original producer's class-mask bit
                   [orig: WeaponDef_ParseProperty @ 0x54d730]. */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
                Token tok[MAX_TOKENS];
                int n = tokenize(v, vl, tok, MAX_TOKENS);
                for (int ti = 0; ti < n; ++ti) {
                    if (cw.charfilter_count >= 8) break;
                    safe_copy(cw.charfilter[cw.charfilter_count], sizeof(cw.charfilter[0]),
                              tok[ti].s, tok[ti].len);
                    ++cw.charfilter_count;
                }
                char vb[16]; size_t vbl = vl < 15 ? vl : 15; to_lower_buf(vb, v, vbl);
                if (vbl == 5 && memcmp(vb, "medic", 5) == 0) cw.charfilter_mask |= 1;
                else if (vbl == 6 && memcmp(vb, "sniper", 6) == 0) cw.charfilter_mask |= 2;
                else if (vbl == 6 && memcmp(vb, "gunner", 6) == 0) cw.charfilter_mask |= 4;
                else if (vbl == 8 && memcmp(vb, "rifleman", 8) == 0) cw.charfilter_mask |= 8;
                else if (vbl == 8 && memcmp(vb, "engineer", 8) == 0) cw.charfilter_mask |= 16;
                parsed = 1;
            } else if (lower_match_key(lower, ll, "teamfilter", 10)) {
                /* Repeatable, one team token per line [orig: parse @0x543FE3].
                   Also packs the original producer's team-mask bit
                   [orig: WeaponDef_ParseProperty @ 0x54d730]. */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
                Token tok[MAX_TOKENS];
                int n = tokenize(v, vl, tok, MAX_TOKENS);
                for (int ti = 0; ti < n; ++ti) {
                    if (cw.teamfilter_count >= 4) break;
                    safe_copy(cw.teamfilter[cw.teamfilter_count], sizeof(cw.teamfilter[0]),
                              tok[ti].s, tok[ti].len);
                    ++cw.teamfilter_count;
                }
                char vb[16]; size_t vbl = vl < 15 ? vl : 15; to_lower_buf(vb, v, vbl);
                if ((vbl == 4 && memcmp(vb, "blue", 4) == 0) || (vbl == 6 && memcmp(vb, "yellow", 6) == 0))
                    cw.teamfilter_mask |= 2;
                else if ((vbl == 3 && memcmp(vb, "red", 3) == 0) || (vbl == 6 && memcmp(vb, "violet", 6) == 0))
                    cw.teamfilter_mask |= 1;
                parsed = 1;
            } else if (lower_match_key(lower, ll, "round_type", 10)) {
                consume_value_str(trimmed, tlen, 10, cw.round_type, sizeof(cw.round_type));
                parsed = 1;
            /* PLAYER_INFO loadout tokens [orig: WeaponDef_ParseProperty @ 0x54d730]. */
            } else if (lower_match_key(lower, ll, "weaponweight", 12)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
                cw.weaponweight = parse_float_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "clipweight", 10)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
                cw.clipweight = parse_float_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "loadout_menu_textid", 19)) {
                consume_value_str(trimmed, tlen, 19, cw.loadout_menu_textid, sizeof(cw.loadout_menu_textid));
                parsed = 1;
            } else if (lower_match_key(lower, ll, "loadout_menu_ttdesc", 19)) {
                consume_value_str(trimmed, tlen, 19, cw.loadout_menu_ttdesc, sizeof(cw.loadout_menu_ttdesc));
                parsed = 1;
            } else if (lower_match_key(lower, ll, "loadout_menu_icon", 17)) {
                consume_value_str(trimmed, tlen, 17, cw.loadout_menu_icon, sizeof(cw.loadout_menu_icon));
                parsed = 1;
            } else if (lower_match_key(lower, ll, "animadm", 7)) {
                consume_value_str(trimmed, tlen, 7, cw.animadm, sizeof(cw.animadm));
                parsed = 1;
            } else if (lower_match_key(lower, ll, "launchuserpoint", 15)) {
                consume_value_str(trimmed, tlen, 15, cw.launch_user_point, sizeof(cw.launch_user_point));
                parsed = 1;
            } else if (lower_match_key(lower, ll, "crosshair", 9)) {
                consume_value_str(trimmed, tlen, 9, cw.crosshair, sizeof(cw.crosshair));
                parsed = 1;
            } else if (lower_match_key(lower, ll, "hudicon", 7)) {
                consume_value_str(trimmed, tlen, 7, cw.hudicon, sizeof(cw.hudicon));
                parsed = 1;
            } else if (lower_match_key(lower, ll, "hudclipgfx", 10)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
                Token tok[MAX_TOKENS];
                int n = tokenize(v, vl, tok, MAX_TOKENS);
                if (n >= 3) {
                    cw.hudclipgfx_offset[0] = parse_int_n(tok[0].s, tok[0].len);
                    cw.hudclipgfx_offset[1] = parse_int_n(tok[1].s, tok[1].len);
                    safe_copy(cw.hudclipgfx_texture, sizeof(cw.hudclipgfx_texture), tok[2].s, tok[2].len);
                }
                parsed = 1;
            } else if (lower_match_key(lower, ll, "hudrndgfx", 9)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
                Token tok[MAX_TOKENS];
                int n = tokenize(v, vl, tok, MAX_TOKENS);
                if (n >= 6) {
                    cw.hudrndgfx_offset[0] = parse_int_n(tok[0].s, tok[0].len);
                    cw.hudrndgfx_offset[1] = parse_int_n(tok[1].s, tok[1].len);
                    cw.hudrndgfx_layout[0] = parse_int_n(tok[2].s, tok[2].len);
                    cw.hudrndgfx_layout[1] = parse_int_n(tok[3].s, tok[3].len);
                    cw.hudrndgfx_layout[2] = parse_int_n(tok[4].s, tok[4].len);
                    safe_copy(cw.hudrndgfx_texture, sizeof(cw.hudrndgfx_texture), tok[5].s, tok[5].len);
                }
                parsed = 1;
            } else if (lower_starts_with(lower, ll, "gfx1a", 5)) {
                consume_value_str(trimmed, tlen, 5, cw.gfx1a, sizeof(cw.gfx1a));
                parsed = 1;
            } else if (lower_starts_with(lower, ll, "gfx1b", 5)) {
                consume_value_str(trimmed, tlen, 5, cw.gfx1b, sizeof(cw.gfx1b));
                parsed = 1;
            } else if (lower_starts_with(lower, ll, "gfx1", 4) && (ll == 4 || isspace((unsigned char)lower[4]))) {
                consume_value_str(trimmed, tlen, 4, cw.gfx1, sizeof(cw.gfx1));
                parsed = 1;
            } else if (lower_starts_with(lower, ll, "gfx3", 4)) {
                consume_value_str(trimmed, tlen, 4, cw.gfx3, sizeof(cw.gfx3));
                parsed = 1;
            } else if (lower_match_key(lower, ll, "flags", 5)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 5, &vl);
                char flag_lower[64];
                size_t fl = vl < sizeof(flag_lower) - 1 ? vl : sizeof(flag_lower) - 1;
                to_lower_buf(flag_lower, v, fl);
                const FlagEntry *fe = lookup_flag(flag_lower, fl);
                if (fe) {
                    cw.flags |= fe->bit;
                    cw.flags2 |= fe->bit2;
                    parsed = 1;
                }
                /* Unknown flags fall through to raw_lines */
            } else if (lower_match_key(lower, ll, "error", 5)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 5, &vl);
                parse_floats(v, vl, cw.error, 6);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "renderfov", 9)) {
                /* [orig: weapon.def parser key 'renderfov' @ 0x54482a] */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
                cw.renderfov = parse_float_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "scope_max_mag", 13)) {
                /* ADS zoom magnification; the scoped FOV = 80 / clamped zoom
                   [orig: Player_ToggleWeaponScope @ 0x4df401]. */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 13, &vl);
                cw.scope_max_mag = parse_float_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "heat_values", 11)) {
                /* percent-per-shot / percent-per-second, each through the engine's
                   digit parser then integer-divided by 100 and by 100*62 (the logic
                   rate) — the two truncating divides are load-bearing, see def.h.
                   [orig: @ 0x543eb7 -> +0x36C / +0x370] */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 11, &vl);
                Token hv[MAX_TOKENS];
                int hn = split_values(v, vl, hv, MAX_TOKENS);
                if (hn >= 1) cw.heat_per_shot = parse_fixed16_digits_n(hv[0].s, hv[0].len) / 100;
                if (hn >= 2) cw.heat_decay_per_tick = parse_fixed16_digits_n(hv[1].s, hv[1].len) / 6200;
                parsed = 1;
            } else if (lower_match_key(lower, ll, "heat_effect", 11)) {
                /* Effect name + the 16.16 glow threshold; any further values on the
                   line are unread in retail too. [orig: @ 0x543e36 -> +0x358 / +0x374] */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 11, &vl);
                Token hv[MAX_TOKENS];
                int hn = split_values(v, vl, hv, MAX_TOKENS);
                if (hn >= 1) safe_copy(cw.heat_effect, sizeof(cw.heat_effect), hv[0].s, hv[0].len);
                if (hn >= 2) cw.heat_glow_threshold = parse_fixed16_digits_n(hv[1].s, hv[1].len);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "special_hold", 12)) {
                /* 3P hold-pose kind, atol [orig: weapon.def key 'special_hold' ->
                   record+0xA4 @ 0x543cb7/0x543cd8]. */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
                cw.special_hold = parse_int_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "attack_anim", 11)) {
                /* 3P fire attack-stamp kind, atol [orig: weapon.def key 'attack_anim' ->
                   record+0xA8 @ 0x543ce9/0x543d0a]. */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 11, &vl);
                cw.attack_anim = parse_int_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "run_anim", 8)) {
                /* Run-gait class, atol [orig: weapon.def key 'run_anim' ->
                   record+0xAC @ 0x543d15/0x543d3c]. */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 8, &vl);
                cw.run_anim = parse_int_n(v, vl);
                parsed = 1;
            } else if (ll > 3 && lower_starts_with(lower, ll, "pos", 3) &&
                       (lower[3] == ' ' || lower[3] == '\t')) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 3, &vl);
                parse_floats(v, vl, cw.pos, 6);
                parsed = 1;
            } else if (ll > 4 && lower_starts_with(lower, ll, "tpos", 4) &&
                       (lower[4] == ' ' || lower[4] == '\t')) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 4, &vl);
                parse_floats(v, vl, cw.tpos, 6);
                parsed = 1;
            } else if (ll > 6 && lower_starts_with(lower, ll, "sights", 6) &&
                       (lower[6] == ' ' || lower[6] == '\t')) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 6, &vl);
                Token tok[MAX_TOKENS];
                int n = tokenize(v, vl, tok, MAX_TOKENS);
                if (n >= 5) {
                    DefSightEntry se;
                    memset(&se, 0, sizeof(se));
                    safe_copy(se.texture, sizeof(se.texture), tok[0].s, tok[0].len);
                    se.x1 = parse_int_n(tok[1].s, tok[1].len);
                    se.y1 = parse_int_n(tok[2].s, tok[2].len);
                    se.x2 = parse_int_n(tok[3].s, tok[3].len);
                    se.y2 = parse_int_n(tok[4].s, tok[4].len);
                    /* Optional flags */
                    for (int ti = 5; ti < n; ++ti) {
                        char fl[16];
                        size_t fll = tok[ti].len < 15 ? tok[ti].len : 15;
                        to_lower_buf(fl, tok[ti].s, fll);
                        if (fll == 5 && memcmp(fl, "blend", 5) == 0) se.blend = 0;
                        else if (fll == 3 && memcmp(fl, "add", 3) == 0) se.blend = 1;
                        else if (fll == 7 && memcmp(fl, "blendat", 7) == 0) se.blend = 2;
                        else if (fll == 5 && memcmp(fl, "scale", 5) == 0) se.scale = 1;
                        else if (fll == 5 && memcmp(fl, "slide", 5) == 0) {
                            se.slide = 1;
                            if (ti + 1 < n) {
                                ++ti;
                                se.slide_frames = parse_int_n(tok[ti].s, tok[ti].len);
                            }
                        }
                    }
                    DA_PUSH(cw.sights, cw.sights_count, cw_sight_cap, se);
                }
                parsed = 1;
            }

            if (!parsed) {
                DA_PUSH_RAW(cw.raw_lines, cw.raw_lines_count, cw_raw_cap, line, line_len);
            }
            continue;
        }

        if (state == ST_ACTION) {
            if (ll == 3 && memcmp(lower, "end", 3) == 0) {
                DA_PUSH(cw.actions, cw.actions_count, cw_act_cap, ca);
                memset(&ca, 0, sizeof(ca));
                ca_raw_cap = 0;
                state = ST_WEAPON;
                continue;
            }
            /* A nested `action` line while one is open is REFUSED by the
               original: it logs "forgot an end", keeps the current row open
               (subsequent keys overwrite it, last writer wins), and never
               creates the new row — suffixes that never got a row become
               zeroed generated defaults at bind time
               [orig: ActionDef_ParseScriptLine @ 0x402409 "forgot an end";
               the driver forwards in-ACTION lines before its own `action`
               dispatch, WeaponDefs_ParseLineCallback @ 0x54388d]. Falling
               through to raw_lines below reproduces exactly that. Every
               shipped weapon.def corpus (JOX, JOTAC localres, RevX02, JO:CA,
               jox01, demo) is fully END-terminated, so no retail data hits
               this path. */

            int parsed = 0;
            if (lower_starts_with(lower, ll, "anim", 4) &&
                (ll == 4 || isspace((unsigned char)lower[4]))) {
                consume_value_str(trimmed, tlen, 4, ca.anim, sizeof(ca.anim));
                parsed = 1;
            } else if (lower_match_key(lower, ll, "function", 8)) {
                consume_value_str(trimmed, tlen, 8, ca.function, sizeof(ca.function));
                parsed = 1;
            } else if (lower_match_key(lower, ll, "delaystart", 10)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
                char vlow[16];
                size_t vll = vl < 15 ? vl : 15;
                to_lower_buf(vlow, v, vll);
                ca.delaystart = (vll == 4 && memcmp(vlow, "auto", 4) == 0) ? -1 : parse_int_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "delayend", 8)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 8, &vl);
                char vlow[16];
                size_t vll = vl < 15 ? vl : 15;
                to_lower_buf(vlow, v, vll);
                ca.delayend = (vll == 4 && memcmp(vlow, "auto", 4) == 0) ? -1 : parse_int_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "delay", 5)) {
                /* bare `delay` is an alias of delayend — both write +40
                   [orig: ActionDef_ParseScriptLine @ 0x40279a / @ 0x402b2c] */
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 5, &vl);
                char vlow[16];
                size_t vll = vl < 15 ? vl : 15;
                to_lower_buf(vlow, v, vll);
                ca.delayend = (vll == 4 && memcmp(vlow, "auto", 4) == 0) ? -1 : parse_int_n(v, vl);
                parsed = 1;
            } else if (lower_starts_with(lower, ll, "soundsetend", 11)) {
                consume_value_str(trimmed, tlen, 11, ca.soundsetend, sizeof(ca.soundsetend));
                parsed = 1;
            } else if (lower_starts_with(lower, ll, "soundset", 8) &&
                       (ll == 8 || isspace((unsigned char)lower[8]))) {
                consume_value_str(trimmed, tlen, 8, ca.soundset, sizeof(ca.soundset));
                parsed = 1;
            } else if (lower_starts_with(lower, ll, "particleuserpoint", 17)) {
                consume_value_str(trimmed, tlen, 17, ca.particleuserpoint, sizeof(ca.particleuserpoint));
                parsed = 1;
            } else if (lower_starts_with(lower, ll, "particle", 8) &&
                       (ll == 8 || isspace((unsigned char)lower[8]))) {
                consume_value_str(trimmed, tlen, 8, ca.particle, sizeof(ca.particle));
                parsed = 1;
            }

            if (!parsed) {
                DA_PUSH_RAW(ca.raw_lines, ca.raw_lines_count, ca_raw_cap, line, line_len);
            }
        }
    }

    return 0;
}

DEF_EXPORT int def_parse_weapons(const char *path, DefWeaponsFile *out) {
    memset(out, 0, sizeof(*out));
    size_t file_len;
    char *buf = read_file(path, &file_len);
    if (!buf) return -1;
    int rc = parse_weapons_buf(buf, file_len, out);
    free(buf);
    return rc;
}

DEF_EXPORT int def_parse_weapons_memory(const uint8_t *data, size_t size, DefWeaponsFile *out) {
    memset(out, 0, sizeof(*out));
    if (!data) return -1;
    return parse_weapons_buf((const char *)data, size, out);
}

DEF_EXPORT void def_free_weapons(DefWeaponsFile *f) {
    if (!f) return;
    for (size_t i = 0; i < f->count; ++i) {
        for (size_t j = 0; j < f->entries[i].actions_count; ++j) {
            free(f->entries[i].actions[j].raw_lines);
        }
        free(f->entries[i].actions);
        free(f->entries[i].sights);
        free(f->entries[i].raw_lines);
    }
    free(f->entries);
    free(f->ammo_class_lines);
    memset(f, 0, sizeof(*f));
}

/* ========================================================================= */
/* Items Parsing                                                             */
/* ========================================================================= */

/* The engine's `type` token -> ItemDef+0x5C values, case-insensitive like the
   original's _stricmp chain. Non-injective by engine design: decoration and
   foliage share 2, powerup and object share 6; an unknown token leaves 0
   (unset), and 7 is unused. [orig: ItemDef_ParseProperty @ 0x49eb00;
   docs/world/itemdef-re.md D-ITEMDEF-1] */
static int item_type_from_string(const char *s, size_t len) {
    char low[16];
    size_t ll = len < 15 ? len : 15;
    to_lower_buf(low, s, ll);
    if (ll == 7 && memcmp(low, "vehicle", 7) == 0) return DEF_ITEM_TYPE_VEHICLE;
    if (ll == 10 && memcmp(low, "decoration", 10) == 0) return DEF_ITEM_TYPE_DECORATION;
    if (ll == 7 && memcmp(low, "foliage", 7) == 0) return DEF_ITEM_TYPE_FOLIAGE;
    if (ll == 6 && memcmp(low, "person", 6) == 0) return DEF_ITEM_TYPE_PERSON;
    if (ll == 6 && memcmp(low, "marker", 6) == 0) return DEF_ITEM_TYPE_MARKER;
    if (ll == 8 && memcmp(low, "building", 8) == 0) return DEF_ITEM_TYPE_BUILDING;
    if (ll == 7 && memcmp(low, "powerup", 7) == 0) return DEF_ITEM_TYPE_POWERUP;
    if (ll == 6 && memcmp(low, "object", 6) == 0) return DEF_ITEM_TYPE_OBJECT;
    if (ll == 6 && memcmp(low, "effect", 6) == 0) return DEF_ITEM_TYPE_EFFECT;
    return DEF_ITEM_TYPE_UNSET;
}

/* Anchored particle-effect slot args: <effect> <userpoint> [<secondary_effect>].
   The original copies argv[1]/argv[2] unguarded into 32-char slots and reads the
   third token only when the line carries more than 3 tokens (argc > 3, key
   included); extra tokens beyond those are ignored. `with_secondary` is 0 for
   particlefx/particlefxw3/particlefxw4, which never read a third token.
   [orig: ItemDef_ParseProperty @ 0x49eb00, particlefx chain @ 0x4a13ad..0x4a15eb] */
static void parse_item_particle_slot(const char *v, size_t vl, DefItemParticleFx *slot,
                                     int with_secondary) {
    Token tok[MAX_TOKENS];
    int n = tokenize(v, vl, tok, MAX_TOKENS);
    if (n >= 1) safe_copy(slot->effect, sizeof(slot->effect), tok[0].s, tok[0].len);
    if (n >= 2) safe_copy(slot->userpoint, sizeof(slot->userpoint), tok[1].s, tok[1].len);
    if (with_secondary && n >= 3)
        safe_copy(slot->secondary_effect, sizeof(slot->secondary_effect), tok[2].s, tok[2].len);
}

/* Shared items.def parser over an in-memory buffer. The caller owns `buf` and must have
   zeroed `out` first. Lets both the path loader and the VFS/PFF byte loader share one parser. */
static int parse_items_buf(const char *buf, size_t file_len, DefItemsFile *out) {
    size_t entries_cap = 0;
    DefItemDef current;
    memset(&current, 0, sizeof(current));
    int in_block = 0;
    size_t raw_cap = 0;
    size_t emplacement_attachments_cap = 0;

    LineIter it = {buf, file_len, 0};
    const char *line; size_t line_len;
    char lower[1024];

    while (next_line(&it, &line, &line_len)) {
        size_t tlen;
        const char *trimmed = trim_span(line, line_len, &tlen);
        if (tlen == 0) continue;

        size_t ll = tlen < sizeof(lower) - 1 ? tlen : sizeof(lower) - 1;
        to_lower_buf(lower, trimmed, ll);

        if (!in_block) {
            if (lower_starts_with(lower, ll, "begin", 5)) {
                memset(&current, 0, sizeof(current));
                raw_cap = 0;
                emplacement_attachments_cap = 0;
                extract_quoted(trimmed, tlen, current.display_name, sizeof(current.display_name));
                in_block = 1;
            }
            continue;
        }

        if (ll == 3 && memcmp(lower, "end", 3) == 0) {
            DA_PUSH(out->entries, out->count, entries_cap, current);
            memset(&current, 0, sizeof(current));
            raw_cap = 0;
            emplacement_attachments_cap = 0;
            in_block = 0;
            continue;
        }

        int parsed = 0;

        if (lower_starts_with(lower, ll, "id ", 3)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 3, &vl);
            current.id = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "sid", 3)) {
            consume_value_str(trimmed, tlen, 3, current.sid, sizeof(current.sid));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "type", 4)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 4, &vl);
            current.type = item_type_from_string(v, vl);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "graphic", 7)) {
            consume_value_str(trimmed, tlen, 7, current.graphic, sizeof(current.graphic));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "anim_def", 8)) {
            consume_value_str(trimmed, tlen, 8, current.anim_def, sizeof(current.anim_def));
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "husk ", 5)) {
            consume_value_str(trimmed, tlen, 5, current.husk, sizeof(current.husk));
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hp ", 3)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 3, &vl);
            current.hp = signed_i16_value(parse_int_n(v, vl)); /* healthMax i16 @+0x17C */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "sound_profilefemale", 19)) {
            /* The female-variant profile [orig: ItemDef_ParseProperty
               "sound_profileFemale" @ 0x49fb76 -> def+0x26C]. */
            consume_value_str(trimmed, tlen, 19, current.sound_profile_female,
                              sizeof(current.sound_profile_female));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "sound_profile", 13)) {
            /* "sound_profile" retargets the female slot too while it still
               tracks the primary (both seed to the "default" profile at alloc;
               an explicit sound_profileFemale detaches it). The original
               compares the two resolved profile POINTERS and rewrites +0x26C
               only when equal [orig: @ 0x49fb0f-0x49fb64 (the +0x26C==+0x268
               gate); alloc seed @ 0x49e3f5-0x49e408]; the name compare is the
               same rule over our unresolved names. */
            if (strcmp(current.sound_profile_female, current.sound_profile) == 0) {
                consume_value_str(trimmed, tlen, 13, current.sound_profile_female,
                                  sizeof(current.sound_profile_female));
            }
            consume_value_str(trimmed, tlen, 13, current.sound_profile, sizeof(current.sound_profile));
            parsed = 1;
        /* [orig: ItemDef_ParseProperty @ 0x49eb00 -- "soundloop_" prefix @ 0x49fec4,
           nightshot/duskshot/dawnshot @ 0x49fdee; the 7-slot range matches the
           engine's Soundloop_1..7 sound-type table @ 0x7d0788] */
        } else if (lower_starts_with(lower, ll, "soundloop_", 10) && ll > 10) {
            char idx_char = lower[10];
            if (idx_char >= '1' && idx_char <= '7') {
                int idx = idx_char - '1';
                consume_value_str(trimmed, tlen, 11, current.soundloops[idx], sizeof(current.soundloops[idx]));
                parsed = 1;
            }
        } else if (lower_match_key(lower, ll, "nightshot", 9)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
            /* Extract first token only */
            size_t end = 0;
            while (end < vl && !isspace((unsigned char)v[end])) ++end;
            safe_copy(current.nightshot, sizeof(current.nightshot), v, end);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "dawnshot", 8)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 8, &vl);
            size_t end = 0;
            while (end < vl && !isspace((unsigned char)v[end])) ++end;
            safe_copy(current.dawnshot, sizeof(current.dawnshot), v, end);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "duskshot", 8)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 8, &vl);
            size_t end = 0;
            while (end < vl && !isspace((unsigned char)v[end])) ++end;
            safe_copy(current.duskshot, sizeof(current.duskshot), v, end);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "dayshot", 7)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 7, &vl);
            size_t end = 0;
            while (end < vl && !isspace((unsigned char)v[end])) ++end;
            safe_copy(current.dayshot, sizeof(current.dayshot), v, end);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "ai_function", 11)) {
            consume_value_str(trimmed, tlen, 11, current.ai_function, sizeof(current.ai_function));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "move_function", 13)) {
            consume_value_str(trimmed, tlen, 13, current.move_function, sizeof(current.move_function));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "render_function", 15)) {
            consume_value_str(trimmed, tlen, 15, current.render_function, sizeof(current.render_function));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "disk_function", 13)) {
            consume_value_str(trimmed, tlen, 13, current.disk_function, sizeof(current.disk_function));
            parsed = 1;
        /* The person-item anim-fire weapon family (world-wac-ai-re §17.4, D-AI-5): only the
           closeattack name is kept — JO riflemen author all four ammo_* slots to the same
           rifle round. [orig: ItemDef_ParseProperty 'ammo_closeattack' @ 0x4a1823 -> def+0x56B] */
        } else if (lower_match_key(lower, ll, "ammo_closeattack", 16)) {
            consume_value_str(trimmed, tlen, 16, current.ammo_closeattack, sizeof(current.ammo_closeattack));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "primary_weapon", 14)) {
            /* The ewep emplacement's mounted weapon.def entry (the gun entity's slot-0
               weapon; the attach label's text source) [orig: -> def+0x54B primaryWeapon,
               docs/world/itemdef-re.md +0x54b] */
            consume_value_str(trimmed, tlen, 14, current.primary_weapon, sizeof(current.primary_weapon));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "addeweapg", 9) ||
                   lower_match_key(lower, ll, "addeweapc", 9) ||
                   lower_match_key(lower, ll, "addeweap", 8)) {
            /* Authored child-emplacement attachment:
                 <userpoint> <item-def id> [down up right left]
               Packed JOX has three deliberately distinct key spellings. Retain
               the variant instead of folding G/C into the ordinary record. */
            const int kind =
                    lower_match_key(lower, ll, "addeweapg", 9)
                            ? DEF_ITEM_EMPLACEMENT_ADDEWEAP_G
                    : lower_match_key(lower, ll, "addeweapc", 9)
                            ? DEF_ITEM_EMPLACEMENT_ADDEWEAP_C
                            : DEF_ITEM_EMPLACEMENT_ADDEWEAP;
            const size_t key_len =
                    kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP ? 8u : 9u;
            size_t vl;
            const char *v = consume_value_span(trimmed, tlen, key_len, &vl);
            Token tok[6];
            const int n = tokenize(v, vl, tok, 6);
            /* Retail has four fixed slots. A fifth valid record is recognized but
               silently ignored. The optional arc is all-or-none: partial tails
               remain raw diagnostics instead of inventing missing limits. */
            if (n == 2 || n >= 6) {
                parsed = 1;
                if (current.emplacement_attachments_count >= 4) {
                    continue;
                }
                DefItemEmplacementAttachment attachment;
                memset(&attachment, 0, sizeof(attachment));
                safe_copy(attachment.userpoint, sizeof(attachment.userpoint),
                          tok[0].s, tok[0].len);
                attachment.item_id = parse_int_n(tok[1].s, tok[1].len);
                attachment.kind = kind;
                attachment.angle_count = n >= 6 ? 4 : 0;
                if (n >= 6) {
                    constexpr int kBamPerDegree = 11930464;
                    attachment.down_angle =
                            parse_int_n(tok[2].s, tok[2].len) * kBamPerDegree;
                    attachment.up_angle =
                            -parse_int_n(tok[3].s, tok[3].len) * kBamPerDegree;
                    attachment.right_angle =
                            parse_int_n(tok[4].s, tok[4].len) * kBamPerDegree;
                    attachment.left_angle =
                            -parse_int_n(tok[5].s, tok[5].len) * kBamPerDegree;
                }
                DA_PUSH(current.emplacement_attachments,
                        current.emplacement_attachments_count,
                        emplacement_attachments_cap, attachment);
                const int stored_slot =
                        static_cast<int>(current.emplacement_attachments_count);
                if (kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP_G)
                    current.emplacement_g_slot = stored_slot;
                else if (kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP_C)
                    current.emplacement_c_slot = stored_slot;
            }
        } else if (lower_match_key(lower, ll, "phrase_set", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            /* Plain signed atol -> target itemDef+0x86C. Presence cannot be
               represented by the zero-initialized value because config 0 is real.
               [orig: @ 0x49F9DB..0x49FA0A] */
            current.phrase_set = parse_int_n(v, vl);
            current.phrase_set_valid = 1;
            parsed = 1;
        } else if (lower_match_key(lower, ll, "clipsize", 8)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 8, &vl);
            /* plain atol -> def+0x894, the entity+0x35C magazine reseed source
               [orig: @ 0x49fa2e-0x49fa48; Entity_ResetToSpawnState @ 0x4b97a9] */
            current.clipsize = parse_int_n(v, vl);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "deathtime", 9)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
            /* seconds -> ticks at parse: 62*v, an explicit 0 -> 496, +62 grace; the
               corpse timer's seed (entity+0x148 at the death edge @ 0x4b9c97)
               [orig: ItemDef_ParseProperty @ 0x49fa6c-0x49faa0 -> def+0x890] */
            int dt = parse_int_n(v, vl) * 62;
            if (dt == 0) dt = 496;
            current.deathtime_ticks = dt + 62;
            parsed = 1;
        /* Vehicle physics-property block, scaled at parse exactly like the original loader
           [orig: ItemDef_ParsePhysicsProperty @0x49d870]. turn_rate2 is matched before
           turn_rate only for clarity — lower_match_key requires a separator after the key. */
        } else if (lower_match_key(lower, ll, "turn_rate2", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            current.turn_rate2 = parse_int_n(v, vl) * 192426; /* deg/s -> BAM/tick [orig: @0x49d8dc] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "turn_rate", 9)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
            current.turn_rate = parse_int_n(v, vl) * 192426; /* deg/s -> BAM/tick [orig: @0x49d89a] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "torque", 6)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 6, &vl);
            current.torque = parse_int_n(v, vl); /* raw shift count [orig: @0x49dcca] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "max_slope", 9)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
            current.max_slope = parse_int_n(v, vl) * 11930464; /* deg -> BAM [orig: @0x49d91e] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "slip_slope", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            current.slip_slope = parse_int_n(v, vl) * 11930464; /* deg -> BAM [orig: @0x49d960] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "player_speed", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            current.player_speed = parse_int_n(v, vl) * 293; /* km/h -> 16.16 u/tick [orig: @0x49d9a2] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "water_speed", 11)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 11, &vl);
            current.water_speed = parse_int_n(v, vl) * 293; /* km/h -> 16.16 u/tick [orig: @0x49d9e4] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "acceleration", 12)) {
            /* token*4; a still-unset deceleration defaults to 2*acceleration (8*token) at
               THIS parse site, mirroring the original's ordering semantics [orig: @0x49da32,
               decel default @0x49da4b]. */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            int a4 = parse_int_n(v, vl) * 4;
            current.acceleration = a4;
            if (current.deceleration == 0) current.deceleration = a4 * 2;
            parsed = 1;
        } else if (lower_match_key(lower, ll, "deceleration", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            current.deceleration = parse_int_n(v, vl) * 4; /* [orig: @0x49da84] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "slip_speed", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            current.slip_speed = parse_int_n(v, vl) * 4; /* [orig: @0x49dafd] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "physics", 7)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 7, &vl);
            current.physics = parse_int_n(v, vl); /* raw selector [orig: @0x49dac8] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "criticalhp", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            current.critical_hp = parse_int_n(v, vl); /* i16 raw at +0x180 */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "criticaldrain", 13)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 13, &vl);
            current.critical_drain = parse_int_n(v, vl); /* i16 raw at +0x182 */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "damage_reduc_pp", 15)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 15, &vl);
            Token tok[2];
            const int count = split_values(v, vl, tok, 2);
            if (count >= 1) {
                current.damage_reduc_pp = parse_float_n(tok[0].s, tok[0].len);
                current.damage_reduc_max = 1.0f - current.damage_reduc_pp;
            }
            if (count >= 2)
                current.damage_reduc_max = parse_float_n(tok[1].s, tok[1].len);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "armor", 5)) {
            /* 'armor A [B]': +0x190 impact = A then overwritten by B;
               +0x192 blast = A. The historical armor_kz API name is an alias
               for blast armor, not a separate parsed field. Both retail words
               are signed i16 (-1 is the 0xFFFF invulnerable value).
               [orig: @ 0x4a00e7-0x4a0147] */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 5, &vl);
            Token tok[2];
            const int count = split_values(v, vl, tok, 2);
            if (count >= 1) {
                const int armor = signed_i16_value(parse_int_n(tok[0].s, tok[0].len));
                current.armor_impact = armor;
                current.armor_blast = armor;
                current.armor_kz = armor;
            }
            if (count >= 2)
                current.armor_impact =
                    signed_i16_value(parse_int_n(tok[1].s, tok[1].len));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "unit_type", 9)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 9, &vl);
            current.unit_type = parse_int_n(v, vl); /* minimap icon class [orig: @0x50FA70]
                                                       + the death-dispatch row key
                                                       [orig: Entity_DispatchDeathCallback
                                                       @0x493f23 vs table @0x815410] */
            parsed = 1;
        /* --- the destruction/husk block [orig: ItemDef_ParseProperty @ 0x49eb00] --- */
        } else if (lower_match_key(lower, ll, "huskfinal", 9)) {
            consume_value_str(trimmed, tlen, 9, current.huskfinal, sizeof(current.huskfinal));
            parsed = 1;
        } else if (lower_match_key(lower, ll, "sounddeath", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            size_t end = 0;
            while (end < vl && !isspace((unsigned char)v[end])) ++end;
            safe_copy(current.sounddeath, sizeof(current.sounddeath), v, end);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "kz", 2)) {
            /* Death-blast radius in units, plain float -> def+0x198. */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 2, &vl);
            current.kz = (float)parse_float_n(v, vl);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "husk_swap_at_sec", 16)) {
            /* seconds*62 ticks; an authored 0 stores 1.0. [orig: @ 0x49f1ce-0x49f228] */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 16, &vl);
            float sec = (float)(parse_float_n(v, vl) * 62.0);
            current.husk_swap_at_sec = (sec == 0.0f) ? 1.0f : sec;
            parsed = 1;
        } else if (lower_match_key(lower, ll, "husk_swap_at", 12)) {
            /* Dual-unit: while +0x1A0 is still 0 the value is a PERCENT (atol*0.01),
               else seconds*62 — the witnessed parse-order dependence.
               [orig: @ 0x49f242-0x49f2c2] */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            if (current.husk_swap_at_sec == 0.0f)
                current.husk_swap_at = (float)(parse_int_n(v, vl) * 0.01);
            else
                current.husk_swap_at = (float)(parse_float_n(v, vl) * 62.0);
            parsed = 1;
        } else if (lower_match_key(lower, ll, "debris_scale", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            current.debris_scale = (float)parse_float_n(v, vl); /* -> def+0x1BC */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "husk_sub_parts", 14)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 14, &vl);
            current.husk_sub_parts = parse_int_n(v, vl); /* -> +0x100 count byte */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "husk_sub_part_types", 19)) {
            /* Each value 'NN_NAME': split at the FIRST '_', slot = NN-1 (0..15), the
               remainder (internal underscores kept) resolved case-insensitively against
               the engine debris-type table names; at most 16 values processed.
               [orig: @ 0x49f314-0x49f396; DeathPieceType_FindByName @ 0x57b310] */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 19, &vl);
            Token tok[MAX_TOKENS];
            int ntok = split_values(v, vl, tok, MAX_TOKENS);
            int processed = 0;
            for (int ti = 0; ti < ntok && processed < 16; ++ti) {
                const char *us = NULL;
                for (size_t k = 0; k < tok[ti].len; ++k) {
                    if (tok[ti].s[k] == '_') { us = tok[ti].s + k; break; }
                }
                if (us == NULL) continue;
                int slot = parse_int_n(tok[ti].s, (size_t)(us - tok[ti].s)) - 1;
                if (slot < 0 || slot > 15) continue;
                ++processed;
                const char *nm = us + 1;
                size_t nl = tok[ti].len - (size_t)(nm - tok[ti].s);
                current.husk_sub_part_types[slot] =
                        (unsigned char)death_piece_type_index(nm, nl);
            }
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "attrib:", 7)) {
            /* Space-separated capability tokens -> ItemDefAttrib/Attrib2 bits. Unknown tokens
               (exp1, pilotonly, forceasset, neutral, ...) are not in the witnessed map and stay
               unmapped. [orig: ItemDef_ParseProperty @0x49eb00; docs/world/itemdef-re.md] */
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 7, &vl);
            Token tok[MAX_TOKENS];
            int ntok = split_values(v, vl, tok, MAX_TOKENS);
            for (int ti = 0; ti < ntok; ++ti) {
                char lo[32];
                size_t k = tok[ti].len < sizeof(lo) - 1 ? tok[ti].len : sizeof(lo) - 1;
                to_lower_buf(lo, tok[ti].s, k);
                int b = lookup_item_attrib(lo, k);
                if (b) { current.attrib |= (unsigned)b; }
                else { int b2 = lookup_item_attrib2(lo, k); if (b2) current.attrib2 |= (unsigned)b2; }
            }
            parsed = 1;
        /* Per-item particle-effect keys, matched in the original's chain order
           (lower_match_key requires a separator after the key, so the shared
           'particlefx' prefix cannot shadow the longer keys). All names are
           copied verbatim as strings [orig: ItemDef_ParseProperty @ 0x49eb00]. */
        } else if (lower_match_key(lower, ll, "particlefx", 10)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
            parse_item_particle_slot(v, vl, &current.particlefx, 0); /* +0x278/+0x298 [orig: @ 0x4a13ad] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlefxs", 11)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 11, &vl);
            parse_item_particle_slot(v, vl, &current.particlefxs, 1); /* +0x2AE/+0x2EE, 2nd +0x2CE [orig: @ 0x4a140b] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlefxw1", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            parse_item_particle_slot(v, vl, &current.particlefxw1, 1); /* +0x304/+0x344, 2nd +0x324 [orig: @ 0x4a148b] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlefxw2", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            parse_item_particle_slot(v, vl, &current.particlefxw2, 1); /* +0x35A/+0x39A, 2nd +0x37A [orig: @ 0x4a150b] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlefxw3", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            parse_item_particle_slot(v, vl, &current.particlefxw3, 0); /* +0x3AE/+0x3CE, NO secondary [orig: @ 0x4a158b] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlefxw4", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            parse_item_particle_slot(v, vl, &current.particlefxw4, 0); /* +0x3E2/+0x402, NO secondary [orig: @ 0x4a15eb] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particledeath", 13)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 13, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particledeath, sizeof(current.particledeath), tok[0].s,
                          tok[0].len); /* +0x416 [orig: @ 0x4a164b] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particleh2odeath", 16)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 16, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particleh2odeath, sizeof(current.particleh2odeath), tok[0].s,
                          tok[0].len); /* +0x44A [orig: @ 0x4a168e] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlefire", 12)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 12, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particlefire, sizeof(current.particlefire), tok[0].s,
                          tok[0].len); /* +0x47E [orig: @ 0x4a16d0] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particleother", 13)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 13, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particleother, sizeof(current.particleother), tok[0].s,
                          tok[0].len); /* +0x4B2 [orig: @ 0x4a1713] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlefinale", 14)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 14, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particlefinale, sizeof(current.particlefinale), tok[0].s,
                          tok[0].len); /* +0x4E4 [orig: @ 0x4a175b] */
            parsed = 1;
        } else if (lower_match_key(lower, ll, "particlespawn", 13)) {
            size_t vl; const char *v = consume_value_span(trimmed, tlen, 13, &vl);
            Token tok[1];
            if (tokenize(v, vl, tok, 1) >= 1)
                safe_copy(current.particlespawn, sizeof(current.particlespawn), tok[0].s,
                          tok[0].len); /* +0x506 [orig: @ 0x4a179d] */
            parsed = 1;
        }

        if (!parsed) {
            DA_PUSH_RAW(current.raw_lines, current.raw_lines_count, raw_cap, line, line_len);
        }
    }

    return 0;
}

DEF_EXPORT int def_parse_items(const char *path, DefItemsFile *out) {
    memset(out, 0, sizeof(*out));
    size_t file_len;
    char *buf = read_file(path, &file_len);
    if (!buf) return -1;
    int rc = parse_items_buf(buf, file_len, out);
    free(buf);
    return rc;
}

DEF_EXPORT int def_parse_items_memory(const uint8_t *data, size_t size, DefItemsFile *out) {
    memset(out, 0, sizeof(*out));
    if (!data) return -1;
    return parse_items_buf((const char *)data, size, out);
}

DEF_EXPORT void def_free_items(DefItemsFile *f) {
    if (!f) return;
    for (size_t i = 0; i < f->count; ++i) {
        free(f->entries[i].emplacement_attachments);
        free(f->entries[i].raw_lines);
    }
    free(f->entries);
    memset(f, 0, sizeof(*f));
}

/* ========================================================================= */
/* HudPos Parsing                                                            */
/* ========================================================================= */

/* Shared buffer parser for hudpos.def, used by both the path and memory entry
   points (mirrors parse_items_buf). Assumes `out` was zeroed by the caller. */
static int parse_hudpos_buf(const char *buf, size_t file_len, DefHudPosFile *out) {
    DefHudPosDef *hud = &out->hud;
    /* Set default alpha for all colors */
    hud->health_border.a = 255;
    hud->heat_border.a = 255;
    hud->hud_textcolor.a = 255;
    hud->weapon_textcolor.a = 255;
    hud->tagcolor_blueteam.a = 255;
    hud->tagcolor_redteam.a = 255;
    hud->tagcolor_good.a = 255;
    hud->tagcolor_middle.a = 255;
    hud->tagcolor_bad.a = 255;
    hud->stanceicon_color.a = 255;
    hud->stancecolor_good.a = 255;
    hud->stancecolor_middle.a = 255;
    hud->stancecolor_bad.a = 255;
    hud->dest_agl_color.a = 255;
    hud->agl_color.a = 255;

    int in_vehicle_block = 0;
    size_t raw_cap = 0, stance_cap = 0, declut_cap = 0, sf_cap = 0;

    LineIter it = {buf, file_len, 0};
    const char *line; size_t line_len;
    char lower[1024];

    while (next_line(&it, &line, &line_len)) {
        size_t tlen;
        const char *trimmed = trim_span(line, line_len, &tlen);
        if (tlen == 0) continue;

        /* Skip full-line comments */
        if (tlen >= 2 && trimmed[0] == '/' && trimmed[1] == '/') continue;

        size_t ll = tlen < sizeof(lower) - 1 ? tlen : sizeof(lower) - 1;
        to_lower_buf(lower, trimmed, ll);

        /* Vehicle HUD blocks */
        if (lower_starts_with(lower, ll, "vehicle_hud", 11)) {
            in_vehicle_block = 1;
            DA_PUSH_RAW(hud->raw_lines, hud->raw_lines_count, raw_cap, line, line_len);
            continue;
        }
        if (lower_starts_with(lower, ll, "vehicle_end", 11)) {
            in_vehicle_block = 0;
            DA_PUSH_RAW(hud->raw_lines, hud->raw_lines_count, raw_cap, line, line_len);
            continue;
        }
        if (in_vehicle_block) {
            DA_PUSH_RAW(hud->raw_lines, hud->raw_lines_count, raw_cap, line, line_len);
            continue;
        }

        /* Get value part */
        size_t space_pos = 0;
        while (space_pos < tlen && !isspace((unsigned char)trimmed[space_pos])) ++space_pos;
        const char *val_part = trimmed + space_pos;
        size_t val_len = tlen - space_pos;
        /* Trim val_part */
        size_t vl;
        val_part = trim_span(val_part, val_len, &vl);
        /* Strip comment from value */
        for (size_t ci = 0; ci + 1 < vl; ++ci) {
            if (val_part[ci] == '/' && val_part[ci + 1] == '/') {
                vl = ci;
                while (vl > 0 && isspace((unsigned char)val_part[vl - 1])) --vl;
                break;
            }
        }

        Token vals[MAX_TOKENS];
        int nvals = split_values(val_part, vl, vals, MAX_TOKENS);

        int parsed = 0;

        /* Fonts */
        if (lower_starts_with(lower, ll, "fonthud1_hi", 11)) {
            if (nvals >= 1) safe_copy(hud->font_hi, sizeof(hud->font_hi), vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "fonthud1_lo", 11)) {
            if (nvals >= 1) safe_copy(hud->font_lo, sizeof(hud->font_lo), vals[0].s, vals[0].len);
            parsed = 1;
        }
        /* Rects */
        else if (lower_starts_with(lower, ll, "mrclippynormal", 14)) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->mrclippy_normal[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "mrclippyalternate", 17)) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->mrclippy_alternate[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudhealth", 9) && !lower_starts_with(lower, ll, "hudhealthborder", 15)) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->health[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudheat", 7) && !lower_starts_with(lower, ll, "hudheatborder", 13)) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->heat[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudpowerbar", 11)) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->powerbar[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "starttimer", 10)) {
            for (int i = 0; i < 4 && i < nvals; ++i)
                hud->starttimer[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        }
        /* Border colors */
        else if (lower_starts_with(lower, ll, "hudhealthborder", 15)) {
            hud->health_border = parse_hud_color(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudheatborder", 13)) {
            hud->heat_border = parse_hud_color(vals, nvals);
            parsed = 1;
        }
        /* Text colors */
        else if (lower_starts_with(lower, ll, "hud_textcolor", 13)) {
            hud->hud_textcolor = parse_hud_color(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "weapon_textcolor", 16)) {
            hud->weapon_textcolor = parse_hud_color(vals, nvals);
            parsed = 1;
        }
        /* Tag colors */
        else if (lower_starts_with(lower, ll, "tagcolor_blueteam", 17)) {
            hud->tagcolor_blueteam = parse_hud_color(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "tagcolor_redteam", 16)) {
            hud->tagcolor_redteam = parse_hud_color(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "tagcolor_good", 13)) {
            hud->tagcolor_good = parse_hud_color(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "tagcolor_middle", 15)) {
            hud->tagcolor_middle = parse_hud_color(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "tagcolor_bad", 12)) {
            hud->tagcolor_bad = parse_hud_color(vals, nvals);
            parsed = 1;
        }
        /* Stance colors (ARGB) */
        else if (lower_starts_with(lower, ll, "stanceicon_color", 16)) {
            hud->stanceicon_color = parse_hud_color_argb(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "stancecolor_good", 16)) {
            hud->stancecolor_good = parse_hud_color_argb(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "stancecolor_middle", 18)) {
            hud->stancecolor_middle = parse_hud_color_argb(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "stancecolor_bad", 15)) {
            hud->stancecolor_bad = parse_hud_color_argb(vals, nvals);
            parsed = 1;
        }
        /* AGL colors */
        else if (lower_starts_with(lower, ll, "destaglcolor", 12)) {
            hud->dest_agl_color = parse_hud_color_argb(vals, nvals);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "aglcolor", 8)) {
            hud->agl_color = parse_hud_color_argb(vals, nvals);
            parsed = 1;
        }
        /* Spinmap */
        else if (lower_starts_with(lower, ll, "hudspinmapx1", 12)) {
            if (nvals >= 1) hud->spinmap_x1 = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudspinmapx2", 12)) {
            if (nvals >= 1) hud->spinmap_x2 = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudspinmapy1", 12)) {
            if (nvals >= 1) hud->spinmap_y1 = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudspinmapy2", 12)) {
            if (nvals >= 1) hud->spinmap_y2 = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        }
        /* Positioned text with alignment */
        else if (lower_starts_with(lower, ll, "hudflagcarrier", 14)) {
            parse_pos_aligned(vals, nvals, hud->flag_carrier);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "gameinfo", 8)) {
            parse_pos_aligned(vals, nvals, hud->game_info);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudwpdinfo", 10)) {
            parse_pos_aligned(vals, nvals, hud->wpd_info);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "zoneinfo", 8)) {
            parse_pos_aligned(vals, nvals, hud->zone_info);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "exppoints", 9)) {
            parse_pos_aligned(vals, nvals, hud->exp_points);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "connectstatus", 13)) {
            parse_pos_aligned(vals, nvals, hud->connect_status);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudteamxy", 9)) {
            parse_pos_aligned(vals, nvals, hud->team_xy);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudplayercount", 14)) {
            parse_pos_aligned(vals, nvals, hud->player_count);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "ammocountpos", 12)) {
            parse_pos_aligned(vals, nvals, hud->ammo_count_pos);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudweaponname", 13)) {
            parse_pos_aligned(vals, nvals, hud->weapon_name_pos);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "mapcoords", 9)) {
            parse_pos_aligned(vals, nvals, hud->map_coords);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudtimeclock", 12)) {
            parse_pos_aligned(vals, nvals, hud->time_clock);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "breathtime", 10)) {
            parse_pos_aligned(vals, nvals, hud->breath_time);
            parsed = 1;
        }
        /* XY positions */
        else if (lower_starts_with(lower, ll, "hudtitlex", 9)) {
            if (nvals >= 1) hud->title_x = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudtitley", 9)) {
            if (nvals >= 1) hud->title_y = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudpingx", 8)) {
            if (nvals >= 1) hud->ping_x = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudpingy", 8)) {
            if (nvals >= 1) hud->ping_y = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudpingright", 12)) {
            if (nvals >= 1) hud->ping_right = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudorders", 9)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->orders[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "specmode_label", 14)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->spec_mode_label[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "lfp_flags", 9)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->lfp_flags[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "lfp_takeoverdlg", 15)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->lfp_takeover_dlg[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "cargopos", 8)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->cargo_pos[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "roomtkpos", 9) && !lower_starts_with(lower, ll, "roomtktxtpos", 12)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->roomtk_pos[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "roomtktxtpos", 12)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->roomtk_txt_pos[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudstancepos", 12)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->stance_pos[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudvehstancepos", 15)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->veh_stance_pos[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudgeartext", 11)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->gear_text[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudwpnicon", 10)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->wpn_icon[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudclip", 7) && !lower_starts_with(lower, ll, "hudclipgfx", 10)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->clip_pos[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudscoperangexy", 15)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->scope_range[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudscopezeroxy", 14)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->scope_zero[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudscopemagxy", 13)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->scope_mag[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "showimpactdistpos", 17)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->impact_dist_pos[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudchattext", 11)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->chat_text[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudsystext", 10)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->sys_text[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        }
        /* Single values */
        else if (lower_starts_with(lower, ll, "hudchline", 9)) {
            if (nvals >= 1) hud->hud_chline = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudaglradius", 12)) {
            if (nvals >= 1) hud->agl_radius = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudroclen", 9)) {
            if (nvals >= 1) hud->roc_len = parse_int_n(vals[0].s, vals[0].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "alphafade", 9)) {
            /* atof per field — fractional values survive into the original's
               x2.55/x62 converts [orig: @0x5a0882..0x5a08c2]. */
            for (int i = 0; i < 3 && i < nvals; ++i)
                hud->alpha_fade[i] = parse_float_n(vals[i].s, vals[i].len);
            parsed = 1;
        }
        /* AGL settings */
        else if (lower_starts_with(lower, ll, "hudagltlrx", 10)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->agl_tlrx[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "hudaglylen", 10)) {
            for (int i = 0; i < 2 && i < nvals; ++i)
                hud->agl_ylen[i] = parse_int_n(vals[i].s, vals[i].len);
            parsed = 1;
        }
        /* HUDSTANCE */
        else if (lower_starts_with(lower, ll, "hudstance", 9) && ll > 9 &&
                 isspace((unsigned char)lower[9])) {
            if (nvals >= 5) {
                DefHudStance st;
                memset(&st, 0, sizeof(st));
                st.id = parse_int_n(vals[0].s, vals[0].len);
                st.offset_x = parse_int_n(vals[1].s, vals[1].len);
                st.offset_y = parse_int_n(vals[2].s, vals[2].len);
                safe_copy(st.texture, sizeof(st.texture), vals[3].s, vals[3].len);
                safe_copy(st.name, sizeof(st.name), vals[4].s, vals[4].len);
                DA_PUSH(hud->stances, hud->stances_count, stance_cap, st);
            }
            parsed = 1;
        }
        /* HUDDECLUT_ */
        else if (lower_starts_with(lower, ll, "huddeclut_", 10)) {
            /* Extract name between _ and first whitespace */
            size_t name_start = 10;
            size_t name_end = name_start;
            while (name_end < ll && !isspace((unsigned char)lower[name_end])) ++name_end;
            if (nvals >= 4) {
                DefDeclutterEntry de;
                memset(&de, 0, sizeof(de));
                safe_copy(de.name, sizeof(de.name), trimmed + name_start, name_end - name_start);
                for (int i = 0; i < 4; ++i)
                    de.flags[i] = parse_int_n(vals[i].s, vals[i].len) != 0 ? 1 : 0;
                DA_PUSH(hud->declutter, hud->declutter_count, declut_cap, de);
            }
            parsed = 1;
        }
        /* Graphics */
        else if (lower_starts_with(lower, ll, "staticframe", 11)) {
            if (nvals >= 1) {
                DefHudGraphic gfx;
                memset(&gfx, 0, sizeof(gfx));
                int idx = 0;
                /* Skip leading // in texture name */
                if (vals[idx].len > 2 && vals[idx].s[0] == '/' && vals[idx].s[1] == '/') {
                    safe_copy(gfx.texture, sizeof(gfx.texture), vals[idx].s + 2, vals[idx].len - 2);
                    idx++;
                } else if (!(vals[idx].len == 2 && vals[idx].s[0] == '/' && vals[idx].s[1] == '/')) {
                    safe_copy(gfx.texture, sizeof(gfx.texture), vals[idx].s, vals[idx].len);
                    idx++;
                }
                if (idx < nvals) gfx.x = parse_int_n(vals[idx].s, vals[idx].len), idx++;
                if (idx < nvals) gfx.y = parse_int_n(vals[idx].s, vals[idx].len), idx++;
                if (gfx.texture[0] != '\0') {
                    DA_PUSH(hud->static_frames, hud->static_frames_count, sf_cap, gfx);
                }
            }
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "parachuteicon", 13)) {
            if (nvals >= 3) {
                safe_copy(hud->parachute_icon.texture, sizeof(hud->parachute_icon.texture), vals[0].s, vals[0].len);
                hud->parachute_icon.x = parse_int_n(vals[1].s, vals[1].len);
                hud->parachute_icon.y = parse_int_n(vals[2].s, vals[2].len);
            }
            parsed = 1;
        } else if (lower_starts_with(lower, ll, "armoricon", 9)) {
            if (nvals >= 3) {
                safe_copy(hud->armor_icon.texture, sizeof(hud->armor_icon.texture), vals[0].s, vals[0].len);
                hud->armor_icon.x = parse_int_n(vals[1].s, vals[1].len);
                hud->armor_icon.y = parse_int_n(vals[2].s, vals[2].len);
            }
            parsed = 1;
        }

        if (!parsed) {
            DA_PUSH_RAW(hud->raw_lines, hud->raw_lines_count, raw_cap, line, line_len);
        }
    }

    return 0;
}

DEF_EXPORT int def_parse_hudpos(const char *path, DefHudPosFile *out) {
    memset(out, 0, sizeof(*out));
    size_t file_len;
    char *buf = read_file(path, &file_len);
    if (!buf) return -1;
    int rc = parse_hudpos_buf(buf, file_len, out);
    free(buf);
    return rc;
}

DEF_EXPORT int def_parse_hudpos_memory(const uint8_t *data, size_t size, DefHudPosFile *out) {
    memset(out, 0, sizeof(*out));
    if (!data) return -1;
    return parse_hudpos_buf((const char *)data, size, out);
}

DEF_EXPORT void def_free_hudpos(DefHudPosFile *f) {
    if (!f) return;
    free(f->hud.stances);
    free(f->hud.declutter);
    free(f->hud.static_frames);
    free(f->hud.raw_lines);
    memset(f, 0, sizeof(*f));
}

/* ========================================================================= */
/* Legacy Single-Entry Parsing                                               */
/* ========================================================================= */

DEF_EXPORT int def_parse_def(const char *path, DefFile *out) {
    memset(out, 0, sizeof(*out));

    size_t file_len;
    char *buf = read_file(path, &file_len);
    if (!buf) return -1;

    enum { ST_UNKNOWN, ST_WEAPON, ST_GENERIC, ST_ACTION };
    int state = ST_UNKNOWN;
    DefAction ca; memset(&ca, 0, sizeof(ca));
    size_t act_cap = 0;

    LineIter it = {buf, file_len, 0};
    const char *line; size_t line_len;
    char lower[1024];

    while (next_line(&it, &line, &line_len)) {
        size_t tlen;
        const char *trimmed = trim_span(line, line_len, &tlen);
        if (tlen == 0) continue;

        size_t ll = tlen < sizeof(lower) - 1 ? tlen : sizeof(lower) - 1;
        to_lower_buf(lower, trimmed, ll);

        if (state == ST_UNKNOWN) {
            if (lower_starts_with(lower, ll, "weapon", 6)) {
                out->kind = DEF_KIND_WEAPON;
                extract_quoted(trimmed, tlen, out->weapon.weapon_name, sizeof(out->weapon.weapon_name));
                state = ST_WEAPON;
                continue;
            }
            if (lower_starts_with(lower, ll, "begin", 5)) {
                out->kind = DEF_KIND_GENERIC;
                extract_quoted(trimmed, tlen, out->generic.display_name, sizeof(out->generic.display_name));
                state = ST_GENERIC;
                continue;
            }
            continue;
        }

        if (state == ST_WEAPON) {
            if (lower_starts_with(lower, ll, "action", 6)) {
                memset(&ca, 0, sizeof(ca));
                extract_quoted(trimmed, tlen, ca.name, sizeof(ca.name));
                state = ST_ACTION;
                continue;
            }
            if (ll == 3 && memcmp(lower, "end", 3) == 0) break;

            if (lower_starts_with(lower, ll, "animadm", 7)) {
                consume_value_str(trimmed, tlen, 7, out->weapon.animadm, sizeof(out->weapon.animadm));
            } else if (lower_starts_with(lower, ll, "launchuserpoint", 15)) {
                consume_value_str(trimmed, tlen, 15, out->weapon.launch_user_point, sizeof(out->weapon.launch_user_point));
            } else if (lower_starts_with(lower, ll, "gfx1a", 5)) {
                consume_value_str(trimmed, tlen, 5, out->weapon.gfx1a, sizeof(out->weapon.gfx1a));
            } else if (lower_starts_with(lower, ll, "gfx1b", 5)) {
                consume_value_str(trimmed, tlen, 5, out->weapon.gfx1b, sizeof(out->weapon.gfx1b));
            } else if (lower_starts_with(lower, ll, "gfx1", 4)) {
                consume_value_str(trimmed, tlen, 4, out->weapon.gfx1, sizeof(out->weapon.gfx1));
            } else if (lower_starts_with(lower, ll, "gfx3", 4)) {
                consume_value_str(trimmed, tlen, 4, out->weapon.gfx3, sizeof(out->weapon.gfx3));
            }
            continue;
        }

        if (state == ST_ACTION) {
            /* A nested `action` line is REFUSED by the original ("forgot an
               end"): the open row stays current and the new row is never
               created — ignoring the line here reproduces that
               [orig: ActionDef_ParseScriptLine @ 0x402409]. */
            if (ll == 3 && memcmp(lower, "end", 3) == 0) {
                DA_PUSH(out->weapon.actions, out->weapon.actions_count, act_cap, ca);
                memset(&ca, 0, sizeof(ca));
                state = ST_WEAPON;
                continue;
            }
            if (lower_starts_with(lower, ll, "anim", 4)) {
                consume_value_str(trimmed, tlen, 4, ca.anim, sizeof(ca.anim));
            }
            continue;
        }

        if (state == ST_GENERIC) {
            if (ll == 3 && memcmp(lower, "end", 3) == 0) break;

            if (lower_starts_with(lower, ll, "type", 4)) {
                consume_value_str(trimmed, tlen, 4, out->generic.type, sizeof(out->generic.type));
            } else if (lower_starts_with(lower, ll, "graphic", 7)) {
                consume_value_str(trimmed, tlen, 7, out->generic.graphic, sizeof(out->generic.graphic));
            } else if (lower_starts_with(lower, ll, "husk", 4)) {
                consume_value_str(trimmed, tlen, 4, out->generic.husk, sizeof(out->generic.husk));
            } else if (lower_starts_with(lower, ll, "anim_def", 8)) {
                consume_value_str(trimmed, tlen, 8, out->generic.anim_def, sizeof(out->generic.anim_def));
            }
            continue;
        }
    }

    free(buf);

    if (out->kind == DEF_KIND_WEAPON && out->weapon.weapon_name[0] == '\0') return -1;
    if (out->kind == DEF_KIND_GENERIC && out->generic.display_name[0] == '\0') return -1;

    return 0;
}

DEF_EXPORT void def_free_def(DefFile *f) {
    if (!f) return;
    free(f->weapon.actions);
    memset(f, 0, sizeof(*f));
}

DEF_EXPORT double def_loadout_weight(const DefWeaponDef *weapons, const int *ammo_counts, size_t n) {
    /* [orig: calculate_loadout_weight @ 0x55f1f0] per weapon:
       weaponweight + (ammo_count > 0 ? ammo_count : maxclips) * clipweight. */
    if (!weapons) return 0.0;
    double total = 0.0;
    for (size_t i = 0; i < n; ++i) {
        total += (double)weapons[i].weaponweight;
        const int ammo = (ammo_counts && ammo_counts[i] > 0) ? ammo_counts[i] : weapons[i].maxclips;
        total += (double)ammo * (double)weapons[i].clipweight;
    }
    return total;
}

DEF_EXPORT DefEncumbrance def_encumbrance_class(double weight) {
    /* [orig: update_player_info_weight_and_weapon_icons @ 0x55f480] the exact
       witnessed thresholds: >= 66.6 HEAVY, >= 33.3 NORMAL, else LIGHT. */
    if (weight >= 66.6) return DEF_ENCUMBRANCE_HEAVY;
    if (weight >= 33.3) return DEF_ENCUMBRANCE_NORMAL;
    return DEF_ENCUMBRANCE_LIGHT;
}
