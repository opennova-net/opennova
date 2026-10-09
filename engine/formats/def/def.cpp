#include <formats/def/def.h>

// The .def umbrella: the loadout weight and encumbrance helpers. One TU per file family lives beside this one (quality
// campaign W3-3).

#include "def_scan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace opennova::defscan; // the shared .def scanner, unqualified as before

namespace opennova::def {

double def_loadout_weight(const DefWeaponDef *weapons, const int *ammo_counts, size_t n) {
    /* [orig: PlayerInfo_CalculateLoadoutWeight @ 0x55f1f0] per weapon:
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

double def_extra_ammo_weight(const DefWeaponDef *w, int count) {
    /* [orig: the armory grenade/extra-ammo leg @ 0x5655c9..0x56561c and the
       PLAYER_INFO sub-weapon/grenade terms in the @ 0x55f1f0 family]: the
       category-3 rows are extra-ammo legs, not parent slots — count *
       clipweight only, no weaponweight; the -1/absent sentinel takes the
       maxclips default, a chosen zero row weighs nothing. */
    if (!w) return 0.0;
    const int clips = count < 0 ? w->maxclips : count;
    return (double)(clips > 0 ? clips : 0) * (double)w->clipweight;
}

DefEncumbrance def_encumbrance_class(double weight) {
    /* [orig: PlayerInfo_UpdateWeightAndWeaponIcons @ 0x55f480] the exact
       witnessed thresholds: >= 66.6 HEAVY, >= 33.3 NORMAL, else LIGHT. */
    if (weight >= 66.6) return DEF_ENCUMBRANCE_HEAVY;
    if (weight >= 33.3) return DEF_ENCUMBRANCE_NORMAL;
    return DEF_ENCUMBRANCE_LIGHT;
}

static int ascii_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        ++a; ++b;
    }
    return *a == *b;
}

int def_subclass_weapon_index(const DefWeaponDef *weapons, size_t n,
                                         size_t parent_index) {
    /* [orig: the stricmp walk over entry+192.. @ 0x55def0 / @ 0x55e8b0 /
       @ 0x55f1f0]: same-round entries expand the parent's ammo rows; the
       first DIFFERING round_type inside the loadout_subclasses window is the
       sub-weapon. */
    if (!weapons || parent_index >= n) return -1;
    const DefWeaponDef *parent = &weapons[parent_index];
    for (int k = 1; k <= parent->loadout_subclasses; ++k) {
        const size_t cand = parent_index + (size_t)k;
        if (cand >= n) return -1;
        if (ascii_ieq(weapons[cand].round_type, parent->round_type)) continue;
        return (int)cand;
    }
    return -1;
}

int def_weapon_index_by_name(const DefWeaponDef *weapons, size_t n, const char *name) {
    /* [orig: AvatarDef_FindIndexByName @ 0x53fd80 over a table whose re-named
       entries the parse reset and refilled @ 0x5436e1..0x543722]: the last
       block of the name is the entry. */
    if (!weapons || !name || ascii_ieq(name, "null")) return -1;
    for (size_t i = n; i-- > 0;)
        if (ascii_ieq(weapons[i].weapon_name, name)) return (int)i;
    return -1;
}

} // namespace opennova::def
