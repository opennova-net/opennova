#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/world/entity.h>

#include <cstdint>

// The destruction presentation drain (runtime/world/destruction.h; the
// world-wac-ai record §24): one typed row per engine event, Godot-space
// positions, plus the drain aggregate with its diagnostic counters. Read-write
// with static make() factories so the present-pass tests author rows; produced
// by Simulation::drain_destruction_events. Identity defaults follow the engine
// (EntityHandle::kInvalid, kSpawnOriginNone) so an unattached row reads as one.

#define DESTRUCTION_ACCESSORS(m_type, m_name, m_default)      \
	m_type get_##m_name() const { return m_name##_; }        \
	void set_##m_name(m_type p_value) { m_name##_ = p_value; }
#define DESTRUCTION_MEMBER(m_type, m_name, m_default) m_type m_name##_ = m_default;

namespace godot {

// One .ptl effect roll: transient (family 0) or one of the attached wreck
// families (1 death, 2 fire, 3 other) keyed to its owner entity.
#define DESTRUCTION_EFFECT_EVENT_FIELDS(X)                                            \
	X(String, effect, String())                                                       \
	X(Vector3, pos, Vector3())                                                        \
	X(Vector3, dir, Vector3())                                                        \
	X(int, family, 0)                                                                 \
	X(int, attach_net_id, 0)                                                          \
	X(int, attach_bms_id, 0)                                                          \
	X(int, attach_wire_handle, opennova::world::EntityHandle::kInvalid)               \
	X(int64_t, attach_spawn_origin, static_cast<int64_t>(opennova::world::kSpawnOriginNone))

class DestructionEffectEvent : public RefCounted {
	GDCLASS(DestructionEffectEvent, RefCounted)

public:
	DESTRUCTION_EFFECT_EVENT_FIELDS(DESTRUCTION_ACCESSORS)
	static Ref<DestructionEffectEvent> make(const String &p_effect, const Vector3 &p_pos,
			int p_family, const Vector3 &p_dir, int p_attach_net_id, int p_attach_bms_id,
			int p_attach_wire_handle, int64_t p_attach_spawn_origin);

protected:
	static void _bind_methods();

private:
	DESTRUCTION_EFFECT_EVENT_FIELDS(DESTRUCTION_MEMBER)
};

// One destruction sound roll at its world position.
#define DESTRUCTION_SOUND_EVENT_FIELDS(X) \
	X(String, sound, String())            \
	X(Vector3, pos, Vector3())

class DestructionSoundEvent : public RefCounted {
	GDCLASS(DestructionSoundEvent, RefCounted)

public:
	DESTRUCTION_SOUND_EVENT_FIELDS(DESTRUCTION_ACCESSORS)
	static Ref<DestructionSoundEvent> make(const String &p_sound, const Vector3 &p_pos);

protected:
	static void _bind_methods();

private:
	DESTRUCTION_SOUND_EVENT_FIELDS(DESTRUCTION_MEMBER)
};

// One husked entity: the present pass swaps its render model to the husk.
#define HUSK_SWAP_EVENT_FIELDS(X)                                                 \
	X(int, net_id, 0)                                                             \
	X(int, wire_handle, opennova::world::EntityHandle::kInvalid)                  \
	X(int, bms_id, 0)                                                             \
	X(int64_t, spawn_origin, static_cast<int64_t>(opennova::world::kSpawnOriginNone)) \
	X(int, item_id, 0)                                                            \
	X(int64_t, spawned_piece_mask, 0)                                             \
	X(Vector3, pos, Vector3())

class HuskSwapEvent : public RefCounted {
	GDCLASS(HuskSwapEvent, RefCounted)

public:
	HUSK_SWAP_EVENT_FIELDS(DESTRUCTION_ACCESSORS)
	static Ref<HuskSwapEvent> make(int p_bms_id, int p_item_id, int64_t p_spawn_origin,
			int p_wire_handle);

protected:
	static void _bind_methods();

private:
	HUSK_SWAP_EVENT_FIELDS(DESTRUCTION_MEMBER)
};

// The death explosion flash for the presenter's light pool.
#define DEATH_LIGHT_EVENT_FIELDS(X) \
	X(Vector3, pos, Vector3())      \
	X(float, radius, 0.0f)

class DeathLightEvent : public RefCounted {
	GDCLASS(DeathLightEvent, RefCounted)

public:
	DEATH_LIGHT_EVENT_FIELDS(DESTRUCTION_ACCESSORS)
	static Ref<DeathLightEvent> make(const Vector3 &p_pos, float p_radius);

protected:
	static void _bind_methods();

private:
	DEATH_LIGHT_EVENT_FIELDS(DESTRUCTION_MEMBER)
};

// One drain: every event since the last drain plus the diagnostic counters
// (probes assert the legs actually ran).
#define DESTRUCTION_DRAIN_COUNTERS(X) \
	X(int, explosions_processed, 0)   \
	X(int, items_destroyed, 0)        \
	X(int, crackles, 0)               \
	X(int, debris_triangles, 0)       \
	X(int, glass_points, 0)

class DestructionDrain : public RefCounted {
	GDCLASS(DestructionDrain, RefCounted)

public:
	DESTRUCTION_DRAIN_COUNTERS(DESTRUCTION_ACCESSORS)
	TypedArray<DestructionEffectEvent> get_effects() const { return effects_; }
	void set_effects(const TypedArray<DestructionEffectEvent> &p_value) { effects_ = p_value; }
	TypedArray<DestructionSoundEvent> get_sounds() const { return sounds_; }
	void set_sounds(const TypedArray<DestructionSoundEvent> &p_value) { sounds_ = p_value; }
	TypedArray<HuskSwapEvent> get_husk_swaps() const { return husk_swaps_; }
	void set_husk_swaps(const TypedArray<HuskSwapEvent> &p_value) { husk_swaps_ = p_value; }
	TypedArray<DeathLightEvent> get_death_lights() const { return death_lights_; }
	void set_death_lights(const TypedArray<DeathLightEvent> &p_value) { death_lights_ = p_value; }
	void add_effect(const Ref<DestructionEffectEvent> &p_event) { effects_.push_back(p_event); }
	void add_sound(const Ref<DestructionSoundEvent> &p_event) { sounds_.push_back(p_event); }
	void add_husk_swap(const Ref<HuskSwapEvent> &p_event) { husk_swaps_.push_back(p_event); }
	void add_death_light(const Ref<DeathLightEvent> &p_event) { death_lights_.push_back(p_event); }
	static Ref<DestructionDrain> make(const TypedArray<HuskSwapEvent> &p_husk_swaps,
			const TypedArray<DestructionEffectEvent> &p_effects,
			const TypedArray<DestructionSoundEvent> &p_sounds,
			const TypedArray<DeathLightEvent> &p_death_lights, int p_debris_triangles,
			int p_glass_points, int p_crackles);

protected:
	static void _bind_methods();

private:
	DESTRUCTION_DRAIN_COUNTERS(DESTRUCTION_MEMBER)
	TypedArray<DestructionEffectEvent> effects_;
	TypedArray<DestructionSoundEvent> sounds_;
	TypedArray<HuskSwapEvent> husk_swaps_;
	TypedArray<DeathLightEvent> death_lights_;
};

} // namespace godot

#undef DESTRUCTION_ACCESSORS
#undef DESTRUCTION_MEMBER
