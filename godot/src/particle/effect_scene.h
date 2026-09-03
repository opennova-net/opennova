#pragma once

#include <cstdint>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <vector>

#include <runtime/particle/effect_scene.h>

#include "particle/particle_file.h"

namespace godot {

class EffectLoadReport;
class EffectSpawnReceipt;
class EffectSpawnRequest;

// A reusable batch of owner pose updates for EffectScene.apply_owner_poses:
// EffectWorld clears and refills one per frame, so the per-frame path
// allocates nothing once warm. add_absent retires the owner (every group
// following it detaches).
class EffectOwnerPoseBatch : public RefCounted {
	GDCLASS(EffectOwnerPoseBatch, RefCounted)

public:
	void clear() { updates_.clear(); }
	void add(int64_t p_owner_token, const Transform3D &p_transform);
	void add_absent(int64_t p_owner_token);
	int get_count() const { return static_cast<int>(updates_.size()); }
	const std::vector<opennova::particle::EffectOwnerPoseUpdate> &updates() const {
		return updates_;
	}

protected:
	static void _bind_methods();

private:
	std::vector<opennova::particle::EffectOwnerPoseUpdate> updates_;
};

// Godot adapter for the portable EffectScene module. This class owns no
// Nodes and performs no simulation or rendering of its own: it only converts
// Godot values to the portable interface; the value-only debug read model
// (EffectWorld.get_debug_group_report) and the renderer read the portable
// snapshots through the native seams below.
class EffectScene : public RefCounted {
	GDCLASS(EffectScene, RefCounted)

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
	EffectScene();

	// options keys: simulation_tick_seconds, max_live_groups,
	// max_live_emitters, random_seed.
	// Loads the catalog documents; the report (particle/effect_load_report.h)
	// carries the engine's load counters plus the input / ignored document counts.
	Ref<EffectLoadReport> open(const TypedArray<ParticleFile> &p_files,
			const Dictionary &p_options = Dictionary());
	int64_t intern(const String &p_effect_name);
	String effect_name(int64_t p_effect_handle) const;

	// One spawn (particle/effect_spawn_records.h): a null or invalid request
	// answers an invalid-request receipt naming the failing field.
	Ref<EffectSpawnReceipt> spawn(const Ref<EffectSpawnRequest> &p_request);

	// Applies every update in the batch; an absent owner detaches all of its
	// following groups. The non-in-place form also refreshes the snapshot.
	void apply_owner_poses_in_place(const Ref<EffectOwnerPoseBatch> &p_batch);
	void apply_owner_poses(const Ref<EffectOwnerPoseBatch> &p_batch);
	PackedInt64Array get_active_owner_tokens() const;
	void detach(int64_t p_group_id);
	void detach_slot(int64_t p_slot_token);
	void reset_runtime_state();

	// Runtime clock: advances simulation without materializing a render snapshot
	// or serializing one into a throwaway Dictionary. The renderer lazily builds
	// one retained snapshot after a fixed-tick catch-up batch.
	void advance_in_place(double p_delta_seconds);

	// Native renderer adapters use the same immutable frame without a
	// Dictionary round trip. This is intentionally not bound to Godot.
	const opennova::particle::ParticleFrameSnapshot &native_frame_snapshot() const;
	// The portable scene itself for the C++ EffectWorld: live counts, the
	// active owner tokens and the debug snapshot (particle::EffectScene::
	// live_counts / active_owner_tokens / inspect) without a Variant round
	// trip. Not bound to Godot.
	const opennova::particle::EffectScene &native_scene() const { return scene_; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::EffectScene::Admission);
VARIANT_ENUM_CAST(godot::EffectScene::Binding);
VARIANT_ENUM_CAST(godot::EffectScene::RenderDomain);
VARIANT_ENUM_CAST(godot::EffectScene::KillPlane);
VARIANT_ENUM_CAST(godot::EffectScene::SpawnStatus);
