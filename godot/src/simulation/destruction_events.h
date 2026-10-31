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
// author through the static make() factories. The effect rows are the engine's
// own (world::DestructionEmitters spawns them in the world's scene): no drain
// carries them.
// Identity defaults follow the engine (EntityHandle::kInvalid,
// kSpawnOriginNone) so an unattached row reads as one.

namespace godot {

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
			int p_wire_handle, bool p_restore_intact, bool p_husk_final, int p_final_overlay);

	bool get_restore_intact() const { return value_.restore_intact; }
	int get_net_id() const { return static_cast<int>(value_.net_id); }
	int get_wire_handle() const { return static_cast<int>(value_.wire_handle); }
	int get_bms_id() const { return value_.bms_id; }
	int64_t get_spawn_origin() const { return static_cast<int64_t>(value_.spawn_origin); }
	int get_item_id() const { return value_.item_id; }
	int64_t get_spawned_piece_mask() const { return static_cast<int64_t>(value_.spawned_piece_mask); }
	Vector3 get_pos() const;
	bool get_husk_final() const { return value_.husk_final; }
	// 0 none, 1 show, 2 hide (opennova::world::HuskSwapEvent::Overlay).
	int get_final_overlay() const { return static_cast<int>(value_.final_overlay); }
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
			int p_debris_triangles, int p_crackles,
			const PackedVector3Array &p_death_light_positions,
			const PackedFloat32Array &p_death_light_radii);

	int get_explosions_processed() const { return value_.explosions_processed; }
	int get_items_destroyed() const { return value_.items_destroyed; }
	int get_crackles() const { return value_.crackles; }
	int get_debris_triangles() const { return value_.debris_triangles; }
	PackedStringArray get_sound_names() const;
	PackedVector3Array get_sound_positions() const;
	PackedVector3Array get_death_light_positions() const;
	PackedFloat32Array get_death_light_radii() const;
	TypedArray<HuskSwapEvent> get_husk_swaps() const;
};

} // namespace godot
