#include <formats/def/def.h>
#include <base/io/tick_rate.h>

// AMMO.DEF: one record per ammunition type.

#include "def_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace opennova::defscan; // the shared .def scanner, unqualified as before

namespace opennova::def {

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

/* Decimal string -> 16.16 fixed point through the engine's digit walker
   (defscan::parse_fixed16_digits_n — the exact structural translation shared with
   the weapon.def keys): integer part by digits, then fractional digits at a scale
   that starts at 2^24 and is repeatedly multiplied by 419430/2^22 (a hair UNDER
   1/10), accumulator seeded 127 so the closing >> 8 rounds to nearest. No sign and
   no exponent: a leading '-' terminates the integer scan and yields 0, exactly as
   the original does. Every fixed-point ammo key routes through it: error @0x40aaf6,
   drag @0x40aac8, bullet_radius @0x40a865, kz_minradius @0x40acfe, kz_maxradius
   @0x40ad2c, tumble_error @0x40ab24, light_move @0x40af3a (all in
   AmmoDef_ParseProperty @0x40a2d0), closing D-WPN-30's round-half-up stand-in.
   [orig: Math_ParseFixedPoint16 @0x6131f0] */

/* Parsed 16.16 seconds -> 62 Hz ticks with rounding. [orig: AmmoDef_ParseSecondsToTicks (ex sub_40A0F0) @0x40a0f0 —
 * (62 * fp16 + 0x8000) >> 16] */
static int parse_age_ticks_n(const char *s, size_t len) {
    return (int)(((long long)opennova::io::kTicksPerSecondInt * parse_fixed16_digits_n(s, len) + 0x8000) >> 16);
}

/* Every line reaches the parser as the retail tokenizer cuts it
   (defscan::for_each_def_line); every key is the whole first token compared
   without case, and a value is a token [orig: AmmoDef_ParseProperty @0x40A2D0,
   _stricmp on tokens[1] throughout, tokens[2].. the values].

   A def is allocated at its `ammo` line, named by token 1 (31 characters at
   most), and closed by `end` [orig: `ammo` @0x40A347..0x40A397 ->
   AmmoDef_AllocateSlot @0x409A20 (the name copy @0x409B01..0x409B24); `end`
   @0x40A3B9..0x40A442 -> AmmoDef_ResetParseState @0x409B30 (@0x40A3F8), which
   clears the open def]. An `ammo` line while a def is open logs "definition
   missing end" and returns 1, which ends the whole file walk [orig:
   @0x40A362..0x40A37D; File_ParseASCIIFile @0x53D942]; the def it interrupted,
   and one the file never closes, were allocated already and stay in the table.
   Lines outside a def are ignored (@0x40A30A..0x40A310). `effects_table` opens
   the def's table, whose `end` closes the table and not the def [orig:
   @0x40A5A3..0x40A5B2; @0x40A42D..0x40A433]. */
static int parse_ammo_buffer(const char *buf, size_t file_len, DefAmmoFile *out) {
    size_t entries_cap = 0;
    DefAmmoDef current;
    memset(&current, 0, sizeof(current));
    int in_block = 0, in_effects = 0, stopped = 0;
    size_t raw_cap = 0, eff_cap = 0;

    for_each_def_line(buf, file_len, [&](const io::ConfigTokens &tokens, const char *line,
                                         size_t line_len, size_t) {
        if (stopped) return;
        const char *key = tokens.tokens[0];
        const char *v = tokens.token(1); // the first value token, "" when none
        const size_t vl = strlen(v);

        if (key_is(key, "ammo")) {
            if (in_block) {
                stopped = 1;
                return;
            }
            memset(&current, 0, sizeof(current));
            raw_cap = 0; eff_cap = 0;
            copy_token(current.name, 32, tokens, 1);
            in_block = 1;
            return;
        }

        if (key_is(key, "end")) {
            if (!in_block) return;
            if (in_effects) {
                in_effects = 0;
                return;
            }
            DA_PUSH(out->entries, out->count, entries_cap, current);
            memset(&current, 0, sizeof(current));
            raw_cap = 0; eff_cap = 0;
            in_block = 0;
            return;
        }

        if (!in_block) return;

        /* Effects table rows: four tokens or more, tag / hit effect / impact
           sound / value [orig: the table gate @0x40A316, `cmp [tokens],4`
           @0x40A323, the row @0x40A46A..0x40A531] */
        if (in_effects) {
            if (tokens.count >= 4) {
                DefEffectTableEntry e;
                memset(&e, 0, sizeof(e));
                copy_token(e.surface_type, sizeof(e.surface_type), tokens, 0);
                copy_token(e.hit_effect, sizeof(e.hit_effect), tokens, 1);
                copy_token(e.impact_sound, sizeof(e.impact_sound), tokens, 2);
                e.value = static_cast<int>(strtol(tokens.token(3), NULL, 10));
                DA_PUSH(current.effects_table, current.effects_table_count, eff_cap, e);
            }
            return;
        }

        if (key_is(key, "effects_table")) {
            in_effects = 1;
            return;
        }

        int parsed = 0;
        if (key_is(key, "velocity")) {
            current.velocity = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "heat_det_range")) {
            // [orig: AmmoDef_ParseProperty @0x40a2d0, heat_det_range]
            current.heat_det_range = static_cast<int16_t>(parse_fixed16_digits_n(v, vl) >> 16);
            parsed = 1;
        } else if (key_is(key, "boresight_maxang")) {
            current.boresight_maxang = static_cast<int32_t>(11930464u *
                static_cast<uint32_t>(parse_int_n(v, vl)));
            parsed = 1;
        } else if (key_is(key, "min_damage")) {
            current.min_damage = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "max_damage")) {
            current.max_damage = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "penetration_impact")) {
            current.penetration_impact = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "penetration_kz")) {
            current.penetration_kz = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "armor_density")) {
            // atol of tokens 1..3, unconditionally [orig: AmmoDef_ParseProperty @0x40ac29..0x40ac74]
            for (int c = 0; c < 3; ++c)
                current.armor_density[c] = static_cast<int>(strtol(tokens.token(c + 1), NULL, 10));
            parsed = 1;
        } else if (key_is(key, "secondary_effect")) {
            /* The blast's per-victim effect [orig: @0x40aa15..0x40aa36
               CEffectWorld_InternEffectHandle -> +0x48]. */
            copy_token(current.secondary_effect, sizeof(current.secondary_effect), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "kz_sound")) {
            /* The blast's per-victim sound set [orig: @0x40a92a..0x40a94b
               SoundBank_FindSetByNameAnyBank -> +0x4C]. */
            copy_token(current.kz_sound, sizeof(current.kz_sound), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "secondary_anim")) {
            current.secondary_anim = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "kz_physics")) {
            current.kz_physics = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "recoil")) {
            // atol of tokens 1..3, unconditionally [orig: AmmoDef_ParseProperty, the recoil arm]
            for (int c = 0; c < 3; ++c)
                current.recoil[c] = static_cast<int>(strtol(tokens.token(c + 1), NULL, 10));
            parsed = 1;
        } else if (key_is(key, "flag")) {
            /* OR the named bit; first table match wins [orig: @0x813500 walk]. An
               unrecognized name is skipped (the original warns). */
            char fl[64];
            size_t fn = vl < sizeof(fl) - 1 ? vl : sizeof(fl) - 1;
            to_lower_buf(fl, v, fn);
            for (size_t fi = 0; fi < sizeof(k_ammo_flag_names) / sizeof(k_ammo_flag_names[0]); ++fi) {
                if (strlen(k_ammo_flag_names[fi].name) == fn &&
                    memcmp(k_ammo_flag_names[fi].name, fl, fn) == 0) {
                    current.flags |= k_ammo_flag_names[fi].bit;
                    break;
                }
            }
            parsed = 1;
        } else if (key_is(key, "max_age")) {
            current.max_age_ticks = parse_age_ticks_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "arm_age")) {
            current.arm_age_ticks = parse_age_ticks_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "error")) {
            current.error_fp16 = parse_fixed16_digits_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "drag")) {
            current.drag_fp16 = parse_fixed16_digits_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "bullet_radius")) {
            current.bullet_radius_fp16 = parse_fixed16_digits_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "spread_count")) {
            current.spread_count = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "kztype")) {
            /* Full-name match against the 8-entry table; only 1..7 accepted
               [orig: @0x40a2d0 rejects index 0/unknown as "bad kill zone type"]. */
            char kz[48];
            size_t kn = vl < sizeof(kz) - 1 ? vl : sizeof(kz) - 1;
            to_lower_buf(kz, v, kn);
            for (int ki = 1; ki < 8; ++ki) {
                if (strlen(k_ammo_kz_names[ki]) == kn && memcmp(k_ammo_kz_names[ki], kz, kn) == 0) {
                    current.kztype = ki;
                    break;
                }
            }
            parsed = 1;
        } else if (key_is(key, "kz_damage")) {
            current.kz_damage = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "kz_minradius")) {
            current.kz_minradius_fp16 = parse_fixed16_digits_n(v, vl); /* +52 [orig: §5.60 map] */
            parsed = 1;
        } else if (key_is(key, "kz_maxradius")) {
            current.kz_maxradius_fp16 = parse_fixed16_digits_n(v, vl); /* +56 */
            parsed = 1;
        } else if (key_is(key, "kz_pieslice")) {
            /* HALF-angle: retail stores (deg / 2) x 11930464 BAM — the cone
             * tests compare |angle diff| <= this half-angle.
             * [orig: atol -> cdq/sub/sar signed div 2 -> imul 0xB60B60 @0x40ad51] */
            current.kz_pieslice_bam = (parse_int_n(v, vl) / 2) * 11930464;
            parsed = 1;
        } else if (key_is(key, "scorch_id")) {
            /* Permanent terrain scorch selector [orig: AmmoDef_ParseProperty
             * stores atol into word +0x74; Projectile_HandleTerrainImpact
             * reads it before the effect presenter]. */
            current.scorch_id = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "scar_type")) {
            /* The impact scar kind [orig: AmmoDef_ParseProperty @0x40aeea..0x40af11,
             * atol -> word +0x76]. */
            current.scar_type = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "min_stable_velocity")) {
            current.min_stable_velocity = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "tumble_error")) {
            current.tumble_error_fp16 = parse_fixed16_digits_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "weight_in_grains")) {
            current.weight_in_grains = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "tracerrate")) {
            current.tracer_rate = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "ai_launcheffect")) {
            /* [orig: @0x40a8fc stricmp] */
            copy_token(current.ai_launcheffect, sizeof(current.ai_launcheffect), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "ai_launch")) {
            /* The AI fire sound-set name; the original resolves the set pointer here
               [orig: @0x40a8c8-0x40a8ef SoundBank_FindSetByNameAnyBank -> +64]. */
            copy_token(current.ai_launch, sizeof(current.ai_launch), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "mf_light")) {
            /* Presence sets the +36 flag, the value lands beside it
               [orig: @0x40a81b dword +36 = 1; @0x40a826-0x40a837 +40 = atol]. */
            current.mf_light = 1;
            current.mf_light_value = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "tracer_type")) {
            /* 1-2 style names: a line with no value stores 0, and a single value
               fills both slots [orig: @0x40a79d-0x40a7fa: count < 2 -> +232 = 0,
               else the name; count < 3 -> +236 = +232, else the second name]. */
            current.tracer_type_friendly =
                    tokens.count < 2 ? 0 : ammo_tracer_type_from_name(v, vl);
            current.tracer_type_enemy = current.tracer_type_friendly;
            if (tokens.count >= 3) {
                const char *enemy = tokens.tokens[2];
                current.tracer_type_enemy = ammo_tracer_type_from_name(enemy, strlen(enemy));
            }
            parsed = 1;
        } else if (key_is(key, "notarmmedammo")) {
            copy_token(current.notarmmed_ammo, sizeof(current.notarmmed_ammo), tokens, 1);
            parsed = 1;
        } else if (key_is(key, "frndlytrcrid")) {
            /* The tracer round's friendly item model, by ITEMS.DEF type id. The
               original resolves to an item index here (name fallback, warning on
               miss) [orig: @0x40a5f8-0x40a63f -> +16]; we keep the raw type id. */
            current.frndly_trcr_type_id = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "foetrcrid")) {
            /* [orig: @0x40a646-0x40a68d -> +20] */
            current.foe_trcr_type_id = parse_int_n(v, vl);
            parsed = 1;
        } else if (key_is(key, "light_impact")) {
            /* The impact flash light: radius (16.16) + packed RGB + fade
               seconds -> 62 Hz ticks, 0 -> the witnessed 10-tick default; every
               token read unconditionally [orig: @0x40af79 -> +132/+128/+136;
               default @0x40b005]. */
            const char *radius = tokens.token(1);
            current.light_impact_radius_fp16 = parse_fixed16_digits_n(radius, strlen(radius));
            current.light_impact_color =
                    ((static_cast<int>(strtol(tokens.token(2), NULL, 10)) << 8) +
                     static_cast<int>(strtol(tokens.token(3), NULL, 10))) * 256 +
                    static_cast<int>(strtol(tokens.token(4), NULL, 10));
            const char *fade = tokens.token(5);
            current.light_impact_ticks = parse_age_ticks_n(fade, strlen(fade));
            if (current.light_impact_ticks == 0) current.light_impact_ticks = 10;
            parsed = 1;
        } else if (key_is(key, "light_move")) {
            /* The in-flight round glow: radius (16.16) + packed RGB, every token
               read unconditionally [orig: @0x40a2d0 'light_move' -> +120 =
               ParseFixedPoint16, +124 = (atol(r) << 16) | (atol(g) << 8) | atol(b)].
               No range clamps — the original's shifted adds bleed out-of-range
               components upward [orig: ((r<<8)+g)<<8 + b @0x40a2d0]. */
            const char *radius = tokens.token(1);
            current.light_move_radius_fp16 = parse_fixed16_digits_n(radius, strlen(radius));
            current.light_move_color =
                    ((static_cast<int>(strtol(tokens.token(2), NULL, 10)) << 8) +
                     static_cast<int>(strtol(tokens.token(3), NULL, 10))) * 256 +
                    static_cast<int>(strtol(tokens.token(4), NULL, 10));
            parsed = 1;
        } else if (key_is(key, "turnrate_maxpit")) {
            /* deg/s (fractional allowed) -> BAM/tick:
               (192426 * fp16 + 0x8000) >> 16 [orig: AmmoDef_ParseTurnRate @0x40a130,
               stored +0x50 @0x40adf0]; the guided pursuit clamp
               (world/guided_missile_flight.h). */
            current.turnrate_maxpit =
                (int)(((long long)192426 * parse_fixed16_digits_n(v, vl) + 0x8000) >> 16);
            parsed = 1;
        } else if (key_is(key, "turnrate_maxyaw")) {
            /* [orig: AmmoDef_ParseTurnRate @0x40a130, stored +0x54 @0x40ae1e] */
            current.turnrate_maxyaw =
                (int)(((long long)192426 * parse_fixed16_digits_n(v, vl) + 0x8000) >> 16);
            parsed = 1;
        }

        if (!parsed) {
            DA_PUSH_RAW(current.raw_lines, current.raw_lines_count, raw_cap, line, line_len);
        }
    });

    /* The def the walk stopped in, or that the file never closes, was allocated
       at its `ammo` line and stays in the table [orig: AmmoDef_AllocateSlot
       @0x409A20 from the `ammo` arm]. */
    if (in_block) DA_PUSH(out->entries, out->count, entries_cap, current);

    return 0;
}

int def_parse_ammo(const char *path, DefAmmoFile *out) {
    memset(out, 0, sizeof(*out));

    size_t file_len;
    char *buf = read_file(path, &file_len);
    if (!buf) return -1;
    int rc = parse_ammo_buffer(buf, file_len, out);
    free(buf);
    return rc;
}

int def_parse_ammo_memory(const uint8_t *data, size_t size, DefAmmoFile *out) {
    memset(out, 0, sizeof(*out));
    if (!data) return -1;
    return parse_ammo_buffer((const char *)data, size, out);
}

void def_free_ammo(DefAmmoFile *f) {
    if (!f) return;
    for (size_t i = 0; i < f->count; ++i) {
        free(f->entries[i].effects_table);
        free(f->entries[i].raw_lines);
    }
    free(f->entries);
    memset(f, 0, sizeof(*f));
}

} // namespace opennova::def
