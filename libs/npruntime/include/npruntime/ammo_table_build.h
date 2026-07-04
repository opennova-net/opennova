// ammo.def -> world::AmmoTable, plus the weapon.def round_type -> ammo index resolve —
// the load-time binding the fire pipeline consumes (the original stores the resolved
// ammo index pair at adm+84, read by RoundData_AddRound as adm dword 21).
// [orig: AmmoDef_LoadAll @ 0x40B0B0 / AmmoDef_ParseProperty @ 0x40A2D0 /
// AmmoDef_LookupByName @ 0x409870; docs/net/novaworld-net-re.md §5.60]
#pragma once

#include <def/def.h>
#include <world/ammo_table.h>
#include <world/weapon_table.h>

namespace opennova::np {

// Dense file-order table (index 0 = the file's own null first block, e.g. AT_NULL).
world::AmmoTable build_ammo_table(const DefAmmoFile &ammo);

// Bind every weapon entry's `round_type` name to its AmmoTable index (-1 when absent or
// unresolvable — the original warns and leaves the pair null).
void resolve_weapon_round_types(world::WeaponTable &weapons, const world::AmmoTable &ammo);

} // namespace opennova::np
