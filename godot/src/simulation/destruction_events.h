#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <runtime/world/entity.h>

#include <cstdint>

// The destruction presentation drain (runtime/world/destruction.h; the
// world-wac-ai record §24): one typed row per engine husk swap / effect roll,
// Godot-space positions, the sound and death-light legs as parallel packed
// columns, plus the drain aggregate with its diagnostic counters. Read-write
// with static make() factories so the present-pass tests author rows;
// produced by Simulation::drain_destruction_events and consumed by
// DestructionPresenter. Identity defaults follow the engine
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

// One drain: every event since the last drain plus the diagnostic counters
// (probes assert the legs actually ran). The destruction sound rolls
// (sound_names / sound_positions) and the death explosion flashes for the
// presenter's light pool (death_light_positions / death_light_radii; the
// engine's world/destruction.h DeathLightEvent carries the witness) are
// parallel columns: the presenter plays them straight into
// MissionAudio.fire_soundset and EffectLightDirector.on_death_light.
#define DESTRUCTION_DRAIN_COUNTERS(X) \
	X(int, explosions_processed, 0)   \
	X(int, items_destroyed, 0)        \
	X(int, crackles, 0)               \
	X(int, debris_triangles, 0)       \
	X(int, glass_points, 0)

#define DESTRUCTION_DRAIN_COLUMNS(X)                                       \
	X(PackedStringArray, sound_names, PackedStringArray())                 \
	X(PackedVector3Array, sound_positions, PackedVector3Array())           \
	X(PackedVector3Array, death_light_positions, PackedVector3Array())     \
	X(PackedFloat32Array, death_light_radii, PackedFloat32Array())

class DestructionDrain : public RefCounted {
	GDCLASS(DestructionDrain, RefCounted)

public:
	DESTRUCTION_DRAIN_COUNTERS(DESTRUCTION_ACCESSORS)
	DESTRUCTION_DRAIN_COLUMNS(DESTRUCTION_ACCESSORS)
	TypedArray<DestructionEffectEvent> get_effects() const { return effects_; }
	void set_effects(const TypedArray<DestructionEffectEvent> &p_value) { effects_ = p_value; }
	TypedArray<HuskSwapEvent> get_husk_swaps() const { return husk_swaps_; }
	void set_husk_swaps(const TypedArray<HuskSwapEvent> &p_value) { husk_swaps_ = p_value; }
	void add_effect(const Ref<DestructionEffectEvent> &p_event) { effects_.push_back(p_event); }
	void add_husk_swap(const Ref<HuskSwapEvent> &p_event) { husk_swaps_.push_back(p_event); }
	void add_sound(const String &p_name, const Vector3 &p_pos);
	void add_death_light(const Vector3 &p_pos, float p_radius);
	static Ref<DestructionDrain> make(const TypedArray<HuskSwapEvent> &p_husk_swaps,
			const TypedArray<DestructionEffectEvent> &p_effects, int p_debris_triangles,
			int p_glass_points, int p_crackles);

protected:
	static void _bind_methods();

private:
	DESTRUCTION_DRAIN_COUNTERS(DESTRUCTION_MEMBER)
	DESTRUCTION_DRAIN_COLUMNS(DESTRUCTION_MEMBER)
	TypedArray<DestructionEffectEvent> effects_;
	TypedArray<HuskSwapEvent> husk_swaps_;
};

} // namespace godot

#undef DESTRUCTION_ACCESSORS
#undef DESTRUCTION_MEMBER
