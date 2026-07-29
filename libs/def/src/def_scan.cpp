#include "def_scan.h"

// Split out of def.cpp (quality campaign W3-3). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// The shared .def text scanner. Its lookup tables (flags, item attributes, death
// pieces) stay private here; only what a family parser calls is declared.

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace defscan {

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

void to_lower_buf(char *dst, const char *src, size_t len) {
    for (size_t i = 0; i < len; ++i)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[len] = '\0';
}

/* Extract quoted string from line: first "..." pair */
size_t extract_quoted(const char *line, size_t line_len, char *dst, size_t dst_size) {
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

int parse_int_n(const char *s, size_t len) {
    char buf[32];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, s, len);
    buf[len] = '\0';
    return (int)strtol(buf, NULL, 10);
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
   lives in libs/world/destruction.cpp, both citing the same original.
   [orig: g_death_piece_types @ 0x8404f0; DeathPieceType_FindByName @ 0x57b310] */
static const char *const k_death_piece_type_names[13] = {
    "HULL",      "WHEEL",     "CHUNK_S",   "CHUNK_M",   "CHUNK_L",
    "ROCK_S",    "ROCK_M",    "ROCK_L",    "CHUNKNP_S", "CHUNKNP_M",
    "CHUNKNP_L", "CACTUS_",   "CHUNKSF_M",
};

int death_piece_type_index(const char *name, size_t len) {
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

const FlagEntry *lookup_flag(const char *name, size_t len) {
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

/* Parse N floats from value string (with comma replacement) */
int parse_floats(const char *s, size_t len, float *out, int max_n) {
    Token tok[MAX_TOKENS];
    int n = split_values(s, len, tok, max_n < MAX_TOKENS ? max_n : MAX_TOKENS);
    for (int i = 0; i < n && i < max_n; ++i)
        out[i] = parse_float_n(tok[i].s, tok[i].len);
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

} // namespace defscan
