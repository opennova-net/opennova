#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>

#include "simulation/present_applier.h"
#include "simulation/present_stats.h"
#include "object/entity_index.h"
#include "object/object_model.h"
#include "mission/mission_object_placer.h"

namespace godot {

class Simulation;

// THE joiner present pass: renders a co-op JOINER's remote entities
// WIRE-DIRECT (docs/net §5.23/§5.25/§5.38b; ADR 0026 — the sole presenter for
// a decoded ClientState).
//
// A production joiner has no authored placed-node identity table. Its native
// sim separately materializes streamed pools 1-3 at the HOST's exact packed
// handles for world-side gameplay, while remote pool-0 organics remain decoded
// client state; this pass renders every remote row from the load batches plus
// live S2C 0x0A instead of resolving a local .bms placement. The joiner's OWN
// player (wire handle H) is self-filtered out of the snapshot (PF_TYPE_ID 0)
// and drawn by LocalPlayerPresenter as the motor-driven local avatar L — the
// live §5.38b two-handle reconciliation.
//
// This class is the COLD path: spawn/defer/unresolved bookkeeping, the
// liveness prune, held-weapon builds, spawn callbacks and stats. It owns a
// private PresentApplier whose wire walk carries plan validity and the
// per-frame per-row hot path (present_applier_wire.cpp) — the behavioral
// semantics and witnesses are documented there and at the engine headers
// (npruntime/wire_present.h for the pool->kind projection).
class WirePresentPass : public RefCounted {
	GDCLASS(WirePresentPass, RefCounted)

public:
	// Building one streamed model can synchronously assemble enough Godot
	// resources to take a substantial part of a frame — a platform cost with
	// no retail counterpart (retail materializes its world stream under the
	// loading/DEATH hold). The budget caps cold builds per presentation call
	// so a large cold topology cannot occupy the SceneTree thread; four keeps
	// tiny dynamic cohorts atomic while yielding hundreds-row streamed loads
	// promptly. NetSessionDrive holds the join-admission edge until
	// pending_spawn_count() drains to zero, so the revealed world is fully
	// materialized, like retail's.
	static constexpr int DEFAULT_COLD_SPAWN_BUDGET = 4;

	// defer_index: any wire row that resolves to an authored placed node is
	// rendered by the mission present walk instead. On the HOST that leaves
	// admitted joiners; a production header-only JOINER passes no defer index
	// and draws every remote row.
	void setup(Object *p_sim, const Ref<MissionObjectPlacer> &p_placer,
			Node3D *p_container, const Ref<EntityIndex> &p_defer_index);
	// SP attachment presentation: only synthetic runtime rows (kind 255,
	// index 0xFFFFFF) render wire-direct.
	void set_synthetic_origin_only(bool p_enabled);
	void set_cold_spawn_budget(int p_budget);
	// Spectator-only one-shot overview camera; live presenters never set one.
	void set_spectator_camera(Camera3D *p_camera);

	void present();
	void present_snapshot(const PackedFloat32Array &p_snap, int p_stride,
			int64_t p_layout_revision);

	// Cold rows the budget deferred on the last presented frame.
	int pending_spawn_count() const { return pending_spawn_count_; }
	Ref<WirePresentStats> get_stats_record() const;

	// The live model owned by this presenter for a packed pool/slot handle
	// (runtime-only entities have no authored BMS identity). Freed entries are
	// ordinary (world teardown frees the container's children first).
	ObjectModel *resolve_wire_handle(int p_handle) const;
	// The live held-weapon model for a wire body (null when unarmed/freed).
	ObjectModel *held_weapon_node(int p_handle) const;
	// Cache and apply one wire draw's environment-lighting context to both its
	// body and held weapon. The cache makes a quality change authoritative even
	// when cold-spawn budgeting has not built either node yet.
	void set_entity_lighting_context(int p_handle, float p_effect_scale,
			bool p_interior_lerp, float p_light_transfer);
	// World position of a named userpoint on this wire body's HELD WEAPON —
	// the anchor retail's adm-arm fire effect spawns at. Falls back to the
	// body's own origin, never the wire fire position (witness:
	// npruntime/wire_present.h ledger, rigid weapon draw + userpoint
	// fallback).
	Vector3 muzzle_world_for(int p_handle, const String &p_userpoint) const;
	int entity_count() const { return int(nodes_.size()); }

	// Render-host seam for consumers that follow a dynamically materialized
	// wire entity; existing nodes are replayed so registration is safe after
	// the first present.
	void set_node_spawned_callback(const Callable &p_callback);

	// Injection seam (tests/tooling): adopt an existing node as this
	// presenter's wire avatar for `handle`, exactly as a cold build would
	// have. The presenter does NOT take ownership.
	void register_wire_node(int p_handle, ObjectModel *p_node);

	void reset_runtime_state();
	void teardown() { reset_runtime_state(); }

	// The applier's held-weapon edge callback (public for callable_mp; not
	// bound). Builds/frees the third-person gun when a body's ADM changes.
	Node3D *rebuild_held_weapon(int p_handle, int p_adm);

protected:
	static void _bind_methods();

private:
	Simulation *sim() const;
	Node3D *container() const;
	void free_wire_node(int p_handle);
	void free_held_weapon(int p_handle);
	void apply_lighting_context(int p_handle);
	bool wire_node_matches_row(ObjectModel *p_node,
			const PackedFloat32Array &p_snap, int p_base, int p_type_id) const;
	int consume_present_logic_tick_delta();
	void frame_spectator_camera();
	void trace_cold_build(const char *p_stage, int p_handle, int p_type_id,
			int p_visual_item_id) const;

	ObjectID sim_id_;
	Ref<MissionObjectPlacer> placer_;
	ObjectID container_id_;
	Ref<EntityIndex> defer_index_;
	Ref<PresentApplier> applier_;
	bool synthetic_origin_only_ = false;
	int cold_spawn_budget_ = DEFAULT_COLD_SPAWN_BUDGET;
	ObjectID camera_id_;
	bool camera_framed_ = false;

	HashMap<int32_t, ObjectID> nodes_;
	HashMap<int32_t, int32_t> unresolved_;
	HashMap<int32_t, ObjectID> weapon_nodes_;
	HashMap<int32_t, String> weapon_graphics_;
	// Mirrors the retail per-entity lighting fields (setup_terrain_effect_for_entity
	// @0x5c74a0; sun visibility Entity_ComputeSunVisibility @0x5c6800 -
	// docs/render/render-lighting-re.md).
	struct LightingContext {
		float effect_scale = 1.0f;
		bool interior_lerp = false;
		float light_transfer = 0.0f;
	};
	HashMap<int32_t, LightingContext> lighting_contexts_;
	Callable node_spawned_callback_;
	int pending_spawn_count_ = 0;
	int64_t last_present_logic_tick_ = -1;
	String trace_path_;

	int64_t stat_spawned_ = 0;
	int64_t stat_unresolved_ = 0;
	int64_t stat_live_ = 0;
};

} // namespace godot
