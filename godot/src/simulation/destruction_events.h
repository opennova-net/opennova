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

#include <runtime/world/destruction.h>
#include <runtime/world/entity.h>

#include <cstdint>

// The destruction presentation drain (runtime/world/destruction.h; the
// world-wac-ai record §24): value wrappers over the engine event rows and the
// drain aggregate (ADR 0043 d10), Godot-space positions on read. The C++
// DestructionPresenter reads the engine DestructionEvents directly; these
// records exist for the pass's public data leg, which the present-pass tests
// and the retail parity probe author through the static make() factories.
// Identity defaults follow the engine (EntityHandle::kInvalid,
// kSpawnOriginNone) so an unattached row reads as one.

namespace godot {

// One .ptl effect roll: transient (family 0) or one of the attached wreck
// families (1 death, 2 fire, 3 other) keyed to its owner entity.
class DestructionEffectEvent : public RefCounted {
	GDCLASS(DestructionEffectEvent, RefCounted)

	opennova::world::DestructionEffectEvent value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::DestructionEffectEvent &p_value) { value_ = p_value; }
	const opennova::world::DestructionEffectEvent &value() const { return value_; }
	static Ref<DestructionEffectEvent> make(const String &p_effect, const Vector3 &p_pos,
			int p_family, const Vector3 &p_dir, int p_attach_net_id, int p_attach_bms_id,
			int p_attach_wire_handle, int64_t p_attach_spawn_origin, bool p_release,
			int p_bank_slot = 0, const Vector3 &p_local_pos = Vector3());

	String get_effect() const;
	Vector3 get_pos() const;
	Vector3 get_dir() const;
	bool get_release() const { return value_.release; }
	int get_bank_slot() const { return value_.bank_slot; }
	int get_family() const { return static_cast<int>(value_.family); }
	int get_attach_net_id() const { return static_cast<int>(value_.attach_net_id); }
	int get_attach_bms_id() const { return value_.attach_bms_id; }
	int get_attach_wire_handle() const { return static_cast<int>(value_.attach_wire_handle); }
	int64_t get_attach_spawn_origin() const {
		return static_cast<int64_t>(value_.attach_spawn_origin);
	}
};

// One husked entity: the present pass swaps its render model to the husk.
class HuskSwapEvent : public RefCounted {
	GDCLASS(HuskSwapEvent, RefCounted)

	opennova::world::HuskSwapEvent value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::HuskSwapEvent &p_value) { value_ = p_value; }
	const opennova::world::HuskSwapEvent &value() const { return value_; }
	static Ref<HuskSwapEvent> make(int p_bms_id, int p_item_id, int64_t p_spawn_origin,
			int p_wire_handle, bool p_restore_intact);

	bool get_restore_intact() const { return value_.restore_intact; }
	int get_net_id() const { return static_cast<int>(value_.net_id); }
	int get_wire_handle() const { return static_cast<int>(value_.wire_handle); }
	int get_bms_id() const { return value_.bms_id; }
	int64_t get_spawn_origin() const { return static_cast<int64_t>(value_.spawn_origin); }
	int get_item_id() const { return value_.item_id; }
	int64_t get_spawned_piece_mask() const { return static_cast<int64_t>(value_.spawned_piece_mask); }
	Vector3 get_pos() const;
};

// One drain: every event since the last drain plus the diagnostic counters
// (probes assert the legs actually ran), over the engine's DestructionEvents.
// The destruction sound rolls and the death explosion flashes read as
// parallel packed columns; the presenter plays them straight into
// MissionAudio.fire_soundset and EffectLightDirector.on_death_light.
class DestructionDrain : public RefCounted {
	GDCLASS(DestructionDrain, RefCounted)

	opennova::world::DestructionEvents value_;

protected:
	static void _bind_methods();

public:
	void assign(const opennova::world::DestructionEvents &p_value);
	const opennova::world::DestructionEvents &value() const { return value_; }
	static Ref<DestructionDrain> make(const TypedArray<HuskSwapEvent> &p_husk_swaps,
			const TypedArray<DestructionEffectEvent> &p_effects, int p_debris_triangles,
			int p_glass_points, int p_crackles, const PackedVector3Array &p_death_light_positions,
			const PackedFloat32Array &p_death_light_radii);

	int get_explosions_processed() const { return value_.explosions_processed; }
	int get_items_destroyed() const { return value_.items_destroyed; }
	int get_crackles() const { return value_.crackles; }
	int get_debris_triangles() const { return value_.debris_triangles; }
	int get_glass_points() const { return value_.glass_points; }
	PackedStringArray get_sound_names() const;
	PackedVector3Array get_sound_positions() const;
	PackedVector3Array get_death_light_positions() const;
	PackedFloat32Array get_death_light_radii() const;
	TypedArray<DestructionEffectEvent> get_effects() const;
	TypedArray<HuskSwapEvent> get_husk_swaps() const;
};

} // namespace godot
