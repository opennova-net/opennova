// See ammo_table_build.h. docs/net/novaworld-net-re.md §5.60.
#include "npruntime/ammo_table_build.h"

namespace opennova::np {

world::AmmoTable build_ammo_table(const DefAmmoFile &ammo) {
	world::AmmoTable table;
	table.entries.reserve(ammo.count);
	for (size_t i = 0; i < ammo.count; ++i) {
		const DefAmmoDef &d = ammo.entries[i];
		world::AmmoTableEntry e;
		e.name = d.name;
		e.valid = true;
		e.flags = d.flags;
		e.velocity = d.velocity;
		e.max_age_ticks = d.max_age_ticks;
		e.arm_age_ticks = d.arm_age_ticks;
		// 16.16 file values -> float mission units (the sim works in floats; the parse
		// kept the original fixed encoding).
		e.spread_error = static_cast<float>(d.error_fp16) / 65536.0f;
		e.drag = static_cast<float>(d.drag_fp16) / 65536.0f;
		e.bullet_radius = static_cast<float>(d.bullet_radius_fp16) / 65536.0f;
		e.spread_count = d.spread_count;
		e.kztype = d.kztype;
		e.kz_damage = d.kz_damage;
		e.weight_in_grains = d.weight_in_grains;
		e.min_damage = d.min_damage;
		e.max_damage = d.max_damage;
		e.penetration_impact = d.penetration_impact;
		e.tracer_rate = d.tracer_rate;
		e.notarmmed_ammo = d.notarmmed_ammo;
		table.entries.push_back(std::move(e));
	}
	return table;
}

void resolve_weapon_round_types(world::WeaponTable &weapons, const world::AmmoTable &ammo) {
	for (world::WeaponTableEntry &e : weapons.entries) {
		if (!e.valid || e.round_type.empty()) {
			e.ammo_index = -1;
			continue;
		}
		e.ammo_index = static_cast<int16_t>(ammo.index_of(e.round_type.c_str()));
	}
}

} // namespace opennova::np
