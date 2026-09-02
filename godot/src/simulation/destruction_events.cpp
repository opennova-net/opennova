#include "simulation/destruction_events.h"

using namespace godot;

namespace {

template <typename T>
constexpr Variant::Type variant_type_of();
template <>
constexpr Variant::Type variant_type_of<int>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<int64_t>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<float>() { return Variant::FLOAT; }
template <>
constexpr Variant::Type variant_type_of<String>() { return Variant::STRING; }
template <>
constexpr Variant::Type variant_type_of<Vector3>() { return Variant::VECTOR3; }

} // namespace

#define DESTRUCTION_BIND_FIELD(m_type, m_name, m_default)                                          \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &self_type::get_##m_name);                      \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &self_type::set_##m_name);             \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);

Ref<DestructionEffectEvent> DestructionEffectEvent::make(const String &p_effect, const Vector3 &p_pos,
		int p_family, const Vector3 &p_dir, int p_attach_net_id, int p_attach_bms_id,
		int p_attach_wire_handle, int64_t p_attach_spawn_origin) {
	Ref<DestructionEffectEvent> out;
	out.instantiate();
	out->effect_ = p_effect;
	out->pos_ = p_pos;
	out->family_ = p_family;
	out->dir_ = p_dir;
	out->attach_net_id_ = p_attach_net_id;
	out->attach_bms_id_ = p_attach_bms_id;
	out->attach_wire_handle_ = p_attach_wire_handle;
	out->attach_spawn_origin_ = p_attach_spawn_origin;
	return out;
}

void DestructionEffectEvent::_bind_methods() {
	DESTRUCTION_EFFECT_EVENT_FIELDS(DESTRUCTION_BIND_FIELD)
	ClassDB::bind_static_method("DestructionEffectEvent",
			D_METHOD("make", "effect", "pos", "family", "dir", "attach_net_id", "attach_bms_id",
					"attach_wire_handle", "attach_spawn_origin"),
			&DestructionEffectEvent::make, DEFVAL(0), DEFVAL(Vector3()), DEFVAL(0), DEFVAL(0),
			DEFVAL(static_cast<int>(opennova::world::EntityHandle::kInvalid)),
			DEFVAL(static_cast<int64_t>(opennova::world::kSpawnOriginNone)));
}

Ref<DestructionSoundEvent> DestructionSoundEvent::make(const String &p_sound, const Vector3 &p_pos) {
	Ref<DestructionSoundEvent> out;
	out.instantiate();
	out->sound_ = p_sound;
	out->pos_ = p_pos;
	return out;
}

void DestructionSoundEvent::_bind_methods() {
	DESTRUCTION_SOUND_EVENT_FIELDS(DESTRUCTION_BIND_FIELD)
	ClassDB::bind_static_method("DestructionSoundEvent", D_METHOD("make", "sound", "pos"),
			&DestructionSoundEvent::make);
}

Ref<HuskSwapEvent> HuskSwapEvent::make(int p_bms_id, int p_item_id, int64_t p_spawn_origin,
		int p_wire_handle) {
	Ref<HuskSwapEvent> out;
	out.instantiate();
	out->bms_id_ = p_bms_id;
	out->item_id_ = p_item_id;
	out->spawn_origin_ = p_spawn_origin;
	out->wire_handle_ = p_wire_handle;
	return out;
}

void HuskSwapEvent::_bind_methods() {
	HUSK_SWAP_EVENT_FIELDS(DESTRUCTION_BIND_FIELD)
	ClassDB::bind_static_method("HuskSwapEvent",
			D_METHOD("make", "bms_id", "item_id", "spawn_origin", "wire_handle"), &HuskSwapEvent::make,
			DEFVAL(static_cast<int64_t>(opennova::world::kSpawnOriginNone)),
			DEFVAL(static_cast<int>(opennova::world::EntityHandle::kInvalid)));
}

Ref<DeathLightEvent> DeathLightEvent::make(const Vector3 &p_pos, float p_radius) {
	Ref<DeathLightEvent> out;
	out.instantiate();
	out->pos_ = p_pos;
	out->radius_ = p_radius;
	return out;
}

void DeathLightEvent::_bind_methods() {
	DEATH_LIGHT_EVENT_FIELDS(DESTRUCTION_BIND_FIELD)
	ClassDB::bind_static_method("DeathLightEvent", D_METHOD("make", "pos", "radius"),
			&DeathLightEvent::make);
}

Ref<DestructionDrain> DestructionDrain::make(const TypedArray<HuskSwapEvent> &p_husk_swaps,
		const TypedArray<DestructionEffectEvent> &p_effects,
		const TypedArray<DestructionSoundEvent> &p_sounds,
		const TypedArray<DeathLightEvent> &p_death_lights, int p_debris_triangles,
		int p_glass_points, int p_crackles) {
	Ref<DestructionDrain> out;
	out.instantiate();
	out->husk_swaps_ = p_husk_swaps;
	out->effects_ = p_effects;
	out->sounds_ = p_sounds;
	out->death_lights_ = p_death_lights;
	out->debris_triangles_ = p_debris_triangles;
	out->glass_points_ = p_glass_points;
	out->crackles_ = p_crackles;
	return out;
}

void DestructionDrain::_bind_methods() {
	DESTRUCTION_DRAIN_COUNTERS(DESTRUCTION_BIND_FIELD)
#define DESTRUCTION_DRAIN_ROWS(m_name, m_class)                                                    \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &DestructionDrain::get_##m_name);              \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &DestructionDrain::set_##m_name);     \
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, #m_name, PROPERTY_HINT_ARRAY_TYPE, #m_class),       \
			"set_" #m_name, "get_" #m_name);
	DESTRUCTION_DRAIN_ROWS(effects, DestructionEffectEvent)
	DESTRUCTION_DRAIN_ROWS(sounds, DestructionSoundEvent)
	DESTRUCTION_DRAIN_ROWS(husk_swaps, HuskSwapEvent)
	DESTRUCTION_DRAIN_ROWS(death_lights, DeathLightEvent)
#undef DESTRUCTION_DRAIN_ROWS
	ClassDB::bind_method(D_METHOD("add_effect", "event"), &DestructionDrain::add_effect);
	ClassDB::bind_method(D_METHOD("add_sound", "event"), &DestructionDrain::add_sound);
	ClassDB::bind_method(D_METHOD("add_husk_swap", "event"), &DestructionDrain::add_husk_swap);
	ClassDB::bind_method(D_METHOD("add_death_light", "event"), &DestructionDrain::add_death_light);
	ClassDB::bind_static_method("DestructionDrain",
			D_METHOD("make", "husk_swaps", "effects", "sounds", "death_lights", "debris_triangles",
					"glass_points", "crackles"),
			&DestructionDrain::make, DEFVAL(TypedArray<DestructionEffectEvent>()),
			DEFVAL(TypedArray<DestructionSoundEvent>()), DEFVAL(TypedArray<DeathLightEvent>()),
			DEFVAL(0), DEFVAL(0), DEFVAL(0));
}

#undef DESTRUCTION_BIND_FIELD
