#include "simulation/destruction_events.h"

#include "util/axes.h"
#include "util/record_bind.h"
#include "util/string_convert.h"

using namespace godot;

// --- HuskSwapEvent ----------------------------------------------------------

Ref<HuskSwapEvent> HuskSwapEvent::make(int p_bms_id, int p_item_id, int64_t p_spawn_origin,
		int p_wire_handle, bool p_restore_intact, bool p_husk_final, int p_final_overlay) {
	opennova::world::HuskSwapEvent v;
	v.restore_intact = p_restore_intact;
	v.husk_final = p_husk_final;
	v.final_overlay = static_cast<opennova::world::HuskSwapEvent::Overlay>(
			p_final_overlay >= 0 && p_final_overlay <= 2 ? p_final_overlay : 0);
	v.bms_id = p_bms_id;
	v.item_id = p_item_id;
	v.spawn_origin = static_cast<uint32_t>(p_spawn_origin);
	v.wire_handle = static_cast<uint16_t>(p_wire_handle);
	Ref<HuskSwapEvent> out;
	out.instantiate();
	out->assign(v);
	return out;
}

Vector3 HuskSwapEvent::get_pos() const { return mission_to_godot(value_.pos); }

void HuskSwapEvent::_bind_methods() {
	ClassDB::bind_static_method("HuskSwapEvent",
			D_METHOD("make", "bms_id", "item_id", "spawn_origin", "wire_handle", "restore_intact",
					"husk_final", "final_overlay"),
			&HuskSwapEvent::make, DEFVAL(static_cast<int64_t>(opennova::world::kSpawnOriginNone)),
			DEFVAL(static_cast<int>(opennova::world::EntityHandle::kInvalid)), DEFVAL(false),
			DEFVAL(false), DEFVAL(0));
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::BOOL, restore_intact)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::INT, net_id)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::INT, wire_handle)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::INT, bms_id)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::INT, spawn_origin)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::INT, item_id)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::INT, spawned_piece_mask)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::VECTOR3, pos)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::BOOL, husk_final)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::INT, final_overlay)
}

// --- DestructionDrain -------------------------------------------------------

void DestructionDrain::assign(const opennova::world::DestructionEvents &p_value) {
	value_.sounds = p_value.sounds;
	value_.husk_swaps = p_value.husk_swaps;
	value_.death_lights = p_value.death_lights;
	value_.explosions_processed = p_value.explosions_processed;
	value_.items_destroyed = p_value.items_destroyed;
	value_.crackles = p_value.crackles;
	value_.debris_triangles = p_value.debris_triangles;
}

Ref<DestructionDrain> DestructionDrain::make(const TypedArray<HuskSwapEvent> &p_husk_swaps,
		int p_debris_triangles, int p_crackles, const PackedVector3Array &p_death_light_positions,
		const PackedFloat32Array &p_death_light_radii) {
	Ref<DestructionDrain> out;
	out.instantiate();
	for (int i = 0; i < p_husk_swaps.size(); ++i) {
		const Ref<HuskSwapEvent> row = p_husk_swaps[i];
		if (row.is_valid()) out->value_.husk_swaps.push_back(row->value());
	}
	const int64_t lights = MIN(p_death_light_positions.size(), p_death_light_radii.size());
	for (int64_t i = 0; i < lights; ++i) {
		opennova::world::DeathLightEvent light;
		light.pos = godot_to_mission<opennova::world::Vec3>(p_death_light_positions[i]);
		light.radius = p_death_light_radii[i];
		out->value_.death_lights.push_back(light);
	}
	out->value_.debris_triangles = p_debris_triangles;
	out->value_.crackles = p_crackles;
	return out;
}

PackedStringArray DestructionDrain::get_sound_names() const {
	PackedStringArray out;
	for (const opennova::world::DestructionSoundEvent &s : value_.sounds)
		out.push_back(opennova::to_gd(s.sound));
	return out;
}

PackedVector3Array DestructionDrain::get_sound_positions() const {
	PackedVector3Array out;
	for (const opennova::world::DestructionSoundEvent &s : value_.sounds)
		out.push_back(mission_to_godot(s.pos));
	return out;
}

PackedVector3Array DestructionDrain::get_death_light_positions() const {
	PackedVector3Array out;
	for (const opennova::world::DeathLightEvent &l : value_.death_lights)
		out.push_back(mission_to_godot(l.pos));
	return out;
}

PackedFloat32Array DestructionDrain::get_death_light_radii() const {
	PackedFloat32Array out;
	for (const opennova::world::DeathLightEvent &l : value_.death_lights) out.push_back(l.radius);
	return out;
}

TypedArray<HuskSwapEvent> DestructionDrain::get_husk_swaps() const {
	TypedArray<HuskSwapEvent> out;
	for (const opennova::world::HuskSwapEvent &h : value_.husk_swaps) {
		Ref<HuskSwapEvent> row;
		row.instantiate();
		row->assign(h);
		out.push_back(row);
	}
	return out;
}

void DestructionDrain::_bind_methods() {
	ClassDB::bind_static_method("DestructionDrain",
			D_METHOD("make", "husk_swaps", "debris_triangles", "crackles",
					"death_light_positions", "death_light_radii"),
			&DestructionDrain::make, DEFVAL(0), DEFVAL(0), DEFVAL(PackedVector3Array()),
			DEFVAL(PackedFloat32Array()));
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::INT, explosions_processed)
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::INT, items_destroyed)
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::INT, crackles)
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::INT, debris_triangles)
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::PACKED_STRING_ARRAY, sound_names)
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::PACKED_VECTOR3_ARRAY, sound_positions)
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::PACKED_VECTOR3_ARRAY, death_light_positions)
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::PACKED_FLOAT32_ARRAY, death_light_radii)
	OPENNOVA_RECORD_READ_ONLY_ROWS(DestructionDrain, husk_swaps, HuskSwapEvent)
}
