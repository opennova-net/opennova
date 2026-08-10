#include "def/def.h"

// Split out of def.cpp (quality campaign W3-3). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// AMMO.DEF: one record per ammunition type.

#include "def_scan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace defscan; // the shared .def scanner, unqualified as before

/* ========================================================================= */
/* Ammo Parsing                                                              */
/* ========================================================================= */

/* `flag <name>` -> bit, matched first-name-wins in TABLE ORDER (three 'internal' names
 * exist; a data file writing "internal" hits 0x40000 first, faithfully).
 * [orig: the 30-entry table @0x813500; AmmoDef_ParseProperty @0x40a2d0] */
static const struct { const char *name; unsigned int bit; } k_ammo_flag_names[] = {
    {"ignoredmg", DEF_AMMO_FLAG_IGNOREDMG},        {"ignore", DEF_AMMO_FLAG_IGNORE},
    {"shrapnel", DEF_AMMO_FLAG_SHRAPNEL},         {"silenced", DEF_AMMO_FLAG_SILENCED},
    {"water", DEF_AMMO_FLAG_WATER},           {"detonatesatchels", DEF_AMMO_FLAG_DETONATESATCHELS},
    {"nosmoke", DEF_AMMO_FLAG_NOSMOKE},         {"nocollide", DEF_AMMO_FLAG_NOCOLLIDE},
    {"nogravity", DEF_AMMO_FLAG_NOGRAVITY},      {"hasitem", DEF_AMMO_FLAG_HASITEM},
    {"instantkillzone", DEF_AMMO_FLAG_INSTANTKILLZONE},{"ownerimmune", DEF_AMMO_FLAG_OWNERIMMUNE},
    {"useownmove", DEF_AMMO_FLAG_USEOWNMOVE},    {"noage", DEF_AMMO_FLAG_NOAGE},
    {"forcetracer", DEF_AMMO_FLAG_FORCETRACER},   {"shotgun", DEF_AMMO_FLAG_SHOTGUN},
    {"claymore", DEF_AMMO_FLAG_CLAYMORE},     {"nooitems", DEF_AMMO_FLAG_NOOITEMS},
    {"nomitems", DEF_AMMO_FLAG_NOMITEMS},    {"noditems", DEF_AMMO_FLAG_NODITEMS},
    {"ignorfoilage", DEF_AMMO_FLAG_IGNORFOILAGE},{"priority", DEF_AMMO_FLAG_PRIORITY},
    {"clipwater", DEF_AMMO_FLAG_CLIPWATER},  {"clipwaterfx", DEF_AMMO_FLAG_CLIPWATERFX},
    {"designatetarget", DEF_AMMO_FLAG_DESIGNATETARGET}, {"lawr", DEF_AMMO_FLAG_LAWR},
    {"fgrenade", DEF_AMMO_FLAG_FGRENADE},  {"internal", 0x40000u},
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
