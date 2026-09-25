#pragma once

#include <cstdint>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/plane.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <vector>

#include <runtime/particle/effect_scene.h>
#include <runtime/world/script_effects.h>

#include "particle/particle_file.h"

namespace godot {

class EffectLoadReport;
class EffectSpawnReceipt;
class EffectSpawnRequest;

// A reusable batch of owner pose updates for EffectScene::apply_owner_poses:
// EffectWorld clears and refills one per frame, so the per-frame path
// allocates nothing once warm. add_absent retires the owner (every group
// following it detaches). A plain C++ scratch, not a ClassDB class.
class EffectOwnerPoseBatch {
public:
	void clear() { updates_.clear(); }
	void add(int64_t p_owner_token, const Transform3D &p_transform);
	void add_absent(int64_t p_owner_token);
	int get_count() const { return static_cast<int>(updates_.size()); }
	const std::vector<opennova::particle::EffectOwnerPoseUpdate> &updates() const {
		return updates_;
	}

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
        ADMISSION_STORE_OWNED = 3,
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
        SPAWN_STATUS_DISABLED = 8,
	};

private:
	std::shared_ptr<opennova::particle::EffectScene> scene_ =
            std::make_shared<opennova::particle::EffectScene>();
	mutable opennova::particle::ParticleFrameSnapshot last_frame_;
	mutable bool snapshot_dirty_ = true;
	opennova::particle::ParticleViewFrustum frustum_{};
	const opennova::particle::EffectSectionMasks *section_masks_ = nullptr;

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
    // Native-only script descriptor bridge; no Variant round trip.
    void spawn_script_effect(const opennova::world::ScriptEffectEvent &event,
            int64_t slot, int64_t owner, uint32_t age_ticks, float water_height,
            const opennova::particle::EffectSectionGate &gate);
	String effect_name(int64_t p_effect_handle) const;

	// One spawn (particle/effect_spawn_records.h): a null or invalid request
	// answers an invalid-request receipt naming the failing field.
	Ref<EffectSpawnReceipt> spawn(const Ref<EffectSpawnRequest> &p_request);

	// Applies every update in the batch; an absent owner detaches all of its
	// following groups. The non-in-place form also refreshes the snapshot.
	void apply_owner_poses_in_place(const EffectOwnerPoseBatch &p_batch);
	void apply_owner_poses(const EffectOwnerPoseBatch &p_batch);
	void detach(int64_t p_group_id);
	void detach_slot(int64_t p_slot_token);
	bool set_group_parameters(int64_t p_group_id, float p_rate_control,
			float p_offset_control);
	// The rotor-wash re-trigger (particle::EffectScene::trigger_group_children):
	// one particle per child emitter at `p_position` along `p_forward`, bound
	// to `p_force_zone`. Native-only seam for the EffectWorld device.
	bool trigger_group_children(int64_t p_group_id, const Vector3 &p_position,
			const Vector3 &p_forward, int p_force_zone);
	void reset_runtime_state();

	// Runtime clock: advances simulation without materializing a render snapshot
	// or serializing one into a throwaway Dictionary. The renderer lazily builds
	// one retained snapshot after a fixed-tick catch-up batch.
	void advance_in_place(double p_delta_seconds);
	void advance_with_forces(
			double p_delta_seconds, const opennova::particle::ParticleForceField *forces);
	// The two per-advance environment inputs retail reads off globals: the
	// mission wind (GLOBALWIND drift, effect-frame units per second) and the
	// camera's clip planes (the NOVISNOUPDATE gate). `p_inside_probe` is a
	// point known to be inside the view (the planes are re-oriented so it is);
	// clearing the frustum advances every emitter.
	void set_global_wind(const Vector3 &p_wind);
	void set_view_frustum(const TypedArray<Plane> &p_planes, const Vector3 &p_inside_probe);
	// The building section masks the group gate reads on the next advances
	// (particle::EffectSectionMasks, borrowed); null leaves every group visible.
	void set_section_masks(const opennova::particle::EffectSectionMasks *p_masks) {
		section_masks_ = p_masks;
	}
	void clear_view_frustum();

	// Native renderer adapters use the same immutable frame without a
	// Dictionary round trip. This is intentionally not bound to Godot.
	const opennova::particle::ParticleFrameSnapshot &native_frame_snapshot() const;
	// The portable scene itself for the C++ EffectWorld: live counts, the
	// active owner tokens and the debug snapshot (particle::EffectScene::
	// live_counts / active_owner_tokens / inspect) without a Variant round
	// trip. Not bound to Godot.
	const opennova::particle::EffectScene &native_scene() const { return *scene_; }
    std::shared_ptr<opennova::particle::EffectScene> shared_native_scene() const { return scene_; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::EffectScene::Admission);
VARIANT_ENUM_CAST(godot::EffectScene::Binding);
VARIANT_ENUM_CAST(godot::EffectScene::RenderDomain);
VARIANT_ENUM_CAST(godot::EffectScene::KillPlane);
VARIANT_ENUM_CAST(godot::EffectScene::SpawnStatus);
