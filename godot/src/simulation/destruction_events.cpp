#include "simulation/destruction_events.h"

#include "util/axes.h"
#include "util/record_bind.h"
#include "util/string_convert.h"

using namespace godot;

// --- DestructionEffectEvent -------------------------------------------------

Ref<DestructionEffectEvent> DestructionEffectEvent::make(const String &p_effect,
		const Vector3 &p_pos, int p_family, const Vector3 &p_dir, int p_attach_net_id,
		int p_attach_bms_id, int p_attach_wire_handle, int64_t p_attach_spawn_origin,
		bool p_release, int p_bank_slot, const Vector3 &p_local_pos, bool p_section_tagged,
		bool p_positioned) {
	opennova::world::DestructionEffectEvent v;
	v.effect = p_effect.utf8().get_data();
	v.release = p_release;
	v.section_tagged = p_section_tagged;
	v.positioned = p_positioned;
	v.bank_slot = static_cast<uint8_t>(p_bank_slot);
	v.attach_local_pos = { p_local_pos.z, -p_local_pos.x, p_local_pos.y };
	v.pos = godot_to_mission<opennova::world::Vec3>(p_pos);
	v.dir = godot_to_mission<opennova::world::Vec3>(p_dir);
	v.family = static_cast<uint8_t>(p_family);
	v.attach_net_id = static_cast<uint16_t>(p_attach_net_id);
	v.attach_bms_id = p_attach_bms_id;
	v.attach_wire_handle = static_cast<uint16_t>(p_attach_wire_handle);
	v.attach_spawn_origin = static_cast<uint32_t>(p_attach_spawn_origin);
	Ref<DestructionEffectEvent> out;
	out.instantiate();
	out->assign(v);
	return out;
}

String DestructionEffectEvent::get_effect() const { return opennova::to_gd(value_.effect); }
Vector3 DestructionEffectEvent::get_pos() const { return mission_to_godot(value_.pos); }
Vector3 DestructionEffectEvent::get_dir() const { return mission_to_godot(value_.dir); }

void DestructionEffectEvent::_bind_methods() {
	ClassDB::bind_static_method("DestructionEffectEvent",
			D_METHOD("make", "effect", "pos", "family", "dir", "attach_net_id", "attach_bms_id",
					"attach_wire_handle", "attach_spawn_origin", "release", "bank_slot",
					"local_pos", "section_tagged", "positioned"),
			&DestructionEffectEvent::make, DEFVAL(0), DEFVAL(Vector3()), DEFVAL(0), DEFVAL(0),
			DEFVAL(static_cast<int>(opennova::world::EntityHandle::kInvalid)),
			DEFVAL(static_cast<int64_t>(opennova::world::kSpawnOriginNone)), DEFVAL(false),
			DEFVAL(0), DEFVAL(Vector3()), DEFVAL(false), DEFVAL(false));
	OPENNOVA_RECORD_READ_ONLY(DestructionEffectEvent, Variant::STRING, effect)
	OPENNOVA_RECORD_READ_ONLY(DestructionEffectEvent, Variant::VECTOR3, pos)
	OPENNOVA_RECORD_READ_ONLY(DestructionEffectEvent, Variant::VECTOR3, dir)
	OPENNOVA_RECORD_READ_ONLY(DestructionEffectEvent, Variant::INT, family)
	OPENNOVA_RECORD_READ_ONLY(DestructionEffectEvent, Variant::INT, bank_slot)
	OPENNOVA_RECORD_READ_ONLY(DestructionEffectEvent, Variant::BOOL, release)
	OPENNOVA_RECORD_READ_ONLY(DestructionEffectEvent, Variant::BOOL, section_tagged)
	OPENNOVA_RECORD_READ_ONLY(DestructionEffectEvent, Variant::BOOL, positioned)
	OPENNOVA_RECORD_READ_ONLY(DestructionEffectEvent, Variant::INT, attach_net_id)
	OPENNOVA_RECORD_READ_ONLY(DestructionEffectEvent, Variant::INT, attach_bms_id)
	OPENNOVA_RECORD_READ_ONLY(DestructionEffectEvent, Variant::INT, attach_wire_handle)
	OPENNOVA_RECORD_READ_ONLY(DestructionEffectEvent, Variant::INT, attach_spawn_origin)
}

// --- HuskSwapEvent ----------------------------------------------------------

