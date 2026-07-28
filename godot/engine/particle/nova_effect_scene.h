#pragma once

#include <cstdint>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <particle/effect_scene.h>

#include "nova_particle_file.h"

namespace godot {

// Godot adapter for the portable EffectScene module. This class owns no
// Nodes and performs no simulation or rendering of its own: it only converts
// Godot values to the portable interface and converts snapshots back to
// value-only Dictionaries for runtime/debug consumers.
class NovaEffectScene : public RefCounted {
	GDCLASS(NovaEffectScene, RefCounted)

public:
	enum Admission {
		ADMISSION_ALWAYS = 0,
		ADMISSION_REPLACE_OWNED = 1,
		ADMISSION_SUPPRESS_WHILE_OWNED = 2,
	};

	enum Binding {
		BINDING_WORLD = 0,
		BINDING_FOLLOW_OWNER = 1,
	};

	enum RenderDomain {
		RENDER_DOMAIN_WORLD = 0,
		RENDER_DOMAIN_FIRST_PERSON = 1,
	};

	enum KillPlane {
		KILL_PLANE_DISABLED = 0,
		KILL_PLANE_ABOVE = 1,
		KILL_PLANE_AT_OR_BELOW = 2,
	};

	enum SpawnStatus {
		SPAWN_STATUS_INVALID_REQUEST = -1,
		SPAWN_STATUS_SPAWNED = 0,
		SPAWN_STATUS_SUPPRESSED = 1,
		SPAWN_STATUS_INVALID_HANDLE = 2,
		SPAWN_STATUS_EMPTY_EFFECT = 3,
		SPAWN_STATUS_MISSING_SLOT = 4,
		SPAWN_STATUS_MISSING_OWNER = 5,
		SPAWN_STATUS_GROUP_CAPACITY_REACHED = 6,
		SPAWN_STATUS_EMITTER_CAPACITY_REACHED = 7,
	};

private:
	opennova::particle::EffectScene scene_;
	mutable opennova::particle::ParticleFrameSnapshot last_frame_;
	mutable bool snapshot_dirty_ = true;

	void _materialize_snapshot() const;

protected:
	static void _bind_methods();

public:
	NovaEffectScene();

	// options keys: simulation_tick_seconds, max_live_groups,
	// max_live_emitters, random_seed.
	Dictionary open(const TypedArray<NovaParticleFile> &p_files,
			const Dictionary &p_options = Dictionary());
	int64_t intern(const String &p_effect_name);
	String effect_name(int64_t p_effect_handle) const;

	// request keys: effect_handle, transform, admission, binding,
	// render_domain, slot_token, owner_token, owner_relative_transform,
	// initial_age_ticks, source_tick, source_order, color_tint,
	// spring_const, lod_divisor, kill_plane, kill_plane_y.
	Dictionary spawn(const Dictionary &p_request);

	// Each update is a Dictionary with owner_token, transform, and present.
	// Removing an owner (present=false) detaches all of its following groups.
	void apply_owner_poses_in_place(const Array &p_updates);
	void apply_owner_poses(const Array &p_updates);
	PackedInt64Array get_active_owner_tokens() const;
	void detach(int64_t p_group_id);
	void detach_slot(int64_t p_slot_token);
	void reset_runtime_state();

	// Runtime clock: advances simulation without materializing a render snapshot
	// or serializing one into a throwaway Dictionary. The renderer lazily builds
	// one retained snapshot after a fixed-tick catch-up batch.
	void advance_in_place(double p_delta_seconds);
	Dictionary advance(double p_delta_seconds);
	Dictionary get_frame_snapshot() const;
	Dictionary get_live_counts() const;
	Dictionary inspect(bool p_include_bounds = true) const;

	// Native renderer adapters use the same immutable frame without a
	// Dictionary round trip. This is intentionally not bound to Godot.
	const opennova::particle::ParticleFrameSnapshot &native_frame_snapshot() const;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaEffectScene::Admission);
VARIANT_ENUM_CAST(godot::NovaEffectScene::Binding);
VARIANT_ENUM_CAST(godot::NovaEffectScene::RenderDomain);
VARIANT_ENUM_CAST(godot::NovaEffectScene::KillPlane);
VARIANT_ENUM_CAST(godot::NovaEffectScene::SpawnStatus);
