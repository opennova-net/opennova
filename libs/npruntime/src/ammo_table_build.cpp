// See ammo_table_build.h. docs/net/novaworld-net-re.md §5.60.
#include "npruntime/ammo_table_build.h"

#include <io/strutil.h>

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
		// Bake the authored effects_table rows into the canonical tag slots [orig:
		// AmmoDef_ParseProperty effects_table stage @ 0x40a46a -> AmmoDef_InitEffectsTable
		// @ 0x409f20]: tag matched case-insensitively from index 1, first row wins
		// ('redefining the effect' @ 0x40a502), 'none' columns stay empty, the count
		// column is discarded [orig: @ 0x40a587], unknown tags dropped (@ 0x40a49f).
		bool tag_seen[world::kImpactEffectTagCount] = {};
		for (size_t row = 0; row < d.effects_table_count; ++row) {
			const DefEffectTableEntry &src = d.effects_table[row];
			const int tag = world::impact_effect_tag_index(src.surface_type);
			if (tag < 0 || tag_seen[tag]) continue;
			tag_seen[tag] = true;
			world::AmmoImpactEffectRow &dst = e.impact_effects[tag];
			if (!opennova::strutil::iequals(src.hit_effect, "none")) dst.effect = src.hit_effect;
			if (!opennova::strutil::iequals(src.impact_sound, "none")) dst.sound = src.impact_sound;
		}
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
