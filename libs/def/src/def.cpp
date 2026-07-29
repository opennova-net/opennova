#include "def/def.h"

// The .def umbrella: the legacy single-entry parse plus the loadout weight and
// encumbrance helpers. One TU per file family lives beside this one (quality
// campaign W3-3).

#include "def_scan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace defscan; // the shared .def scanner, unqualified as before

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
