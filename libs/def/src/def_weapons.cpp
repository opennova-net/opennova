#include "def/def.h"

// Split out of def.cpp (quality campaign W3-3). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// WEAPONS.DEF: one record per weapon.

#include "def_scan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace defscan; // the shared .def scanner, unqualified as before

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
                /* [orig: WeaponDefs_ParseLineCallback store @ 0x54410D;
                   Math_ParseFixedPoint16 @ 0x6131F0] */
                cw.weaponweight_fp16 = parse_fixed16_digits_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "clipweight", 10)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 10, &vl);
                cw.clipweight = parse_float_n(v, vl);
                /* [orig: WeaponDefs_ParseLineCallback store @ 0x5440DB;
                   Math_ParseFixedPoint16 @ 0x6131F0] */
                cw.clipweight_fp16 = parse_fixed16_digits_n(v, vl);
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
                /* Six independent 16.16 parses, stored consecutively at
                   AdmDef+0xB0..+0xC4. Keep the float view for existing callers.
                   [orig: WeaponDefs_ParseLineCallback @ 0x543B21-0x543BB5;
                   Math_ParseFixedPoint16 @ 0x6131F0] */
                Token values[6];
                int count = split_values(v, vl, values, 6);
                for (int i = 0; i < count; ++i) {
                    cw.error[i] = parse_float_n(values[i].s, values[i].len);
                    cw.error_fp16[i] = parse_fixed16_digits_n(values[i].s, values[i].len);
                }
                parsed = 1;
            } else if (lower_match_key(lower, ll, "error_hiptheta", 14)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 14, &vl);
                /* [orig: WeaponDefs_ParseLineCallback @ 0x543BC0, store +0xCC
                   @ 0x543BE7; Math_ParseFixedPoint16 @ 0x6131F0] */
                cw.error_hip_theta_fp16 = parse_fixed16_digits_n(v, vl);
                parsed = 1;
            } else if (lower_match_key(lower, ll, "error_uptheta", 13)) {
                size_t vl; const char *v = consume_value_span(trimmed, tlen, 13, &vl);
                /* [orig: WeaponDefs_ParseLineCallback @ 0x543BF2, store +0xD0
                   @ 0x543C19; Math_ParseFixedPoint16 @ 0x6131F0] */
                cw.error_up_theta_fp16 = parse_fixed16_digits_n(v, vl);
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
