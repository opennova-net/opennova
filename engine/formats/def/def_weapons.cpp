#include <formats/def/def.h>

// WEAPONS.DEF: one record per weapon.

#include "def_scan.h"

#include <base/io/ascii_config.h>
#include <base/io/strutil.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

using namespace opennova::defscan; // the shared .def scanner, unqualified as before

namespace opennova::def {

/* ========================================================================= */
/* Weapons Parsing                                                           */
/* ========================================================================= */

/* Every line reaches the parser as the retail tokenizer cuts it
   (defscan::for_each_def_line), and every key is the WHOLE first token
   compared without case: `END // x` closes a block, `KEY,value` binds, a
   `weapon` or `action` name needs no quotes (the AT4 and RPG entries open
   their scopeup rows with a bare `ACTION SCOPEUP`), and a scalar key reads its
   first value token only.
   [orig: WeaponDefs_ParseLineCallback @0x543680 (stricmp on token 0: "weapon"
   @0x54369D, "end" @0x54374C, "ammoclass_max_carry" @0x5437F2, "action"
   @0x5438CF, the scalar reads of tokens[2] throughout);
   ActionDef_ParseScriptLine @0x4023C0 ("action" @0x4023F3, "end" @0x40251B)] */

/* abs of a 32-bit atol, wrapping as the original's cdq/xor/sub does
   (abs(INT_MIN) stays INT_MIN). */
static int abs32_of(long value) {
    const uint32_t bits = static_cast<uint32_t>(value);
    return static_cast<int>(static_cast<int32_t>(value) < 0 ? 0u - bits : bits);
}

/* CRT atof on a token span: the double the retail parse multiplies before its
   ftol, kept unnarrowed (parse_float_n rounds through a float). */
static double parse_double_n(const char *s, size_t len) {
    char buf[64];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, s, len);
    buf[len] = '\0';
    return strtod(buf, NULL);
}

// Keep the decimal-digit angle parser ahead of BAM promotion; a float degree
// intermediate loses authored Q16 bits around the 180/360-degree seams.
// A line with fewer than six values is refused whole: the original's token
// count gate (key + 6 values) warns "too few params" and returns before the
// first store, so the row keeps whatever it held.
// [orig: WeaponDefs_ParseLineCallback @0x543680: the pos gate @0x5445EE..0x544613
//  and tpos gate @0x544735..0x54475A -> WeaponDefs_ParseWarning; the
//  Math_ParseFixedPoint16 calls @0x544662 / @0x5447A9]
static void parse_view_pose(const io::ConfigTokens &tokens, float position[3], int32_t rotation[3]) {
    Token values[6];
    if (value_tokens(tokens, values, 6) < 6) return;
    for (int i = 0; i < 3; ++i)
        position[i] = parse_float_n(values[i].s, values[i].len);
    for (int i = 0; i < 3; ++i)
        rotation[i] = parse_fixed16_digits_n(values[i + 3].s, values[i + 3].len);
}

/* `auto` or a tick count: -1 marks the automatic delay [orig:
   ActionDef_ParseScriptLine, the "auto" compare @0x40272B / @0x402B41]. */
static int parse_delay(const char *v, size_t vl) {
    return strutil::iequals(std::string_view(v, vl), "auto") ? -1 : parse_int_n(v, vl);
}

