#include <formats/def/def.h>

// WEAPONS.DEF: one record per weapon.

#include <base/io/crt_ftol.h>
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

/* abs of a 32-bit atol (io::retail_atol, saturating), wrapping as the
   original's cdq/xor/sub does (abs(INT_MIN) stays INT_MIN). */
static int abs32_of(int32_t value) {
    const uint32_t bits = static_cast<uint32_t>(value);
    return static_cast<int>(value < 0 ? 0u - bits : bits);
}

/* CRT atof on a token span: the double the retail parse multiplies before its
   ftol, kept unnarrowed (parse_float_n rounds through a float). */
static double parse_double_n(const char *s, size_t len) {
    return io::retail_atof_n(s, len);
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

/* Whether a named file is one the game can open: never an empty name, and
   every name when the reader was given no probe [orig: FileSystem_FileExists
   @0x75AA50]. */
static bool file_exists(const DefFileProbe *files, const char *name) {
    return name[0] != '\0' &&
           (files == nullptr || files->exists == nullptr || files->exists(files->ctx, name));
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
    bool stopped = false;

    /* One row per suffix: a later block of the same name finds the row and
       re-runs ActionDef_InitDefaults on it, so it replaces the earlier block
       wholesale and nothing of that one survives.
       [orig: ActionDef_ParseScriptLine @0x4023C0 — the name lookup
       ActionDef_FindByNameInTable @0x402360, found @0x4024A1, both paths into
       InitDefaults @0x4024DA] */
    auto commit_action = [&](size_t end_line) {
        ca.end_line = end_line;
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
    };

    // Every line counts, numbered as the retail walk cuts them at CR LF
    // [orig: File_ParseASCIIFile @0x53D8C7..0x53D8F5].
    const size_t walk_end = for_each_def_line(buf, file_len, [&](const io::ConfigTokens &tokens,
                                                                 const char *line, size_t line_len,
                                                                 size_t line_index) {
        const char *key = tokens.tokens[0];
        const char *v = tokens.token(1); // the first value token, "" when none
        const size_t vl = strlen(v);

        /* A `weapon` line while an entry is open, in an action block too (the
           compare comes ahead of the in-block forward), logs "weapon didn't
           have an end" and returns 1, which ends the walk: the open entry and
           every line from here on stay as they are
           [orig: WeaponDefs_ParseLineCallback @0x5436AD..0x5436D2]. */
        if (state != ST_TOP && key_is(key, "weapon")) {
            stopped = true;
            return true;
        }

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
            carry.cap = abs32_of(io::retail_atol(cap));
            DA_PUSH(out->ammo_class_carries, out->ammo_class_carries_count, carry_cap, carry);
            return false;
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
                /* The name is strncpy'd 32 bytes into the record [orig:
                   WeaponDefs_ParseLineCallback, strncpy(def+0x14, tokens[2], 0x20)
                   @0x543737], so a longer name keeps its first 32 characters. Past
                   32 retail's copy has no terminator and reads on into +0x34; the
                   port cuts there (D-ITEMDEF-10; no shipped name exceeds 21). */
                safe_copy(cw.weapon_name, 33, v, vl);
                cw.open_line = line_index;
                state = ST_WEAPON;
            }
            return false;
        }

        if (state == ST_WEAPON) {
            if (key_is(key, "action")) {
                memset(&ca, 0, sizeof(ca));
                ca_raw_cap = 0;
                safe_copy(ca.name, sizeof(ca.name), v, vl);
                ca.open_line = line_index;
                state = ST_ACTION;
                return false;
            }

            if (key_is(key, "end")) {
                cw.end_line = line_index;
                DA_PUSH(out->entries, out->count, entries_cap, cw);
                memset(&cw, 0, sizeof(cw));
                cw_raw_cap = 0; cw_act_cap = 0; cw_sight_cap = 0;
                state = ST_TOP;
                return false;
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
                   engineer=6, must be <= 6) and stores atol of the next slot at
                   the value's index, whatever the line's count (a bare class
                   reads slot 2's "", 0) [orig: handler @ 0x543ab0, the table walk
                   over tokens[2] @0x543AC4..0x543AE2 (table @ 0x830EE8), atol of
                   tokens[3] @0x543B03 -> AdmDef+0x60+value*4 @0x543B16]. The walk
                   runs seven rows over the table's five, so a class it does not
                   name reaches the CLASS_MANA string's bytes as a name pointer;
                   the port ignores the line (D-ITEMDEF-12). */
                static const struct { const char *name; int value; } kClasses[] = {
                    {"medic", 1}, {"sniper", 2}, {"gunner", 3}, {"rifleman", 5}, {"engineer", 6}};
                for (const auto &c : kClasses) {
                    if (!strutil::iequals(tokens.token(1), c.name)) continue;
                    cw.classrounds[c.value] = io::retail_atol(tokens.token(2));
                    break;
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
                cw.ammobucket = abs32_of(io::retail_atol(v));
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
                cw.ammo_class_count = abs32_of(io::retail_atol(tokens.token(2)));
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
                /* The texture must be a file (a warning and no store otherwise);
                   the second texture is read only on a line carrying it, and it
                   too must be a file [orig: WeaponDefs_ParseLineCallback
                   @0x544928 — FileSystem_FileExists(tokens[2]) @0x54493E, the
                   return @0x544953; the `cmp [esi],2; jle` @0x54496E, the NULL
                   test @0x54497A, FileExists(tokens[3]) @0x544983] */
                if (file_exists(files, tokens.token(1))) {
                    copy_token(cw.crosshair, sizeof(cw.crosshair), tokens, 1);
                    if (tokens.count > 2 && file_exists(files, tokens.token(2)))
                        copy_token(cw.crosshair_secondary, sizeof(cw.crosshair_secondary), tokens, 2);
                }
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
                /* HUDCLIPGFX: offset x/y + texture. The texture, slot 3 whatever
                   the line's count, must be a file (a warning and no store
                   otherwise) [orig: WeaponDefs_ParseLineCallback @0x54427f —
                   FileSystem_FileExists(tokens[4]) @0x544295, the return
                   @0x5442AA; atol of tokens[2] and [3] @0x5442B3 / @0x5442C8 ->
                   +0x26C / +0x270; the texture @0x5442EC]. */
                if (file_exists(files, tokens.token(3))) {
                    cw.hudclipgfx_offset[0] = io::retail_atol(tokens.token(1));
                    cw.hudclipgfx_offset[1] = io::retail_atol(tokens.token(2));
                    copy_token(cw.hudclipgfx_texture, sizeof(cw.hudclipgfx_texture), tokens, 3);
                }
                parsed = 1;
            } else if (key_is(key, "hudrndgfx")) {
                /* HUDRNDGFX: start x/y, step x/y, rounds-per-icon divisor, texture.
                   The texture, slot 6 whatever the line's count, must be a file
                   (a warning and no store otherwise) [orig:
                   WeaponDefs_ParseLineCallback @0x5442fc — FileSystem_FileExists
                   (tokens[7]) @0x544316, the return @0x544335; atol of
                   tokens[2..6] @0x544349..0x54439D -> weapon
                   +644/+648/+652/+656/+727; the texture @0x5443C1]. */
                if (file_exists(files, tokens.token(6))) {
                    cw.hudrndgfx_offset[0] = io::retail_atol(tokens.token(1));
                    cw.hudrndgfx_offset[1] = io::retail_atol(tokens.token(2));
                    cw.hudrndgfx_layout[0] = io::retail_atol(tokens.token(3));
                    cw.hudrndgfx_layout[1] = io::retail_atol(tokens.token(4));
                    cw.hudrndgfx_layout[2] = io::retail_atol(tokens.token(5));
                    copy_token(cw.hudrndgfx_texture, sizeof(cw.hudrndgfx_texture), tokens, 6);
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
                /* Slots 1..3 whatever the line's count [orig:
                   WeaponDefs_ParseLineCallback @ 0x544118..0x544169;
                   Math_ParseFixedPoint16 @ 0x6131F0] */
                for (int i = 0; i < 3; ++i) {
                    const char *value = tokens.token(1 + i);
                    cw.stability_fp16[i] = parse_fixed16_digits_n(value, strlen(value));
                }
                parsed = 1;
            } else if (key_is(key, "error")) {
                /* Six independent 16.16 parses, stored consecutively at
                   AdmDef+0xB0..+0xC4. Keep the float view for existing callers.
                   [orig: WeaponDefs_ParseLineCallback @ 0x543B21-0x543BB5;
                   Math_ParseFixedPoint16 @ 0x6131F0] */
                for (int i = 0; i < 6; ++i) {
                    const char *value = tokens.token(1 + i); // slots 1..6 whatever the count
                    cw.error[i] = parse_float_n(value, strlen(value));
                    cw.error_fp16[i] = parse_fixed16_digits_n(value, strlen(value));
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
                /* atol, then fild: the field of view is a whole number of
                   degrees [orig: weapon.def parser key 'renderfov' @ 0x54482a,
                   `call j__atol` @0x544840, `fild` @0x54484C] */
                cw.renderfov = static_cast<float>(parse_int_n(v, vl));
                parsed = 1;
            } else if (key_is(key, "scope_max_mag")) {
                /* ADS zoom magnification; the scoped FOV = 80 / clamped zoom
                   [orig: Player_ToggleWeaponScope @ 0x4df401]. Two atol'd values:
                   the max -> +0x90 and the slot's initial zoom -> +0x94, which
                   stays 0 when the row carries one value (atol of the empty
                   second token) [orig: WeaponDefs_ParseLineCallback @ 0x544f1e
                   / @ 0x544f29 and @ 0x544f33 / @ 0x544f44]. */
                cw.scope_max_mag = (float)io::retail_atol(tokens.token(1));
                cw.scope_max_mag_arg2 = io::retail_atol(tokens.token(2));
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
                   `cmp dword ptr [esi],4; jle`); the first three are read whatever
                   the count, slot 3 a stale one on a short line. Consumers: the SIGHTS `slide`
                   multiplier in HUD_DrawWeaponSightOverlays @ 0x4dcf57..0x4dcff7 and
                   Weapon_GetScopeZoomLevel @ 0x422ff3; see def.h.
                   [orig: WeaponDefs_ParseLineCallback @ 0x544e8b..0x544efd — the
                    stores @ 0x544eac / @ 0x544ec1 / @ 0x544ed9, the count gate
                    @ 0x544edf, the fourth store @ 0x544efd] */
                cw.scope_max_zero_steps = io::retail_atol(tokens.token(1));
                cw.scope_zero_step = io::retail_atol(tokens.token(2));
                cw.scope_zero_default = io::retail_atol(tokens.token(3));
                if (tokens.count > 4) cw.scope_zero_extra = io::retail_atol(tokens.token(4));
                parsed = 1;
            } else if (key_is(key, "heat_values")) {
                /* percent-per-shot / percent-per-second, each through the engine's
                   digit parser then integer-divided by 100 and by 100*62 (the logic
                   rate) — the two truncating divides are load-bearing, see def.h.
                   Slots 1 and 2 whatever the line's count.
                   [orig: @ 0x543eb7 -> +0x36C / +0x370 (the reads @0x543EC9 /
                   @0x543EF1)] */
                const char *per_shot = tokens.token(1);
                const char *decay = tokens.token(2);
                cw.heat_per_shot = parse_fixed16_digits_n(per_shot, strlen(per_shot)) / 100;
                cw.heat_decay_per_tick = parse_fixed16_digits_n(decay, strlen(decay)) / 6200;
                parsed = 1;
            } else if (key_is(key, "heat_effect")) {
                /* Effect name + the 16.16 glow threshold, slots 1 and 2 whatever
                   the line's count; any further values on the line are unread in
                   retail too. [orig: @ 0x543e36 -> +0x358 (the copy
                   @0x543E4E..0x543E63) / +0x374 (@0x543E65..0x543E7A)] */
                copy_token(cw.heat_effect, sizeof(cw.heat_effect), tokens, 1);
                const char *threshold = tokens.token(2);
                cw.heat_glow_threshold = parse_fixed16_digits_n(threshold, strlen(threshold));
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
                   whatever the line's count, a corner past it reading the slot
                   there (io::ConfigTokens::token: "" for slot 2, else what an
                   earlier line left) [orig: atol of tokens[3..6]
                   @0x544B48..0x544B6C]. The record
                   holds four 36-byte rows ahead of their count (+0x1C8..+0x257,
                   the count @+0x258), so a fifth row lands on the count itself and
                   corrupts the record; rows past the fourth are not kept
                   (D-ITEMDEF-7) [orig: the count bump @0x544B11..0x544B20, the row
                   address @0x544B2F..0x544B32]. */
                if (file_exists(files, tokens.token(1)) && cw.sights_count < 4) {
                    DefSightEntry se;
                    memset(&se, 0, sizeof(se));
                    copy_token(se.texture, sizeof(se.texture), tokens, 1);
                    se.x1 = io::retail_atol(tokens.token(2));
                    se.y1 = io::retail_atol(tokens.token(3));
                    se.x2 = io::retail_atol(tokens.token(4));
                    se.y2 = io::retail_atol(tokens.token(5));
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
                            se.slide_frames = io::retail_atol(tokens.token(8));
                        }
                    }
                    DA_PUSH(cw.sights, cw.sights_count, cw_sight_cap, se);
                }
                parsed = 1;
            }

            if (!parsed) {
                DA_PUSH_RAW(cw.raw_lines, cw.raw_lines_count, cw_raw_cap, line, line_len);
            }
            return false;
        }

        if (state == ST_ACTION) {
            if (key_is(key, "end")) {
                commit_action(line_index);
                state = ST_WEAPON;
                return false;
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
        return false;
    });

    /* An entry no `end` closed keeps the slot its `weapon` line claimed, and
       an action block left open in it the row its `action` line claimed
       (DefWeaponDef::unclosed). */
    if (state == ST_ACTION) commit_action(walk_end);
    if (state != ST_TOP) {
        cw.end_line = walk_end;
        cw.unclosed = 1;
        DA_PUSH(out->entries, out->count, entries_cap, cw);
    }
    if (stopped) {
        out->stopped = 1;
        out->stop_line = walk_end;
    }
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