Ref<HuskSwapEvent> HuskSwapEvent::make(int p_bms_id, int p_item_id, int64_t p_spawn_origin,
		int p_wire_handle, bool p_restore_intact) {
	opennova::world::HuskSwapEvent v;
	v.restore_intact = p_restore_intact;
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
			D_METHOD("make", "bms_id", "item_id", "spawn_origin", "wire_handle", "restore_intact"),
			&HuskSwapEvent::make, DEFVAL(static_cast<int64_t>(opennova::world::kSpawnOriginNone)),
			DEFVAL(static_cast<int>(opennova::world::EntityHandle::kInvalid)), DEFVAL(false));
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::BOOL, restore_intact)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::INT, net_id)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::INT, wire_handle)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::INT, bms_id)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::INT, spawn_origin)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::INT, item_id)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::INT, spawned_piece_mask)
	OPENNOVA_RECORD_READ_ONLY(HuskSwapEvent, Variant::VECTOR3, pos)
}

// --- DestructionDrain -------------------------------------------------------

void DestructionDrain::assign(const opennova::world::DestructionEvents &p_value) {
	value_.effects = p_value.effects;
	value_.sounds = p_value.sounds;
	value_.husk_swaps = p_value.husk_swaps;
	value_.death_lights = p_value.death_lights;
	value_.explosions_processed = p_value.explosions_processed;
	value_.items_destroyed = p_value.items_destroyed;
	value_.crackles = p_value.crackles;
	value_.debris_triangles = p_value.debris_triangles;
	value_.glass_points = p_value.glass_points;
}

Ref<DestructionDrain> DestructionDrain::make(const TypedArray<HuskSwapEvent> &p_husk_swaps,
		const TypedArray<DestructionEffectEvent> &p_effects, int p_debris_triangles,
		int p_glass_points, int p_crackles, const PackedVector3Array &p_death_light_positions,
		const PackedFloat32Array &p_death_light_radii) {
	Ref<DestructionDrain> out;
	out.instantiate();
	for (int i = 0; i < p_husk_swaps.size(); ++i) {
		const Ref<HuskSwapEvent> row = p_husk_swaps[i];
		if (row.is_valid()) out->value_.husk_swaps.push_back(row->value());
	}
	for (int i = 0; i < p_effects.size(); ++i) {
		const Ref<DestructionEffectEvent> row = p_effects[i];
		if (row.is_valid()) out->value_.effects.push_back(row->value());
	}
	const int64_t lights = MIN(p_death_light_positions.size(), p_death_light_radii.size());
	for (int64_t i = 0; i < lights; ++i) {
		opennova::world::DeathLightEvent light;
		light.pos = godot_to_mission<opennova::world::Vec3>(p_death_light_positions[i]);
		light.radius = p_death_light_radii[i];
		out->value_.death_lights.push_back(light);
	}
	out->value_.debris_triangles = p_debris_triangles;
	out->value_.glass_points = p_glass_points;
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

TypedArray<DestructionEffectEvent> DestructionDrain::get_effects() const {
	TypedArray<DestructionEffectEvent> out;
	for (const opennova::world::DestructionEffectEvent &e : value_.effects) {
		Ref<DestructionEffectEvent> row;
		row.instantiate();
		row->assign(e);
		out.push_back(row);
	}
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
			D_METHOD("make", "husk_swaps", "effects", "debris_triangles", "glass_points", "crackles",
					"death_light_positions", "death_light_radii"),
			&DestructionDrain::make, DEFVAL(TypedArray<DestructionEffectEvent>()), DEFVAL(0),
			DEFVAL(0), DEFVAL(0), DEFVAL(PackedVector3Array()), DEFVAL(PackedFloat32Array()));
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::INT, explosions_processed)
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::INT, items_destroyed)
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::INT, crackles)
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::INT, debris_triangles)
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::INT, glass_points)
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::PACKED_STRING_ARRAY, sound_names)
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::PACKED_VECTOR3_ARRAY, sound_positions)
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::PACKED_VECTOR3_ARRAY, death_light_positions)
	OPENNOVA_RECORD_READ_ONLY(DestructionDrain, Variant::PACKED_FLOAT32_ARRAY, death_light_radii)
	OPENNOVA_RECORD_READ_ONLY_ROWS(DestructionDrain, effects, DestructionEffectEvent)
	OPENNOVA_RECORD_READ_ONLY_ROWS(DestructionDrain, husk_swaps, HuskSwapEvent)
}