/* Shared buffer parser for weapon.def, used by both the path and memory entry points. */
static int parse_weapons_buf(const char *buf, size_t file_len, DefWeaponsFile *out,
                             const DefFileProbe *files) {
    enum { ST_TOP, ST_WEAPON, ST_ACTION };
    int state = ST_TOP;

    size_t entries_cap = 0, carry_cap = 0;
    DefWeaponDef cw; memset(&cw, 0, sizeof(cw));
    DefWeaponAction ca; memset(&ca, 0, sizeof(ca));
    size_t cw_raw_cap = 0, cw_act_cap = 0, cw_sight_cap = 0;
    size_t ca_raw_cap = 0;

    // Every line counts: split at LF, a CR before it dropped, which for CR LF
    // text is the retail walk's numbering [orig: File_ParseASCIIFile @0x53D8C7..0x53D8F5].
    for_each_def_line(buf, file_len, [&](const io::ConfigTokens &tokens, const char *line,
                                         size_t line_len, size_t line_index) {
        const char *key = tokens.tokens[0];
        const char *v = tokens.token(1); // the first value token, "" when none
        const size_t vl = strlen(v);

        /* Top-level ammoclass_max_carry: a table row wherever it stands, read
           before the in-ACTION forward and the current-weapon gate, from the
           tokens: the class and abs(atol) of the value.
           [orig: WeaponDefs_ParseLineCallback, the key @0x5437F2, tokens[2]
           @0x5437FE, abs(atol(tokens[3])) @0x543862..0x543873] */
        if (key_is(key, "ammoclass_max_carry")) {
            DefAmmoClassCarry carry;
            memset(&carry, 0, sizeof(carry));
            safe_copy(carry.name, sizeof(carry.name), v, vl);
            const char *cap = tokens.token(2);
            carry.cap = abs32_of(strtol(cap, NULL, 10));
            DA_PUSH(out->ammo_class_carries, out->ammo_class_carries_count, carry_cap, carry);
            return;
        }

        if (state == ST_TOP) {
            if (key_is(key, "weapon")) {
                memset(&cw, 0, sizeof(cw));
                cw_raw_cap = 0; cw_act_cap = 0; cw_sight_cap = 0;
                /* [orig: AdmDef_InitEntryDefaults @ 0x53ff31 seeds renderfov = 80.0] */
                cw.renderfov = 80.0f;
                /* [orig: AdmDef_InitEntryDefaults def[38] = 2 @ 0x53ff73 -> +0x98
                   'scope_min_mag', the scope zoom floor] */
                cw.scope_min_mag = 2;
                /* [orig: AdmDef_InitEntryDefaults @ 0x53FF61/0x53FF67/0x53FF6D] */
                for (int &stability : cw.stability_fp16) stability = 0x10000;
                safe_copy(cw.weapon_name, sizeof(cw.weapon_name), v, vl);
                cw.open_line = line_index;
                state = ST_WEAPON;
            }
            return;
        }

        if (state == ST_WEAPON) {
            if (key_is(key, "action")) {
                memset(&ca, 0, sizeof(ca));
                ca_raw_cap = 0;
                safe_copy(ca.name, sizeof(ca.name), v, vl);
                ca.open_line = line_index;
                state = ST_ACTION;
                return;
            }

            if (key_is(key, "end")) {
                cw.end_line = line_index;
                DA_PUSH(out->entries, out->count, entries_cap, cw);
                memset(&cw, 0, sizeof(cw));
                cw_raw_cap = 0; cw_act_cap = 0; cw_sight_cap = 0;
                state = ST_TOP;
                return;
            }

            int parsed = 0;
            if (key_is(key, "category")) {
                cw.category = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "rank")) {
                cw.rank = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "clipsize")) {
                cw.clipsize = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "startrounds")) {
                cw.startrounds = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "targetyawrange")) {
                /* Emplaced turret azimuth half-arc, degrees (the "180 tripod"
                   authors 90). Seeds the itemDef turret-limit fallback
                   [orig: Entity_GetWeaponTurretLimits @0x540e35]. */
                cw.targetyawrange = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "targetpitchmax")) {
                cw.targetpitchmax = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "targetpitchmin")) {
                cw.targetpitchmin = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "classrounds")) {
                /* classrounds <class> <n> — the class token resolves through the
                   char-class VALUE table (medic=1 sniper=2 gunner=3 rifleman=5
                   engineer=6, must be <= 6) and stores at the value's index
                   [orig: handler @ 0x543ab0 -> AdmDef+0x60+value*4, table @ 0x830EE8]. */
                Token tok[kMaxValueTokens];
                int n = value_tokens(tokens, tok, kMaxValueTokens);
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
            } else if (key_is(key, "switchcategory")) {
                /* switchcategory <N> — post-recoil auto-switch target category
                   [orig: handler @ 0x5445a8 -> +0x168 flag, +0x164 category]. */
                cw.has_switchcategory = 1;
                cw.switchcategory = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "statid")) {
                cw.statid = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "maxclips")) {
                /* [orig: parse @0x5440A9 -> AdmDef[83]+0x14C] */
                cw.maxclips = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "ammobucket")) {
                /* abs(atol) [orig: the cdq/xor/sub @0x544037 -> +0xDC] */
                cw.ammobucket = abs32_of(strtol(v, NULL, 10));
                parsed = 1;
            } else if (key_is(key, "sameas")) {
                /* [orig: @0x544056..0x544072 -- strncpy(AdmDef+0x34, value, 0x20):
                   at most 32 characters survive; a 32-character name keeps no
                   terminator in the record and its readers run on into the
                   next field, which this copy does not reproduce] */
                safe_copy(cw.sameas, sizeof(cw.sameas), v, vl);
                parsed = 1;
            } else if (key_is(key, "ammoclass")) {
                /* ammoclass <CLASS_NAME> <pool-units-per-round>: the count is
                   abs(atol) of token 2, read whether or not the line carries it
                   [orig: parse @0x5441CB, the count @0x54422E..0x544242] */
                copy_token(cw.ammo_class, sizeof(cw.ammo_class), tokens, 1);
                cw.ammo_class_count = abs32_of(strtol(tokens.token(2), NULL, 10));
                parsed = 1;
            } else if (key_is(key, "attachtextid")) {
                /* The attach-label text key; the original resolves it against the
                   Gametext "Overlays" section at parse and stores the char* at
                   AdmDef+0x3A0 [orig: @ 0x544d6c]. We keep the key for the HUD. */
                safe_copy(cw.attach_text_id, sizeof(cw.attach_text_id), v, vl);
                parsed = 1;
            } else if (key_is(key, "loadout_selectable")) {
                cw.loadout_selectable = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "loadout_subclasses")) {
                /* [orig: parse @0x544E43 -> AdmDef[235]+0x3AC] */
                cw.loadout_subclasses = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "weapon_class")) {
                /* Dual representation: the raw file token, plus the loadout slot the
                   original producer routes by (0=accessory 1=primary 2=secondary
                   3=grenade) [orig: WeaponDef_ParseProperty @ 0x54d730]. */
                safe_copy(cw.weapon_class, sizeof(cw.weapon_class), v, vl);
                if (strutil::iequals(v, "accessory")) cw.weapon_class_slot = 0;
                else if (strutil::iequals(v, "primary")) cw.weapon_class_slot = 1;
                else if (strutil::iequals(v, "secondary")) cw.weapon_class_slot = 2;
                else if (strutil::iequals(v, "grenade")) cw.weapon_class_slot = 3;
                parsed = 1;
            } else if (key_is(key, "charfilter")) {
                /* Repeatable, one soldier-type token per line: only token 1 is
                   read, its bit ORed in, any further token ignored [orig: the
                   table walk over tokens[2] @0x543F40..0x543F6E]. Also packs the
                   original producer's class-mask bit [orig: WeaponDef_ParseProperty
                   @ 0x54d730]. */
                if (tokens.count >= 2 && cw.charfilter_count < 8) {
                    copy_token(cw.charfilter[cw.charfilter_count], sizeof(cw.charfilter[0]),
                               tokens, 1);
                    ++cw.charfilter_count;
                }
                if (strutil::iequals(v, "medic")) cw.charfilter_mask |= 1;
                else if (strutil::iequals(v, "sniper")) cw.charfilter_mask |= 2;
                else if (strutil::iequals(v, "gunner")) cw.charfilter_mask |= 4;
                else if (strutil::iequals(v, "rifleman")) cw.charfilter_mask |= 8;
                else if (strutil::iequals(v, "engineer")) cw.charfilter_mask |= 16;
                parsed = 1;
            } else if (key_is(key, "teamfilter")) {
                /* Repeatable, one team token per line: only token 1 is read
                   [orig: the table walk over tokens[2] @0x543FB5..0x543FE3]. Also
                   packs the original producer's team-mask bit
                   [orig: WeaponDef_ParseProperty @ 0x54d730]. */
                if (tokens.count >= 2 && cw.teamfilter_count < 4) {
                    copy_token(cw.teamfilter[cw.teamfilter_count], sizeof(cw.teamfilter[0]),
                               tokens, 1);
                    ++cw.teamfilter_count;
                }
                if (strutil::iequals(v, "blue") || strutil::iequals(v, "yellow"))
                    cw.teamfilter_mask |= 2;
                else if (strutil::iequals(v, "red") || strutil::iequals(v, "violet"))
                    cw.teamfilter_mask |= 1;
                parsed = 1;
            } else if (key_is(key, "round_type")) {
                safe_copy(cw.round_type, sizeof(cw.round_type), v, vl);
                parsed = 1;
            /* PLAYER_INFO loadout tokens [orig: WeaponDef_ParseProperty @ 0x54d730]. */
            } else if (key_is(key, "weaponweight")) {
                cw.weaponweight = parse_float_n(v, vl);
                /* [orig: WeaponDefs_ParseLineCallback store @ 0x54410D;
                   Math_ParseFixedPoint16 @ 0x6131F0] */
                cw.weaponweight_fp16 = parse_fixed16_digits_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "clipweight")) {
                cw.clipweight = parse_float_n(v, vl);
                /* [orig: WeaponDefs_ParseLineCallback store @ 0x5440DB;
                   Math_ParseFixedPoint16 @ 0x6131F0] */
                cw.clipweight_fp16 = parse_fixed16_digits_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "loadout_menu_textid")) {
                safe_copy(cw.loadout_menu_textid, sizeof(cw.loadout_menu_textid), v, vl);
                parsed = 1;
            } else if (key_is(key, "loadout_menu_ttdesc")) {
                safe_copy(cw.loadout_menu_ttdesc, sizeof(cw.loadout_menu_ttdesc), v, vl);
                parsed = 1;
            } else if (key_is(key, "loadout_menu_icon")) {
                safe_copy(cw.loadout_menu_icon, sizeof(cw.loadout_menu_icon), v, vl);
                parsed = 1;
            } else if (key_is(key, "animadm")) {
                safe_copy(cw.animadm, sizeof(cw.animadm), v, vl);
                parsed = 1;
            } else if (key_is(key, "launchuserpoint")) {
                safe_copy(cw.launch_user_point, sizeof(cw.launch_user_point), v, vl);
                parsed = 1;
            } else if (key_is(key, "crosshair")) {
                Token tok[2];
                int n = value_tokens(tokens, tok, 2);
                if (n > 0)
                    safe_copy(cw.crosshair, sizeof(cw.crosshair), tok[0].s, tok[0].len);
                if (n > 1)
                    safe_copy(cw.crosshair_secondary, sizeof(cw.crosshair_secondary), tok[1].s,
                            tok[1].len);
                parsed = 1;
            } else if (key_is(key, "commandersx")) {
                safe_copy(cw.commanders_x, sizeof(cw.commanders_x), v, vl);
                parsed = 1;
            } else if (key_is(key, "hud_loadout_select")) {
                safe_copy(cw.hud_loadout_select, sizeof(cw.hud_loadout_select), v, vl);
                parsed = 1;
            } else if (key_is(key, "splash")) {
                cw.splash = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "hudicon")) {
                safe_copy(cw.hudicon, sizeof(cw.hudicon), v, vl);
                parsed = 1;
            } else if (key_is(key, "hudclipgfx")) {
                /* HUDCLIPGFX: offset x/y + texture [orig: WeaponDefs_ParseLineCallback
                   @0x54427f]. */
                Token tok[kMaxValueTokens];
                int n = value_tokens(tokens, tok, kMaxValueTokens);
                if (n >= 3) {
                    cw.hudclipgfx_offset[0] = parse_int_n(tok[0].s, tok[0].len);
                    cw.hudclipgfx_offset[1] = parse_int_n(tok[1].s, tok[1].len);
                    safe_copy(cw.hudclipgfx_texture, sizeof(cw.hudclipgfx_texture), tok[2].s, tok[2].len);
                }
                parsed = 1;
            } else if (key_is(key, "hudrndgfx")) {
                /* HUDRNDGFX: start x/y, step x/y, rounds-per-icon divisor, texture
                   [orig: WeaponDefs_ParseLineCallback @0x5442fc -> weapon
                   +644/+648/+652/+656/+727]. */
                Token tok[kMaxValueTokens];
                int n = value_tokens(tokens, tok, kMaxValueTokens);
                if (n >= 6) {
                    cw.hudrndgfx_offset[0] = parse_int_n(tok[0].s, tok[0].len);
                    cw.hudrndgfx_offset[1] = parse_int_n(tok[1].s, tok[1].len);
                    cw.hudrndgfx_layout[0] = parse_int_n(tok[2].s, tok[2].len);
                    cw.hudrndgfx_layout[1] = parse_int_n(tok[3].s, tok[3].len);
                    cw.hudrndgfx_layout[2] = parse_int_n(tok[4].s, tok[4].len);
                    safe_copy(cw.hudrndgfx_texture, sizeof(cw.hudrndgfx_texture), tok[5].s, tok[5].len);
                }
                parsed = 1;
            } else if (key_is(key, "gfx1a")) {
                safe_copy(cw.gfx1a, sizeof(cw.gfx1a), v, vl);
                parsed = 1;
            } else if (key_is(key, "gfx1b")) {
                safe_copy(cw.gfx1b, sizeof(cw.gfx1b), v, vl);
                parsed = 1;
            } else if (key_is(key, "gfx1")) {
                safe_copy(cw.gfx1, sizeof(cw.gfx1), v, vl);
                parsed = 1;
            } else if (key_is(key, "gfx3")) {
                safe_copy(cw.gfx3, sizeof(cw.gfx3), v, vl);
                parsed = 1;
            } else if (key_is(key, "flags")) {
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
            } else if (key_is(key, "stability")) {
                Token values[3];
                int count = value_tokens(tokens, values, 3);
                /* [orig: WeaponDefs_ParseLineCallback @ 0x544118..0x544169;
                   Math_ParseFixedPoint16 @ 0x6131F0] */
                for (int i = 0; i < count; ++i)
                    cw.stability_fp16[i] = parse_fixed16_digits_n(values[i].s, values[i].len);
                parsed = 1;
            } else if (key_is(key, "error")) {
                /* Six independent 16.16 parses, stored consecutively at
                   AdmDef+0xB0..+0xC4. Keep the float view for existing callers.
                   [orig: WeaponDefs_ParseLineCallback @ 0x543B21-0x543BB5;
                   Math_ParseFixedPoint16 @ 0x6131F0] */
                Token values[6];
                int count = value_tokens(tokens, values, 6);
                for (int i = 0; i < count; ++i) {
                    cw.error[i] = parse_float_n(values[i].s, values[i].len);
                    cw.error_fp16[i] = parse_fixed16_digits_n(values[i].s, values[i].len);
                }
                parsed = 1;
            } else if (key_is(key, "error_hiptheta")) {
                /* [orig: WeaponDefs_ParseLineCallback @ 0x543BC0, store +0xCC
                   @ 0x543BE7; Math_ParseFixedPoint16 @ 0x6131F0] */
                cw.error_hip_theta_fp16 = parse_fixed16_digits_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "error_uptheta")) {
                /* [orig: WeaponDefs_ParseLineCallback @ 0x543BF2, store +0xD0
                   @ 0x543C19; Math_ParseFixedPoint16 @ 0x6131F0] */
                cw.error_up_theta_fp16 = parse_fixed16_digits_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "renderfov")) {
                /* [orig: weapon.def parser key 'renderfov' @ 0x54482a] */
                cw.renderfov = parse_float_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "scope_max_mag")) {
                /* ADS zoom magnification; the scoped FOV = 80 / clamped zoom
                   [orig: Player_ToggleWeaponScope @ 0x4df401]. Two atol'd values:
                   the max -> +0x90 and the slot's initial zoom -> +0x94, which
                   stays 0 when the row carries one value (atol of the empty
                   second token) [orig: WeaponDefs_ParseLineCallback @ 0x544f1e
                   / @ 0x544f29 and @ 0x544f33 / @ 0x544f44]. */
                Token values[2];
                const int count = value_tokens(tokens, values, 2);
                cw.scope_max_mag = count >= 1
                        ? (float)parse_int_n(values[0].s, values[0].len) : 0.0f;
                cw.scope_max_mag_arg2 = count >= 2
                        ? parse_int_n(values[1].s, values[1].len) : 0;
                parsed = 1;
            } else if (key_is(key, "scope_min_mag")) {
                /* The scope zoom floor -> +0x98, atol [orig: WeaponDefs_ParseLineCallback
                   @ 0x544f4f..0x544f7a]. */
                cw.scope_min_mag = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "soundfireloop")) {
                safe_copy(cw.soundfireloop, sizeof(cw.soundfireloop), v, vl);
                parsed = 1;
            } else if (key_is(key, "soundtrailoff")) {
                safe_copy(cw.soundtrailoff, sizeof(cw.soundtrailoff), v, vl);
                parsed = 1;
            } else if (key_is(key, "vmacrotoken")) {
                safe_copy(cw.vmacrotoken, sizeof(cw.vmacrotoken), v, vl);
                parsed = 1;
            } else if (key_is(key, "soundhead")) {
                safe_copy(cw.soundhead, sizeof(cw.soundhead), v, vl);
                parsed = 1;
            } else if (key_is(key, "soundlockedtone")) {
                safe_copy(cw.soundlockedtone, sizeof(cw.soundlockedtone), v, vl);
                parsed = 1;
            } else if (key_is(key, "scope_paralax_distance")) {
                /* The sight's parallax height: atof * 65535.0 (dbl_7D0958), ftol
                   -> +0x8C, the zero-yaw atan2's numerator; see def.h.
                   [orig: WeaponDefs_ParseLineCallback @ 0x544e4e..0x544e80 — atof
                    @ 0x544e64, the multiply @ 0x544e69, ftol @ 0x544e72, the store
                    @ 0x544e80] */
                cw.scope_paralax_distance_fp16 = (int)(parse_double_n(v, vl) * 65535.0);
                parsed = 1;
            } else if (key_is(key, "scope_max_zero")) {
                /* The scope-zero table: atol x3 in order -> +0x84 / +0x9C / +0xA0,
                   plus an optional fourth value -> +0x88, stored only when the line
                   carries four (the token count the gate compares includes the key:
                   `cmp dword ptr [esi],4; jle`). Consumers: the SIGHTS `slide`
                   multiplier in HUD_DrawWeaponSightOverlays @ 0x4dcf57..0x4dcff7 and
                   Weapon_GetScopeZoomLevel @ 0x422ff3; see def.h.
                   [orig: WeaponDefs_ParseLineCallback @ 0x544e8b..0x544efd — the
                    stores @ 0x544eac / @ 0x544ec1 / @ 0x544ed9, the count gate
                    @ 0x544edf, the fourth store @ 0x544efd] */
                Token zv[kMaxValueTokens];
                int zn = value_tokens(tokens, zv, kMaxValueTokens);
                if (zn >= 1) cw.scope_max_zero_steps = parse_int_n(zv[0].s, zv[0].len);
                if (zn >= 2) cw.scope_zero_step = parse_int_n(zv[1].s, zv[1].len);
                if (zn >= 3) cw.scope_zero_default = parse_int_n(zv[2].s, zv[2].len);
                if (zn >= 4) cw.scope_zero_extra = parse_int_n(zv[3].s, zv[3].len);
                parsed = 1;
            } else if (key_is(key, "heat_values")) {
                /* percent-per-shot / percent-per-second, each through the engine's
                   digit parser then integer-divided by 100 and by 100*62 (the logic
                   rate) — the two truncating divides are load-bearing, see def.h.
                   [orig: @ 0x543eb7 -> +0x36C / +0x370] */
                Token hv[kMaxValueTokens];
                int hn = value_tokens(tokens, hv, kMaxValueTokens);
                if (hn >= 1) cw.heat_per_shot = parse_fixed16_digits_n(hv[0].s, hv[0].len) / 100;
                if (hn >= 2) cw.heat_decay_per_tick = parse_fixed16_digits_n(hv[1].s, hv[1].len) / 6200;
                parsed = 1;
            } else if (key_is(key, "heat_effect")) {
                /* Effect name + the 16.16 glow threshold; any further values on the
                   line are unread in retail too. [orig: @ 0x543e36 -> +0x358 / +0x374] */
                Token hv[kMaxValueTokens];
                int hn = value_tokens(tokens, hv, kMaxValueTokens);
                if (hn >= 1) safe_copy(cw.heat_effect, sizeof(cw.heat_effect), hv[0].s, hv[0].len);
                if (hn >= 2) cw.heat_glow_threshold = parse_fixed16_digits_n(hv[1].s, hv[1].len);
                parsed = 1;
            } else if (key_is(key, "emplacedstance")) {
                cw.emplacedstance = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "special_hold")) {
                /* 3P hold-pose kind, atol [orig: weapon.def key 'special_hold' ->
                   record+0xA4 @ 0x543cb7/0x543cd8]. */
                cw.special_hold = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "attack_anim")) {
                /* 3P fire attack-stamp kind, atol [orig: weapon.def key 'attack_anim' ->
                   record+0xA8 @ 0x543ce9/0x543d0a]. */
                cw.attack_anim = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "run_anim")) {
                /* Run-gait class, atol [orig: weapon.def key 'run_anim' ->
                   record+0xAC @ 0x543d15/0x543d3c]. */
                cw.run_anim = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "pos")) {
                parse_view_pose(tokens, cw.pos, cw.pos_rotation_deg_q16);
                parsed = 1;
            } else if (key_is(key, "tpos")) {
                parse_view_pose(tokens, cw.tpos, cw.tpos_rotation_deg_q16);
                parsed = 1;
            } else if (key_is(key, "sights")) {
                /* A row is refused when its texture is no file the game can
                   open (a warning, no row) [orig: FileSystem_FileExists(tokens[2])
                   @0x544AE2, the return @0x544B10]; otherwise the row is taken
                   whatever the line's count, a corner past it reading as atol("")
                   [orig: atol of tokens[3..6] @0x544B48..0x544B6C]. The record
                   holds four 36-byte rows ahead of their count (+0x1C8..+0x257,
                   the count @+0x258), so a fifth row lands on the count itself and
                   corrupts the record; rows past the fourth are not kept
                   (D-ITEMDEF-7) [orig: the count bump @0x544B11..0x544B20, the row
                   address @0x544B2F..0x544B32]. */
                const char *texture = tokens.token(1);
                const bool exists = texture[0] != '\0' &&
                        (files == nullptr || files->exists == nullptr ||
                         files->exists(files->ctx, texture));
                if (exists && cw.sights_count < 4) {
                    DefSightEntry se;
                    memset(&se, 0, sizeof(se));
                    copy_token(se.texture, sizeof(se.texture), tokens, 1);
                    se.x1 = (int)strtol(tokens.token(2), NULL, 10);
                    se.y1 = (int)strtol(tokens.token(3), NULL, 10);
                    se.x2 = (int)strtol(tokens.token(4), NULL, 10);
                    se.y2 = (int)strtol(tokens.token(5), NULL, 10);
                    /* By position: the blend mode is token 6 (a name the
                       material maker does not know, or none, is `blend`), and
                       token 7 the one `scale` or `slide` flag, `slide`'s frame
                       count token 8, read only on a line of 8 tokens or more
                       [orig: the sights arm @0x544AC8 —
                        WeaponDef_CreateBlendNamedMaterial @0x540190 over
                        tokens[7] (@0x544B3F), the `cmp [esi],8; jl` @0x544B7A,
                        "scale" @0x544B86 and "slide" @0x544BA2 against
                        tokens[8], atol(tokens[9]) @0x544BC3] */
                    const char *blend = tokens.token(6);
                    if (strutil::iequals(blend, "add")) se.blend = DEF_SIGHT_BLEND_ADD;
                    else if (strutil::iequals(blend, "multiply")) se.blend = DEF_SIGHT_BLEND_MULTIPLY;
                    else if (strutil::iequals(blend, "blendat")) se.blend = DEF_SIGHT_BLEND_BLEND_AT;
                    else if (strutil::iequals(blend, "addat")) se.blend = DEF_SIGHT_BLEND_ADD_AT;
                    else if (strutil::iequals(blend, "multiplyat")) se.blend = DEF_SIGHT_BLEND_MULTIPLY_AT;
                    else se.blend = DEF_SIGHT_BLEND_BLEND;
                    if (tokens.count >= 8) {
                        const char *flag = tokens.token(7);
                        if (strutil::iequals(flag, "scale")) se.scale = 1;
                        if (strutil::iequals(flag, "slide")) {
                            se.slide = 1;
                            se.slide_frames = (int)strtol(tokens.token(8), NULL, 10);
                        }
                    }
                    DA_PUSH(cw.sights, cw.sights_count, cw_sight_cap, se);
                }
                parsed = 1;
            }

            if (!parsed) {
                DA_PUSH_RAW(cw.raw_lines, cw.raw_lines_count, cw_raw_cap, line, line_len);
            }
            return;
        }

        if (state == ST_ACTION) {
            if (key_is(key, "end")) {
                /* One row per suffix: a later block of the same name finds the
                   row and re-runs ActionDef_InitDefaults on it, so it replaces
                   the earlier block wholesale and nothing of that one survives.
                   [orig: ActionDef_ParseScriptLine @0x4023C0 — the name lookup
                   ActionDef_FindByNameInTable @0x402360, found @0x4024A1, both
                   paths into InitDefaults @0x4024DA] */
                ca.end_line = line_index;
                size_t row = cw.actions_count;
                for (size_t i = 0; i < cw.actions_count; ++i)
                    if (strutil::iequals(cw.actions[i].name, ca.name)) row = i;
                if (row < cw.actions_count) {
                    free(cw.actions[row].raw_lines);
                    cw.actions[row] = ca;
                } else {
                    DA_PUSH(cw.actions, cw.actions_count, cw_act_cap, ca);
                }
                memset(&ca, 0, sizeof(ca));
                ca_raw_cap = 0;
                state = ST_WEAPON;
                return;
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
            if (key_is(key, "anim")) {
                /* [orig: the strcpy of tokens[2] into ActionDef+58 @0x402873] */
                safe_copy(ca.anim, sizeof(ca.anim), v, vl);
                parsed = 1;
            } else if (key_is(key, "function")) {
                safe_copy(ca.function, sizeof(ca.function), v, vl);
                parsed = 1;
            } else if (key_is(key, "delaystart")) {
                ca.delaystart = parse_delay(v, vl);
                parsed = 1;
            } else if (key_is(key, "delayend") || key_is(key, "delay")) {
                /* bare `delay` is an alias of delayend — both write +40
                   [orig: ActionDef_ParseScriptLine @ 0x40279a / @ 0x402b2c] */
                ca.delayend = parse_delay(v, vl);
                parsed = 1;
            } else if (key_is(key, "action_value")) {
                ca.action_value = parse_int_n(v, vl);
                parsed = 1;
            } else if (key_is(key, "soundsetend")) {
                safe_copy(ca.soundsetend, sizeof(ca.soundsetend), v, vl);
                parsed = 1;
            } else if (key_is(key, "soundset")) {
                safe_copy(ca.soundset, sizeof(ca.soundset), v, vl);
                parsed = 1;
            } else if (key_is(key, "particleuserpoint")) {
                safe_copy(ca.particleuserpoint, sizeof(ca.particleuserpoint), v, vl);
                parsed = 1;
            } else if (key_is(key, "particle")) {
                safe_copy(ca.particle, sizeof(ca.particle), v, vl);
                parsed = 1;
            }

            if (!parsed) {
                DA_PUSH_RAW(ca.raw_lines, ca.raw_lines_count, ca_raw_cap, line, line_len);
            }
        }
    });

    return 0;
}

int def_parse_weapons(const char *path, DefWeaponsFile *out, const DefFileProbe *files) {
    memset(out, 0, sizeof(*out));
    size_t file_len;
    char *buf = read_file(path, &file_len);
    if (!buf) return -1;
    int rc = parse_weapons_buf(buf, file_len, out, files);
    free(buf);
    return rc;
}

int def_parse_weapons_memory(const uint8_t *data, size_t size, DefWeaponsFile *out,
                             const DefFileProbe *files) {
    memset(out, 0, sizeof(*out));
    if (!data) return -1;
    return parse_weapons_buf((const char *)data, size, out, files);
}

void def_free_weapons(DefWeaponsFile *f) {
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
    free(f->ammo_class_carries);
    memset(f, 0, sizeof(*f));
}

} // namespace opennova::def
