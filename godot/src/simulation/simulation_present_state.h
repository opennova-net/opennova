// The Simulation's present-side caches (ADR 0043 d9/d10): the PF_* layout
// identity, the FollowOwner effect-pose index, the minimap snapshot cache, the
// present-row scratch, and the occlusion frame's verdicts, delta baselines and
// sun-quality mirrors. Every field is a per-frame or per-load cache over the
// kernel (the mutable ones are filled by const readers); plain data with no
// behavior.
#pragma once

#include <runtime/inmatch/effect_pose_index.h> // EffectPoseIndex (+ the BmsHandleIndex it resolves through)
#include <runtime/inmatch/role_feeds.h> // EntityLightingFeed

#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

#include <runtime/inmatch/present_rows.h> // PoolPresentLifecycleMap (the host present path's respawn mirror)
#include <runtime/world/entity.h>         // EntityHandle

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace godot {

struct SimulationPresentSnapshot {
	std::vector<float> rows;
	opennova::inmatch::DoorPhaseTable door_phases;
	uint64_t layout_revision = 0;
};

struct SimulationPresentState {
	// --- the present snapshot ---------------------------------------------------
	// Exact identity/order of the most recently returned PF_* buffer. Dynamic
	// values (pose, animation, visibility) deliberately do not participate:
	// GDScript row plans may keep their offsets while reading fresh values.
	struct PresentRowIdentity {
		int32_t wire_handle = 0;
		int32_t type_id = 0;
		int32_t bms_id = 0;
		int32_t kind = -1;
		int32_t index = -1;

		bool operator==(const PresentRowIdentity &p_other) const {
			return wire_handle == p_other.wire_handle &&
			       type_id == p_other.type_id &&
			       bms_id == p_other.bms_id &&
			       kind == p_other.kind &&
			       index == p_other.index;
		}
	};
	mutable std::vector<PresentRowIdentity> layout;
	mutable uint64_t layout_revision = 0;

	// FollowOwner consumes the same wire-decoded pose as the present pass, but it
	// does so once per fixed tick inside a catch-up batch. Keep the identity index
	// native and generation-bound so GDScript does not rebuild the full PF_* buffer
	// plus four Dictionary indexes for every catch-up tick.
	// The present-effect pose index and the authored-id handle index
	// (inmatch/effect_pose_index.h, world/bms_handle_index.h): both lazy,
	// both keyed on the decoded-client epoch / the registry's spawn serial.
	mutable opennova::inmatch::EffectPoseIndex effect_poses;
	mutable opennova::world::BmsHandleIndex bms_handles;
	// The PF_* present rows are built by the engine (runtime/inmatch/present_rows.h,
	// both roles); this owns the host path's respawn-revision mirror and the
	// leased native snapshot storage. Script reads copy only at their boundary.
	mutable opennova::inmatch::PoolPresentLifecycleMap pool_lifecycle;
	mutable std::shared_ptr<SimulationPresentSnapshot> snapshot;
	// The last snapshot's build time and row count (the F3 counters).
	mutable uint64_t last_snapshot_us = 0;
	mutable int last_entity_count = 0;
	// get_hud_minimap_snapshot cache: the retained banks bump
	// ClientMinimapState::revision, and every other input — entity resolves,
	// per-row draw policies, the restored local row — advances only with the
	// world logic tick, so the display frames between 62 Hz ticks reuse the
	// built array instead of re-walking the 1160 retained slots.
	mutable PackedInt32Array minimap_snapshot_cache;
	mutable uint64_t minimap_snapshot_revision = 0;
	mutable uint64_t minimap_snapshot_tick = 0;
	mutable uint16_t minimap_snapshot_local_handle = 0xFFFF;
	mutable bool minimap_snapshot_valid = false;

	// --- the occlusion frame ----------------------------------------------------
	// Per-frame entity render-gate verdicts (bms_id -> culled), rebuilt by
	// run_occlusion_frame; consumed via get_render_culled_changes.
	std::vector<int32_t> occlusion_culled_bms;
	// The same render gate over the decoded rows the EntityPresenter wire walk draws (no
	// placed identity): culled wire handles this frame, the applied baseline,
	// and each row's persistent three-ray latch.
	std::vector<int32_t> occlusion_culled_wire;
	std::vector<int32_t> occl_apply_culled_wire_last;
	std::unordered_map<uint16_t, uint8_t> wire_occlusion_latch;
	// The MODEL foliage tier's anchors this frame: the collected (visible)
	// person entities with a stance bit and no groundEntity, Godot space.
	PackedVector3Array foliage_mask_anchors;
	// Reused probe scratch (cleared per frame, capacity retained).
	std::vector<opennova::world::EntityHandle> occlusion_probe_handles;
	// Delta baselines for the render-occlusion apply path: what the shell last
	// applied, so steady frames emit nothing. Cleared on world reset and via
	// reset_occlusion_apply_baseline() (the occlusion A/B seam re-arms a full
	// re-emit).
	std::unordered_map<uint32_t, int64_t> occl_apply_building_last;
	std::vector<int32_t> occl_apply_culled_last;
	// The per-drawn-entity lighting diff and its last-emitted caches
	// (inmatch/role_feeds.h EntityLightingFeed carries the witnesses), plus
	// the frame's change list draw_lighting_changes hands out.
	opennova::inmatch::EntityLightingFeed entity_lighting;
	std::vector<opennova::inmatch::EntityLightingChange> entity_lighting_changes;
	// Mutable retail Lighting_SetInteriorLightGroup state left by the marched
	// iris samples. Outdoor samples clear it, indoor samples with interior data
	// replace it, and indoor-no-data samples intentionally retain the previous
	// pair. OpenNova's actual draw selection carries groups explicitly, but this
	// state preserves the witnessed sequence instead of erasing the side effect.
	opennova::world::EntityHandle iris_interior_group_entity;
	int32_t iris_interior_group_section = 0;
	// The two halves of run_occlusion_frame: the portal/section build
	// (OcclusionWorld::build_frame) and the per-entity render-gate probe loop.
	uint64_t last_occlusion_build_us = 0;
	uint64_t last_occlusion_probe_us = 0;
};

} // namespace godot
