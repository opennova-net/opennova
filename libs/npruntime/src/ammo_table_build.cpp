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
		e.spread_error_fp16 = d.error_fp16;
		// The parser's atol results are assigned directly into three bytes, so values
		// outside 0..255 narrow modulo 256. [orig: AmmoDef_ParseProperty @0x40A2D0]
		for (int stance = 0; stance < 3; ++stance)
			e.recoil[stance] = static_cast<uint8_t>(d.recoil[stance]);
		e.drag = static_cast<float>(d.drag_fp16) / 65536.0f;
		e.drag_fp16 = d.drag_fp16;
		e.bullet_radius = static_cast<float>(d.bullet_radius_fp16) / 65536.0f;
		e.bullet_radius_fp16 = d.bullet_radius_fp16;
		e.spread_count = d.spread_count;
		e.kztype = d.kztype;
		e.kz_damage = d.kz_damage;
		e.weight_in_grains = d.weight_in_grains;
		e.min_stable_velocity = d.min_stable_velocity;
		e.tumble_error_fp16 = d.tumble_error_fp16;
		e.min_damage = d.min_damage;
		e.max_damage = d.max_damage;
		e.penetration_impact = d.penetration_impact;
		// Kill-zone blast geometry + the blast armor gate (the explosion queue's
		// consumers; docs/world/world-wac-ai-re.md §24).
		e.penetration_kz = d.penetration_kz;
		e.kz_minradius = static_cast<float>(d.kz_minradius_fp16) / 65536.0f;
		e.kz_maxradius = static_cast<float>(d.kz_maxradius_fp16) / 65536.0f;
		e.kz_pieslice_bam = d.kz_pieslice_bam;
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
		e.ai_launch_set = d.ai_launch;
		e.ai_launch_effect = d.ai_launcheffect;
		e.mf_light = d.mf_light;
		e.mf_light_value = d.mf_light_value;
		e.tracer_type_friendly = d.tracer_type_friendly;
		e.tracer_type_enemy = d.tracer_type_enemy;
		e.tracer_item_friendly = d.frndly_trcr_type_id;
		e.tracer_item_enemy = d.foe_trcr_type_id;
		e.light_move_radius = static_cast<float>(d.light_move_radius_fp16) / 65536.0f;
		e.light_move_color = static_cast<uint32_t>(d.light_move_color);
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
