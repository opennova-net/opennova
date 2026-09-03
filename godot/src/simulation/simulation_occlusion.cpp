// Simulation — the occlusion runtime (building portals, iris march, sound
// occlusion), the F3 hitbox/round reports and pick, the debug round spawn,
// and the ray/contact capture seams the F3 Rays and Physics windows drive.
#include "simulation/simulation_internal.h"
#include "simulation/debug_pick_card.h"
#include "simulation/hitbox_debug_report.h"
#include "simulation/round_debug_report.h"

#include <runtime/replication/connection_fan.h>
#include <runtime/world/occlusion_feed.h>
#include <runtime/replication/entity_wire_bridge.h> // entity_class_of (the host's own rows)
#include <runtime/renderer/light_runtime.h> // sun_visibility_factor — the quality->scale owner
#include <runtime/world/occlusion_camera.h> // the camera hand-over
#include <runtime/world/iris_march.h> // the iris exposure march
#include <runtime/world/presentation_frame.h>

#include <unordered_set>

using namespace sim_internal;

void Simulation::occlusion_init_mission() {
	// The kernel's mission-start portal init (the witness lives there).
	if (!kernel_) return;
	kernel_->occlusion_init_mission();
}

void Simulation::run_occlusion_frame(const Transform3D &p_camera, double p_fov_y_deg,
                                         double p_aspect, double p_near,
                                         double p_fog_dist_units, double p_water_z_units,
                                         bool p_force_indoors) {
	if (!kernel_) return;
	// The camera hand-over: the scene's view as presentation-frame vectors;
	// the mission/render remaps, the frustum planes and the Q22 rows are the
	// engine's (runtime/world/occlusion_camera.h).
	opennova::world::OcclusionViewSpec view;
	const Vector3 eye = p_camera.origin;
	const Vector3 fwd_g = -p_camera.basis.get_column(2).normalized();
	const Vector3 right_g = p_camera.basis.get_column(0).normalized();
	const Vector3 up_g = p_camera.basis.get_column(1).normalized();
	auto store = [](const Vector3 &v, float out[3]) {
		out[0] = static_cast<float>(v.x);
		out[1] = static_cast<float>(v.y);
		out[2] = static_cast<float>(v.z);
	};
	store(eye, view.eye);
	store(fwd_g, view.forward);
	store(right_g, view.right);
	store(up_g, view.up);
	view.fov_y_deg = static_cast<float>(p_fov_y_deg);
	view.aspect = static_cast<float>(p_aspect);
	view.near_units = static_cast<float>(p_near);
	view.fog_dist_units = static_cast<float>(p_fog_dist_units);
	view.water_z_units = static_cast<float>(p_water_z_units);
	view.local_blink_flags = kernel_->collision.local_player_blink_flags;
	view.force_indoors = p_force_indoors;
	opennova::world::OcclusionFrameCamera cam;
	opennova::world::occlusion_camera_from_view(view, cam);
	// Mirror the env view distance into the 0x0A priority score's global — the
	// same value retail's env writes into word_26C681E for the render AND the
	// priority builder to read (D-NET-139: the LOS gate + the +200 inside-view
	// bonus). Headless embedders that never run an occlusion frame leave it 0,
	// which disables both terms exactly like an unwritten retail global.
	opennova::replication::set_view_distance_units(static_cast<int>(p_fog_dist_units));

	const uint64_t occl_build_start =
			runtime_profiling_enabled_ ? opennova::io::perf_now_us() : 0;
	kernel_->occlusion.build_frame(kernel_->world, kernel_->collision, cam);
	const uint64_t occl_probe_start =
			runtime_profiling_enabled_ ? opennova::io::perf_now_us() : 0;
	if (runtime_profiling_enabled_)
		last_occlusion_build_us_ = occl_probe_start - occl_build_start;

	// The entity collectors' render gates over the non-building entities the
	// host draws. [orig: Terrain_CollectVisibleEntities_0 @ 0x5c6f20 /
	// collect_visible_entities_for_terrain @ 0x5c8c60]
	occlusion_culled_bms_.clear();
	std::vector<opennova::world::EntityHandle> &handles =
			occlusion_probe_handles_;
	handles.clear();
	kernel_->world.registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind == opennova::world::EntityKind::Building ||
		    e.kind == opennova::world::EntityKind::Marker)
			return;
		if (e.bms_id == 0) return; // wire avatars ride their own present path
		handles.push_back(e.handle);
	});
	for (const opennova::world::EntityHandle h : handles) {
		opennova::world::Entity *e = kernel_->world.registry.get(h);
		if (e == nullptr) continue;
		if (!kernel_->occlusion.entity_render_visible(kernel_->world, kernel_->collision, *e, cam))
			occlusion_culled_bms_.push_back(e->bms_id);
	}
	// The decoded rows the wire pass draws — remote organics and runtime
	// spawns with no placed identity — pass the SAME collector gate: retail's
	// client walks the pool entities it built from the wire exactly as the
	// host walks its own (the witness lives on OcclusionWorld::
	// sphere_render_visible). A row with a registry twin uses that twin's
	// collision bound sphere (the host's runtime spawns); a bare row is the
	// position-centred unit sphere the organics leg above falls back to.
	occlusion_culled_wire_.clear();
	if (runtime_ != nullptr) {
		// A latch belongs to one row lifetime: drop the counters of handles
		// that left the state so a reused handle starts fresh (retail memsets
		// the destroyed entity, latch included).
		std::unordered_set<uint16_t> live_handles;
		for (const opennova::replication::ClientEntityState &es :
				runtime_->state().entities)
			live_handles.insert(es.handle);
		for (auto it = wire_occlusion_latch_.begin();
				it != wire_occlusion_latch_.end();) {
			if (live_handles.count(it->first) == 0)
				it = wire_occlusion_latch_.erase(it);
			else
				++it;
		}
		const uint16_t self_handle = runtime_->has_self_handle()
				? runtime_->self_handle()
				: opennova::world::EntityHandle::kInvalid;
		for (const opennova::replication::ClientEntityState &es :
				runtime_->state().entities) {
			const uint16_t handle = es.handle;
			if (handle == opennova::world::EntityHandle::kInvalid ||
					es.type_id == 0 || handle == self_handle)
				continue;
			// A hidden row is never collected; the present pass hides it
			// itself, and its latch does not tick (the gate's bit0 test in
			// OcclusionWorld::entity_render_visible).
			if (es.state_flags_known && (es.state_flags & 0x01u) != 0) continue;
			const opennova::world::EntityHandle h{handle};
			const opennova::world::Entity *twin = nullptr;
			if (!joiner_ || h.pool() != 0) {
				const opennova::world::Entity *candidate =
						kernel_->world.registry.get(h);
				if (candidate != nullptr &&
						static_cast<uint16_t>(candidate->item_id) == es.type_id)
					twin = candidate;
			}
			if (twin != nullptr && (twin->bms_id != 0 ||
					twin->spawn_origin != opennova::world::kSpawnOriginNone))
				continue; // a placed row: the registry walk above gated it
			if (h == kernel_->world.cached.local_player) continue;
			int32_t center_world[3] = {es.x, es.y, es.z};
			int32_t radius = 0x10000;
			const opennova::world::CollisionModel *cm = twin != nullptr
					? kernel_->collision.model_for(kernel_->world, h)
					: nullptr;
			if (cm != nullptr && cm->valid()) {
				int32_t center_local[3];
				opennova::world::OcclusionWorld::bound_sphere_fixed(
						*cm, center_local, radius);
				const opennova::world::CollisionMatrix pose =
						opennova::world::collision_matrix_from_heading(
								es.heading_bam, center_world);
				pose.transform_point(center_local, center_world);
			}
			uint8_t &latch = wire_occlusion_latch_[handle];
			if (!kernel_->occlusion.sphere_render_visible(kernel_->collision, cam,
						center_world, radius, latch, kernel_->world.logic_tick))
				occlusion_culled_wire_.push_back(static_cast<int32_t>(handle));
		}
	}
	if (runtime_profiling_enabled_)
		last_occlusion_probe_us_ = opennova::io::perf_now_us() - occl_probe_start;
}

PackedInt64Array Simulation::get_building_visibility() const {
	PackedInt64Array out;
	if (!kernel_) return out;
	// Pairs [bms_id, visible<<32 | mask] for every building with an OCCLUSION
	// instance, plus collision-backed de-batched buildings that still entered
	// the retail building batch. OOBJ instances apply their section mask.
	// Without OOBJ there is no safe reimpl part-to-section map, so those buildings
	// keep all render parts while still receiving batch/frustum/TOC visibility.
	kernel_->world.registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind != opennova::world::EntityKind::Building || e.bms_id == 0) return;
		const bool has_occlusion = kernel_->occlusion.has_instance(e.handle);
		if (!has_occlusion &&
				kernel_->collision.model_for(kernel_->world, e.handle) == nullptr)
			return;
		const bool visible = kernel_->occlusion.building_visible(e.handle);
		const uint32_t mask =
		    has_occlusion ? kernel_->occlusion.section_mask(e.handle) : 0xFFFFFFFFu;
		out.push_back(e.bms_id);
		out.push_back(opennova::world::pack_building_visibility(mask, visible));
	});
	return out;
}

int64_t Simulation::building_visibility_mask(int64_t p_packed) {
	return static_cast<int64_t>(opennova::world::building_visibility_mask(p_packed));
}

bool Simulation::building_visibility_visible(int64_t p_packed) {
	return opennova::world::building_visibility_visible(p_packed);
}

PackedInt32Array Simulation::get_render_culled_bms_ids() const {
	PackedInt32Array out;
	for (const int32_t id : occlusion_culled_bms_) out.push_back(id);
	return out;
}

// Same verdict walk as get_building_visibility(), emitting only pairs whose
// packed visible<<32|mask changed since the last call. The GDScript apply
// walks changes instead of the whole building set, so a steady frame does no
// per-building node work at all.
PackedInt64Array Simulation::get_building_visibility_changes() {
	PackedInt64Array out;
	if (!kernel_) return out;
	kernel_->world.registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind != opennova::world::EntityKind::Building || e.bms_id == 0) return;
		const bool has_occlusion = kernel_->occlusion.has_instance(e.handle);
		if (!has_occlusion &&
				kernel_->collision.model_for(kernel_->world, e.handle) == nullptr)
			return;
		const bool visible = kernel_->occlusion.building_visible(e.handle);
		const uint32_t mask =
		    has_occlusion ? kernel_->occlusion.section_mask(e.handle) : 0xFFFFFFFFu;
		const int64_t packed = opennova::world::pack_building_visibility(mask, visible);
		const uint32_t key = e.handle.packed;
		auto it = occl_apply_building_last_.find(key);
		if (it != occl_apply_building_last_.end() && it->second == packed) return;
		occl_apply_building_last_[key] = packed;
		out.push_back(e.bms_id);
		out.push_back(packed);
	});
	return out;
}

// [added..., removed...] as [count, ids..., count, ids...] against the applied
// baseline, which becomes the current set (world/occlusion_feed.h).
static PackedInt32Array culled_changes_since(const std::vector<int32_t> &p_now,
		std::vector<int32_t> &r_applied) {
	std::vector<int32_t> added;
	std::vector<int32_t> removed;
	opennova::world::culled_changes_since(p_now, r_applied, added, removed);
	PackedInt32Array out;
	out.push_back(static_cast<int32_t>(added.size()));
	for (const int32_t id : added) out.push_back(id);
	out.push_back(static_cast<int32_t>(removed.size()));
	for (const int32_t id : removed) out.push_back(id);
	return out;
}

PackedInt32Array Simulation::get_wire_render_culled_changes() {
	return culled_changes_since(occlusion_culled_wire_, occl_apply_culled_wire_last_);
}

PackedInt32Array Simulation::get_render_culled_changes() {
	return culled_changes_since(occlusion_culled_bms_, occl_apply_culled_last_);
}

float Simulation::sun_quality_factor(int p_quality) const {
	// quality = 4 - blocked, so the owner's factor(blocked) inverts cleanly.
	return opennova::renderer::sun_visibility_factor(4 - p_quality);
}

// The per-drawn-entity sun-visibility factor feed (D-RLIT-3). Retail computes
// the factor inside the sector render walk for every entity it draws and
// pushes it onto the render-state stack around that entity's submits
// [orig: setup_terrain_effect_for_entity @0x5c74a0 -> Entity_ComputeSunVisibility
// @0x5c6800, stack write @0x5c7bff, see docs/render/render-lighting-re.md];
// contained entities take the interior light group instead and the factor
// stays 1.0. The blocked-ray count and eligibility gate are engine-side
// (world::CollisionWorld); this walk mirrors both drawn identity domains and
// diffs quality per BMS id or wire handle so a steady frame emits nothing.
PackedInt64Array Simulation::get_draw_lighting_changes(
		const Vector3 &p_light_dir) {
	PackedInt64Array out;
	if (!kernel_) return out;

	// Sun step in mission fixed: light_dir * 200 u, the same tuple mapping the
	// iris march uses [orig: end = start + 200 * lightdir @0x5c6858..0x5c6876].
	const int32_t sun[3] = {
		opennova::world::to_fixed(p_light_dir.x * 200.0f),
		opennova::world::to_fixed(-p_light_dir.z * 200.0f),
		opennova::world::to_fixed(p_light_dir.y * 200.0f)};

	std::unordered_set<int32_t> culled(occlusion_culled_bms_.begin(),
	                                   occlusion_culled_bms_.end());
	if (sun_quality_present_layout_revision_ != present_layout_revision_) {
		sun_quality_last_by_wire_.clear();
		sun_quality_present_layout_revision_ = present_layout_revision_;
	}

	const auto entity_quality = [&](const opennova::world::Entity &e) {
		// Contained entities route through the interior light group; the
		// outdoor factor stays 1.0 [orig: the blink-ref branch @0x5c74b7].
		// The +0x1C0 slice gate [orig: @0x5c6808] lives inside the ray walk:
		// an entity with no proximity-candidate slice blocks nothing and holds
		// quality 4 — statics never ray, and only slice candidates (structures
		// overlapping the entity's inflated bubble) can shade it.
		if (e.blink_hits[0] != 0) return static_cast<uint8_t>(4);
		const int blocked =
				kernel_->collision.sun_visibility_blocked_rays(kernel_->world, e, sun);
		return static_cast<uint8_t>(4 - blocked);
	};

	// The local player is a spawned entity (bms_id 0, outside the placed-node
	// walk); its quality feeds the presenter seam only — the FP parts keep the
	// witnessed effectScale=1 exemption while the third-person body dims.
	const opennova::world::Entity *local =
			kernel_->world.registry.get(kernel_->world.cached.local_player);
	local_sun_quality_ = local != nullptr ? entity_quality(*local) : 4;

	kernel_->world.registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind == opennova::world::EntityKind::Building ||
		    e.kind == opennova::world::EntityKind::Marker)
			return;
		// Only authored placements have a MissionPresentPass node addressed by
		// BMS id. Runtime-spawned rows can also carry a nonzero bms_id (players
		// use their net id), but the EntityPresenter wire walk owns their rendering.
		if (e.bms_id == 0 ||
				e.spawn_origin == opennova::world::kSpawnOriginNone)
			return;
		if (e.handle == kernel_->world.cached.local_player) return;
		// Retail only rays a drawn entity; a culled one keeps its last factor
		// until it renders again (the stack slot is simply never pushed).
		if (culled.count(e.bms_id) != 0) return;
		const uint8_t quality = entity_quality(e);
		const auto it = sun_quality_last_by_bms_.find(e.bms_id);
		const uint8_t last =
				it != sun_quality_last_by_bms_.end() ? it->second : 4;
		if (quality == last) return;
		sun_quality_last_by_bms_[e.bms_id] = quality;
		out.push_back(-1);
		out.push_back(e.bms_id);
		out.push_back(quality);
	});

	// Every rendered role consumes ClientState. Placed rows above continue to
	// address MissionPresentPass by BMS id; only rows without authored identity
	// reach the EntityPresenter wire walk and therefore need a wire-handle lighting update.
	// On a joiner, pool-0 H must NEVER be cast to a local EntityHandle (H=0 and
	// L=0 can coexist); streamed pool-1 twins are allowed only after the type
	// check below. Host/SP rows use their authoritative exact-handle entity.
	// Cadence: like the registry walk above, the three casts run per display
	// frame. Retail casts inside the sector render walk for every drawn entity
	// every frame; only the candidate SLICE the walker iterates refreshes on
	// the 17-tick arena edge, which CollisionWorld::build_tick_tables already
	// mirrors for wire rows [orig: Terrain_RenderSectorEntities @0x5c7bf1 /
	// Terrain_RenderSectorEntitiesBySide @0x5c7f9a -> setup_terrain_effect_for_entity
	// @0x5c74a0 -> Entity_ComputeSunVisibility @0x5c6800 per frame; the slice
	// gate g_ProxSliceRefreshCounter >= 0x10 @0x4c240f ->
	// Entity_BuildProximityListsFromPools @0x4c2418, see
	// docs/render/render-lighting-re.md]. Throttling the casts themselves to
	// that cadence would hold a moving vehicle's sun factor stale for up to
	// 16 ticks; the per-handle cache below only suppresses unchanged emits.
	if (!joiner_) {
		// The host presents its own pools (D-NET-140 closed): the wire-rendered
		// rows are the runtime-spawned pool-0 organics and pool-1 dynamics with
		// no authored identity; placed rows went through the walk above.
		for (int pool = 0; pool <= 1; ++pool) {
			kernel_->world.registry.for_each_in_pool(pool, [&](const opennova::world::Entity &e) {
				const uint16_t handle = e.handle.packed;
				if (e.item_id == 0 || e.handle == kernel_->world.cached.local_player ||
						e.spawn_origin != opennova::world::kSpawnOriginNone) {
					sun_quality_last_by_wire_.erase(handle);
					return;
				}
				// A hidden row is not drawn, so retail does not push a new stack
				// value; preserve the last emitted quality (see the wire loop).
				if ((e.flags & 0x01u) != 0) return;
				const opennova::EntityClass cls = opennova::replication::entity_class_of(e);
				const bool person_source = pool == 0 &&
						(cls == opennova::EntityClass::Player ||
						 cls == opennova::EntityClass::Infantry);
				const bool dynamic_source = pool == 1 &&
						kernel_->wire_collision_shape_for_type(static_cast<uint16_t>(e.item_id))
								.pool1_candidate_source_eligible;
				if (!person_source && !dynamic_source) {
					sun_quality_last_by_wire_.erase(handle);
					return;
				}
				const uint8_t quality = entity_quality(e);
				const auto it = sun_quality_last_by_wire_.find(handle);
				const uint8_t last =
						it != sun_quality_last_by_wire_.end() ? it->second : 4;
				if (quality == last) return;
				sun_quality_last_by_wire_[handle] = quality;
				out.push_back(handle);
				out.push_back(0);
				out.push_back(quality);
			});
		}
	} else if (runtime_) {
		const std::unordered_set<int32_t> wire_culled(
				occlusion_culled_wire_.begin(), occlusion_culled_wire_.end());
		for (const opennova::replication::ClientEntityState &es :
				runtime_->state().entities) {
			const uint16_t handle = es.handle;
			if (handle == opennova::world::EntityHandle::kInvalid ||
					es.type_id == 0 ||
					(runtime_->has_self_handle() &&
					 handle == runtime_->self_handle())) {
				sun_quality_last_by_wire_.erase(handle);
				continue;
			}
			// Retail only rays a drawn entity; a culled one keeps its last
			// factor until it renders again (see the registry walk above).
			if (wire_culled.count(static_cast<int32_t>(handle)) != 0) continue;
			// A hidden row is not drawn, so retail does not push a new stack
			// value. Preserve the last emitted quality: if it moves while hidden,
			// the first visible frame must compare against that retained material
			// state and emit the restoration instead of assuming default quality 4.
			if (es.state_flags_known && (es.state_flags & 0x01u) != 0)
				continue;

			const opennova::world::EntityHandle h{handle};
			const opennova::world::Entity *native = nullptr;
			const opennova::world::Entity *joiner_twin = nullptr;
			if (!joiner_) {
				const opennova::world::Entity *candidate =
						kernel_->world.registry.get(h);
				if (candidate != nullptr &&
						static_cast<uint16_t>(candidate->item_id) == es.type_id)
					native = candidate;
			} else if (h.pool() != 0) {
				const opennova::world::Entity *candidate =
						kernel_->world.registry.get(h);
				if (candidate != nullptr &&
						static_cast<uint16_t>(candidate->item_id) == es.type_id)
					joiner_twin = candidate;
			}
			// The EntityPresenter wire walk defers authored rows to their placed node (or
			// static batch). Do not repeat the same native ray query and cache an
			// update for a wire node that deliberately does not exist.
			const opennova::world::Entity *placed =
					native != nullptr ? native : joiner_twin;
			if (h == kernel_->world.cached.local_player ||
					(placed != nullptr && placed->spawn_origin !=
							opennova::world::kSpawnOriginNone)) {
				sun_quality_last_by_wire_.erase(handle);
				continue;
			}

			const bool person_source = h.pool() == 0 &&
					(es.cls == opennova::EntityClass::Player ||
					 es.cls == opennova::EntityClass::Infantry);
			const opennova::world::ResolvedCollisionShape shape =
					kernel_->wire_collision_shape_for_type(es.type_id);
			const bool dynamic_source = h.pool() == 1 &&
					shape.pool1_candidate_source_eligible;
			if (!person_source && !dynamic_source) {
				sun_quality_last_by_wire_.erase(handle);
				continue;
			}

			uint8_t quality = 4;
			if (native != nullptr) {
				quality = entity_quality(*native);
			} else if ((es.rm_entity_flags &
					opennova::world::kEntityFlagIndoors) == 0 &&
					(joiner_twin == nullptr ||
					 joiner_twin->blink_hits[0] == 0)) {
				const int blocked = kernel_->collision.wire_sun_visibility_blocked_rays(
						kernel_->world, handle,
						opennova::world::FixedVec3{es.x, es.y, es.z},
						shape.bbox_center_q16, sun);
				quality = static_cast<uint8_t>(4 - blocked);
			}

			const auto it = sun_quality_last_by_wire_.find(handle);
			const uint8_t last =
					it != sun_quality_last_by_wire_.end() ? it->second : 4;
			if (quality == last) continue;
			sun_quality_last_by_wire_[handle] = quality;
			out.push_back(handle);
			out.push_back(0);
			out.push_back(quality);
		}
	}
	return out;
}

// Mirrors the PF_HIDDEN row source (ent->hidden). The local-view-suppression
// and joiner lifecycle folds only apply to rows without a bms_id, which the
// occlusion frame never manages, so plain hidden is the whole intent here.
bool Simulation::entity_present_visible(int p_bms_id) const {
	if (!kernel_ || p_bms_id == 0) return true;
	bool visible = true;
	bool found = false;
	kernel_->world.registry.for_each([&](const opennova::world::Entity &e) {
		if (found || e.bms_id != p_bms_id) return;
		found = true;
		visible = !e.hidden;
	});
	return visible;
}

void Simulation::reset_occlusion_apply_baseline() {
	occl_apply_building_last_.clear();
	occl_apply_culled_last_.clear();
	occl_apply_culled_wire_last_.clear();
	sun_quality_last_by_bms_.clear();
	sun_quality_last_by_wire_.clear();
	sun_quality_present_layout_revision_ = -1;
	local_sun_quality_ = 4;
	iris_interior_group_entity_ = opennova::world::EntityHandle{};
	iris_interior_group_section_ = 0;
}

bool Simulation::occlusion_water_visible() const {
	return kernel_->occlusion.water_visible();
}

bool Simulation::local_player_indoors() const {
	if (!kernel_) return false;
	const opennova::world::Entity *e = kernel_->world.registry.get(kernel_->world.cached.local_player);
	return e != nullptr && (e->flags & opennova::world::kEntityFlagIndoors) != 0;
}

static_assert(Simulation::BLINK_INDOORS == opennova::world::kBlinkIndoorsBit,
              "BLINK_INDOORS drifted from collision.h");
static_assert(Simulation::BLINK_WATER_OFF == opennova::world::kBlinkWaterOffBit,
              "BLINK_WATER_OFF drifted from collision.h");

int Simulation::local_player_blink_flags() const {
	return static_cast<int>(kernel_->collision.local_player_blink_flags);
}

int Simulation::local_player_interior_item_id() const {
	if (!kernel_) return 0;
	const opennova::world::Entity *player =
			kernel_->world.registry.get(kernel_->world.cached.local_player);
	if (player == nullptr || player->blink_hits[0] == 0) return 0;
	const opennova::world::EntityHandle building =
			opennova::world::EntityHandle::make(
					2, static_cast<int32_t>(player->blink_hits[0] >> 20));
	const opennova::world::Entity *parent = kernel_->world.registry.get(building);
	return parent == nullptr
			? 0
			: static_cast<int>(parent->item_id)
					+ opennova::mission::kItemIdOffset;
}

// The pool-2 entity a packed blink hit names, as a bms_id. 0 = the hit's pool
// slot holds no bms-identified entity (nothing to own a light).
// [orig: Pool_GetEntryUnchecked(2, hit >> 20) @0x56c8c9, see
// docs/render/render-lighting-re.md]
int Simulation::blink_hit_owner_bms_id(uint32_t p_hit) const {
	if (!kernel_ || p_hit == 0) return 0;
	const opennova::world::EntityHandle building =
			opennova::world::EntityHandle::make(
					2, opennova::world::BlinkAccum::hit_pool_entity_index(p_hit));
	const opennova::world::Entity *parent = kernel_->world.registry.get(building);
	return parent == nullptr ? 0 : static_cast<int>(parent->bms_id);
}

PackedInt64Array Simulation::query_blink_owner_at(const Vector3 &p_world) {
	PackedInt64Array out;
	if (!kernel_) return out;
	// Godot world (x, up, z) -> mission fixed (x, -z, up) 16.16.
	const int32_t point[3] = {opennova::world::to_fixed(p_world.x),
	                          opennova::world::to_fixed(-p_world.z),
	                          opennova::world::to_fixed(p_world.y)};
	opennova::world::BlinkAccum blink;
	kernel_->collision.query_blink_boxes_at_point(kernel_->world, point, blink);
	const int owner = blink_hit_owner_bms_id(blink.hits[0]);
	if (owner == 0) return out;
	out.push_back(owner);
	out.push_back(opennova::world::BlinkAccum::hit_section(blink.hits[0]));
	return out;
}

PackedInt64Array Simulation::get_entity_interior_groups() const {
	PackedInt64Array out;
	if (!kernel_) return out;
	kernel_->world.registry.for_each([&](const opennova::world::Entity &e) {
		if (e.bms_id == 0 || e.blink_hits[0] == 0) return;
		const int owner = blink_hit_owner_bms_id(e.blink_hits[0]);
		if (owner == 0 || owner == e.bms_id) return; // never its own interior
		out.push_back(e.bms_id);
		out.push_back(owner);
		out.push_back(opennova::world::BlinkAccum::hit_section(e.blink_hits[0]));
	});
	return out;
}

PackedInt64Array Simulation::local_player_interior_group() const {
	PackedInt64Array out;
	if (!kernel_) return out;
	const opennova::world::Entity *player =
			kernel_->world.registry.get(kernel_->world.cached.local_player);
	if (player == nullptr || player->blink_hits[0] == 0) return out;
	const int owner = blink_hit_owner_bms_id(player->blink_hits[0]);
	if (owner == 0) return out;
	out.push_back(owner);
	out.push_back(
			opennova::world::BlinkAccum::hit_section(player->blink_hits[0]));
	return out;
}

int64_t Simulation::sound_occlusion_distance_q16(const Vector3 &listener_pos,
                                                     const Vector3 &source_pos,
                                                     int64_t distance_q16,
                                                     int source_bms_id) {
	// [orig: Sound_ApplyOcclusionDistance @ 0x529970] — the audio layer feeds
	// the AUDIO listener (camera), emitter/one-shot position, and source
	// identity when known. -1 denotes the local player, positive values are
	// authored BMS ids, and zero keeps the no-entity path. Godot world
	// (x, up, z) -> mission fixed (x, -z, up) 16.16.
	if (!kernel_) return distance_q16;
	const int32_t lp[3] = {opennova::world::to_fixed(listener_pos.x),
	                       opennova::world::to_fixed(-listener_pos.z),
	                       opennova::world::to_fixed(listener_pos.y)};
	const int32_t sp[3] = {opennova::world::to_fixed(source_pos.x),
	                       opennova::world::to_fixed(-source_pos.z),
	                       opennova::world::to_fixed(source_pos.y)};
	// Retail's emitter slot carries the source entity pointer from
	// registration (@0x528659 passes slot+4); the id index is that identity.
	// A static's blink/indoors state is the pool stagger think's product
	// (AiSystem, [orig: Entity_UpdateAllEntities @0x4c2299..0x4c22ba]), not
	// something the audio query refreshes.
	opennova::world::EntityHandle source;
	if (source_bms_id < 0) {
		source = kernel_->world.cached.local_player;
	} else if (source_bms_id > 0) {
		source = handle_for_bms_id(source_bms_id);
	}
	return kernel_->collision.sound_occlusion_inflate(kernel_->world, kernel_->world.cached.local_player,
	                                                source, lp, sp,
	                                                static_cast<int32_t>(distance_q16));
}

opennova::world::EntityHandle Simulation::handle_for_bms_id(int p_bms_id) const {
	if (!kernel_ || p_bms_id <= 0) return opennova::world::EntityHandle{};
	const uint64_t serial = kernel_->world.registry.spawn_serial();
	if (bms_handle_index_world_ != &kernel_->world || bms_handle_index_serial_ != serial) {
		bms_handle_index_.clear();
		kernel_->world.registry.for_each([&](const opennova::world::Entity &e) {
			if (e.bms_id > 0 && bms_handle_index_.find(e.bms_id) == bms_handle_index_.end())
				bms_handle_index_[e.bms_id] = e.handle;
		});
		bms_handle_index_world_ = &kernel_->world;
		bms_handle_index_serial_ = serial;
	}
	const auto found = bms_handle_index_.find(p_bms_id);
	if (found == bms_handle_index_.end()) return opennova::world::EntityHandle{};
	// A despawned row's slot may have been reused; confirm the occupant still
	// carries the id before handing the handle out.
	const opennova::world::Entity *e = kernel_->world.registry.get(found->second);
	return e != nullptr && e->bms_id == p_bms_id ? found->second
	                                              : opennova::world::EntityHandle{};
}

PackedInt32Array Simulation::compute_iris_samples(const Vector3 &p_cam_pos,
                                                      const Vector3 &p_cam_forward,
                                                      const Vector3 &p_light_dir) {
	// The march is the engine's (runtime/world/iris_march.h); this converts
	// the presentation-frame vectors to mission fixed and mirrors the interior
	// light group the last sample named.
	PackedInt32Array out;
	if (!kernel_) return out;
	auto mission_fixed = [](const Vector3 &v, float scale, int32_t fixed[3]) {
		const float pres[3] = { static_cast<float>(v.x) * scale,
			static_cast<float>(v.y) * scale, static_cast<float>(v.z) * scale };
		float mission[3];
		opennova::world::mission_from_presentation(pres, mission);
		fixed[0] = opennova::world::to_fixed(mission[0]);
		fixed[1] = opennova::world::to_fixed(mission[1]);
		fixed[2] = opennova::world::to_fixed(mission[2]);
	};
	int32_t cam[3];
	mission_fixed(p_cam_pos, 1.0f, cam);
	int32_t reach[3];
	mission_fixed(p_cam_forward, opennova::world::kIrisMarchReachUnits, reach);
	int32_t end[3] = { cam[0] + reach[0], cam[1] + reach[1], cam[2] + reach[2] };
	int32_t sun[3];
	mission_fixed(p_light_dir, opennova::world::kIrisSunRayUnits, sun);

	opennova::world::IrisMarch march;
	opennova::world::compute_iris_march(kernel_->world, kernel_->collision, kernel_->occlusion,
			cam, end, sun, march);
	if (march.count == 0) return out;
	iris_interior_group_entity_ = march.interior_group_entity;
	iris_interior_group_section_ = march.interior_group_section;
	for (int i = 0; i < march.count; ++i) out.append(march.samples[i]);
	return out;
}

Ref<HitboxDebugReport> Simulation::get_hitbox_debug() {
	constexpr int32_t kEntityCap = 96;
	Ref<HitboxDebugReport> out;
	out.instantiate();
	if (!kernel_) return out;

	// Anchor on the local player. All hitbox payloads use
	// the same 80-unit debug budget; a preview with no player sweeps to the caps.
	int32_t anchor[3] = {0, 0, 0};
	int32_t debug_range = -1;
	const opennova::world::EntityHandle local_player =
	    kernel_->world.cached.local_player;
	const opennova::world::Entity *lp =
	    local_player.valid() ? kernel_->world.registry.get(local_player) : nullptr;
	if (lp != nullptr) {
		anchor[0] = opennova::world::to_fixed(lp->position.x);
		anchor[1] = opennova::world::to_fixed(lp->position.y);
		anchor[2] = opennova::world::to_fixed(lp->position.z);
		debug_range = 80 << 16; // the organic-fallback scan below shares the budget
	}
	const std::vector<opennova::world::CollisionWorld::DebugHitboxEntity> ents =
	    kernel_->hitboxes(lp != nullptr ? lp->position : opennova::world::Vec3{},
	                      lp != nullptr ? 80.0f : -1.0f, kEntityCap, 24000);
	for (const opennova::world::CollisionWorld::DebugHitboxEntity &ent : ents) {
		Ref<HitboxDebugEntity> d;
		d.instantiate();
		d->set_entity_handle(static_cast<int>(ent.handle.packed));
		d->set_pos(godot_from_fixed3(ent.pos));
		d->set_bound_radius(static_cast<float>(ent.bound_radius / kFixed16));
		d->set_husk(ent.husk);
		d->set_has_faces(ent.has_faces);
		d->set_face_total(ent.face_total);
		PackedVector3Array tris;
		PackedByteArray materials;
		PackedInt32Array flags;
		tris.resize(static_cast<int64_t>(ent.faces.size()) * 3);
		materials.resize(static_cast<int64_t>(ent.faces.size()));
		flags.resize(static_cast<int64_t>(ent.faces.size()));
		Vector3 *tw = tris.ptrw();
		uint8_t *mw = materials.ptrw();
		int32_t *fw = flags.ptrw();
		for (size_t i = 0; i < ent.faces.size(); ++i) {
			const opennova::world::CollisionWorld::DebugHitboxFace &f = ent.faces[i];
			for (int k = 0; k < 3; ++k) tw[i * 3 + k] = godot_from_fixed3(f.v[k]);
			mw[i] = f.material;
			fw[i] = static_cast<int32_t>(f.flags);
		}
		d->set_tris(tris);
		d->set_materials(materials);
		d->set_flags(flags);
		out->add_entity(d);
	}

	// Posed pool-0 COBJ spheres from the exact person narrow phase. They share
	// the nearby 80-unit/96-actor debug budget. Preserve F3's late-spawn demand
	// bridge even though the local avatar is presentation-hidden; one spare query
	// slot then prevents its authored rows from consuming the target budget.
	// Entities whose graphic cannot supply usable authored sections are appended
	// below with the bounded compatibility fallback used by RoundSim.
	if (local_player.valid()) kernel_->ensure_collision_instance(kernel_->world, local_player);
	std::unordered_map<uint16_t, bool> posed_handles;
	const std::vector<opennova::world::CollisionWorld::DebugPersonSection> people =
	    kernel_->collision.debug_person_sections(
	        kernel_->world, anchor, debug_range, kEntityCap + 1);
	for (const opennova::world::CollisionWorld::DebugPersonSection &person : people) {
		if (person.handle == local_player) continue;
		const bool new_handle =
		    posed_handles.find(person.handle.packed) == posed_handles.end();
		if (new_handle && posed_handles.size() >= static_cast<size_t>(kEntityCap)) break;
		out->add_organic(HitboxDebugOrganic::make(static_cast<int>(person.handle.packed),
		    person.section, godot_from_fixed3(person.center),
		    static_cast<float>(person.radius / kFixed16),
		    static_cast<float>(person.authored_radius / kFixed16), person.masked, false));
		posed_handles[person.handle.packed] = true;
	}
	const size_t pool0 = kernel_->world.registry.pool_capacity(0);
	int fallback_entity_count = static_cast<int>(posed_handles.size());
	for (size_t s = 0; s < pool0; ++s) {
		if (fallback_entity_count >= kEntityCap) break;
		const opennova::world::Entity *e =
		    kernel_->world.registry.get(opennova::world::EntityHandle{static_cast<uint16_t>(s)});
		if (e == nullptr || e->handle == local_player ||
		    (e->engine_flags & 0x02000001u) != 0 ||
		    posed_handles.find(static_cast<uint16_t>(s)) != posed_handles.end())
			continue;
		if (debug_range >= 0) {
			const int32_t ep[3] = {
			    opennova::world::to_fixed(e->position.x),
			    opennova::world::to_fixed(e->position.y),
			    opennova::world::to_fixed(e->position.z)};
			if (std::llabs(static_cast<int64_t>(ep[0]) - anchor[0]) > debug_range ||
			    std::llabs(static_cast<int64_t>(ep[1]) - anchor[1]) > debug_range ||
			    std::llabs(static_cast<int64_t>(ep[2]) - anchor[2]) > debug_range)
				continue;
		}
		out->add_organic(HitboxDebugOrganic::make(static_cast<int>(s), 1,
		    Vector3(e->position.x,
		            e->position.z + opennova::world::kOrganicStandInCenterZ,
		            -e->position.y),
		    opennova::world::kOrganicStandInRadius, opennova::world::kOrganicStandInRadius,
		    false, true));
		++fallback_entity_count;
	}
	return out;
}

int Simulation::debug_spawn_round(const Vector3 &p_from_godot, const Vector3 &p_dir_godot,
                                      const String &p_ammo_name) {
	if (!kernel_) return -1;
	const int ammo_index = kernel_->world.tables.ammo.index_of(p_ammo_name.utf8().get_data());
	if (ammo_index < 0) return -1;
	// Godot world (x, up, z) -> mission (x, -z, up); direction -> the spawn's
	// yaw/pitch BAM (the §5.16 mission bearing: vel = (cos yaw, sin yaw, sin
	// pitch) x speed — round_sim.cpp spawn).
	const double dx = p_dir_godot.x;
	const double dy = -static_cast<double>(p_dir_godot.z);
	const double dz = p_dir_godot.y;
	const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (len <= 0.0) return -1;
	double sz = dz / len;
	if (sz > 1.0) sz = 1.0;
	if (sz < -1.0) sz = -1.0;
	constexpr double kBamPerRad = 4294967296.0 / (2.0 * 3.14159265358979323846);
	opennova::world::RoundSpawnParams params;
	params.owner = kernel_->world.cached.local_player;
	params.shooter_handle = params.owner.valid() ? params.owner.packed : 0xFFFF;
	params.ammo_index = ammo_index;
	params.origin = opennova::world::Vec3{p_from_godot.x, -p_from_godot.z, p_from_godot.y};
	params.dir_yaw_bam =
	    static_cast<int32_t>(std::llround(std::atan2(dy, dx) * kBamPerRad));
	params.dir_pitch_bam = static_cast<int32_t>(std::llround(std::asin(sz) * kBamPerRad));
	return kernel_->world.round_sim.spawn(kernel_->world, params);
}

Ref<DebugPickCard> Simulation::debug_pick_entity(const Vector3 &p_from_godot,
                                             const Vector3 &p_dir_godot,
                                             float p_max_range_units) {
	// Every field carries its typed default (the stable-card convention).
	Ref<DebugPickCard> out;
	out.instantiate();
	if (!kernel_) return out;
	out->set_tick(static_cast<int64_t>(kernel_->world.logic_tick));

	// Godot world (x, up, z) -> mission (x, -z, up) — the debug_spawn_round
	// conversion; the direction is normalized in doubles.
	const double dx = p_dir_godot.x;
	const double dy = -static_cast<double>(p_dir_godot.z);
	const double dz = p_dir_godot.y;
	const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
	if (len <= 0.0) return out;
	double range = static_cast<double>(p_max_range_units);
	if (range < 1.0) range = 1.0;
	if (range > 2000.0) range = 2000.0;
	const double fx = p_from_godot.x;
	const double fy = -static_cast<double>(p_from_godot.z);
	const double fz = p_from_godot.y;

	opennova::world::ProjectileTrace trace;
	trace.start = opennova::world::FixedVec3{
	    opennova::world::to_fixed(static_cast<float>(fx)),
	    opennova::world::to_fixed(static_cast<float>(fy)),
	    opennova::world::to_fixed(static_cast<float>(fz))};
	trace.end = opennova::world::FixedVec3{
	    opennova::world::to_fixed(static_cast<float>(fx + dx / len * range)),
	    opennova::world::to_fixed(static_cast<float>(fy + dy / len * range)),
	    opennova::world::to_fixed(static_cast<float>(fz + dz / len * range))};
	// A plain geometric ray, exactly what a bullet would test: ammo_flags
	// stays 0 (0x80 would bypass terrain, 0x4000000 would skip material-17
	// faces), persons are walked, wire proxies excluded. The local player is
	// the owner, so an eye ray never picks the picker — or the vehicle they
	// are mounted in (the ray[18] mount exclusion).
	trace.owner = kernel_->world.cached.local_player;
	trace.radius_q16 = 0;
	trace.ammo_flags = 0;
	const opennova::world::CollisionWorld::RayDebugScope ray_scope(
	    kernel_->collision,
	    opennova::world::CollisionWorld::RayDebugCategory::kPick);
	const opennova::world::ProjectileHit hit =
	    kernel_->collision.trace_projectile(kernel_->world, trace);
	if (!hit.hit()) return out;

	const int32_t hp[3] = {hit.position_q16.x, hit.position_q16.y, hit.position_q16.z};
	const int32_t hn[3] = {hit.normal_q16.x, hit.normal_q16.y, hit.normal_q16.z};
	out->set_hit_position_godot(godot_from_fixed3(hp));
	out->set_hit_normal_godot(godot_from_fixed3(hn));
	out->set_distance_units(static_cast<float>(range * (static_cast<double>(hit.t_q16) / 65536.0)));
	out->set_section(hit.section_index);
	out->set_face(hit.face_index);
	out->set_bone(hit.bone_index);
	out->set_hit_zone(hit.hit_zone);
	out->set_surface_type(hit.surface_type);
	out->set_material_flags(static_cast<int64_t>(hit.material_flags));

	switch (hit.hit_class) {
		case opennova::world::ProjectileHitClass::Terrain:
			out->set_blocked("terrain");
			return out;
		case opennova::world::ProjectileHitClass::Water:
			out->set_blocked("water");
			return out;
		default:
			break;
	}
	const opennova::world::Entity *ent = kernel_->world.registry.get(hit.geometry_entity);
	if (ent == nullptr) {
		// A decoded wire proxy or an already-freed slot: the geometry hit but
		// carries no pickable identity (joined visual-only clients).
		out->set_blocked("proxy");
		return out;
	}
	out->set_hit(true);
	switch (hit.hit_class) {
		case opennova::world::ProjectileHitClass::StaticEntity:
			out->set_hit_class("static");
			break;
		case opennova::world::ProjectileHitClass::DynamicEntity:
			out->set_hit_class("dynamic");
			break;
		default:
			out->set_hit_class("person");
			break;
	}
	out->set_entity_handle(static_cast<int>(hit.geometry_entity.packed));
	out->set_pool(hit.geometry_entity.pool());
	out->set_kind(opennova::world::spawn_origin_kind(ent->spawn_origin));
	out->set_index(static_cast<int>(
			opennova::world::spawn_origin_index(ent->spawn_origin)));
	out->set_bms_id(ent->bms_id);
	out->set_net_id(static_cast<int>(ent->net_id));
	out->set_item_id(ent->item_id);
	out->set_name(String(ent->name.c_str()));
	out->set_position_godot(mission_to_godot(ent->position));
	out->set_bound_radius(ent->bound_radius);
	return out;
}

Ref<RoundDebugReport> Simulation::get_round_debug() const {
	Ref<RoundDebugReport> out;
	out.instantiate();
	if (!kernel_) return out;
	const opennova::world::RoundSim &rs = kernel_->world.round_sim;
	static const char *const kKindNames[] = {"organic", "item face", "item sphere",
	                                         "terrain",  "expired",   "face miss"};
	// Oldest -> newest so the view can draw newest-last (brightest).
	const int count = rs.debug_trail_count;
	int idx = (rs.debug_trail_next - count + opennova::world::RoundSim::kDebugTrailCap *
	          2) % opennova::world::RoundSim::kDebugTrailCap;
	for (int i = 0; i < count; ++i, idx = (idx + 1) % opennova::world::RoundSim::kDebugTrailCap) {
		const opennova::world::RoundDebugEvent &ev =
		    rs.debug_trail[static_cast<size_t>(idx)];
		Ref<RoundDebugEvent> d;
		d.instantiate();
		d->set_tick(static_cast<int64_t>(ev.tick));
		d->set_kind(static_cast<int>(ev.kind));
		d->set_kind_name(String(ev.kind <= 5 ? kKindNames[ev.kind] : "?"));
		d->set_material(static_cast<int>(ev.material));
		d->set_section(static_cast<int>(ev.section));
		d->set_secondary_section(static_cast<int>(ev.secondary_section));
		d->set_fallback(ev.organic_fallback);
		d->set_face(static_cast<int>(ev.face));
		d->set_effect_tag(ev.effect_tag);
		d->set_effect_tag_name(
		    (ev.effect_tag >= 0 && ev.effect_tag < opennova::world::kImpactEffectTagCount)
		        ? String(opennova::world::kImpactEffectTagNames[ev.effect_tag])
		        : String(""));
		d->set_entity_handle(static_cast<int>(ev.entity));
		d->set_shooter_handle(static_cast<int>(ev.shooter));
		d->set_ammo_index(ev.ammo_index);
		d->set_husk(ev.husk);
		d->set_t(ev.t);
		d->set_p0(mission_to_godot(ev.p0));
		d->set_p1(mission_to_godot(ev.p1));
		d->set_hit(mission_to_godot(ev.hit));
		// The struck entity's item name when it still resolves (wrecks keep
		// their slot until cleanup) — display sugar for the F3 list.
		String label;
		const opennova::world::Entity *te =
		    kernel_->world.registry.get(opennova::world::EntityHandle{ev.entity});
		if (te != nullptr && !te->name.empty())
			label = String(te->name.c_str());
		d->set_entity_name(label);
		out->add_event(d);
	}
	out->set_tick(static_cast<int64_t>(kernel_->world.logic_tick));
	return out;
}

void Simulation::set_ray_debug_recording(bool p_enabled) {
	if (!kernel_) return;
	kernel_->collision.set_ray_debug_enabled(p_enabled);
}

void Simulation::set_ray_debug_filter(int64_t p_mask, int64_t p_ttl_ticks) {
	if (!kernel_) return;
	if (p_mask >= 0) {
		kernel_->collision.set_ray_debug_mask(static_cast<uint32_t>(p_mask));
	}
	if (p_ttl_ticks >= 0) {
		kernel_->collision.set_ray_debug_ttl_ticks(
				static_cast<int32_t>(p_ttl_ticks));
	}
}

void Simulation::clear_ray_debug() {
	if (!kernel_) return;
	// The enable edge clears; bounce the flag to reuse that one clear path.
	const bool was = kernel_->collision.ray_debug_enabled();
	kernel_->collision.set_ray_debug_enabled(false);
	kernel_->collision.set_ray_debug_enabled(was);
}

bool Simulation::native_rays_snapshot(opennova::devtools::RaysSnapshot &out) const {
	using CW = opennova::world::CollisionWorld;
	out = opennova::devtools::RaysSnapshot{};
	static_assert(opennova::devtools::kRayCategoryCount ==
	              static_cast<int>(CW::RayDebugCategory::kCount));
	for (int c = 0; c < opennova::devtools::kRayCategoryCount; ++c) {
		out.categories[c].name =
				CW::ray_debug_category_name(static_cast<CW::RayDebugCategory>(c));
	}
	if (!kernel_) return false;
	out.valid = true;
	out.logic_tick = kernel_->world.logic_tick;
	out.recording = kernel_->collision.ray_debug_enabled();
	out.category_mask = kernel_->collision.ray_debug_mask();
	out.ttl_ticks = kernel_->collision.ray_debug_ttl_ticks();
	const auto &rings = kernel_->collision.ray_debug_rings();
	for (size_t c = 0; c < rings.size(); ++c) {
		out.categories[c].held = rings[c].count;
		out.categories[c].total = rings[c].total;
	}
	return true;
}

void Simulation::set_contact_debug_capture(bool p_enabled) {
	if (!kernel_) return;
	kernel_->collision.set_contact_debug_enabled(p_enabled);
}

void Simulation::set_contact_debug_kind_mask(int64_t p_mask) {
	if (!kernel_ || p_mask < 0) return;
	kernel_->collision.set_contact_debug_mask(static_cast<uint32_t>(p_mask));
}

void Simulation::clear_contact_debug() {
	if (!kernel_) return;
	// The enable edge clears; bounce the flag to reuse that one clear path.
	const bool was = kernel_->collision.contact_debug_enabled();
	kernel_->collision.set_contact_debug_enabled(false);
	kernel_->collision.set_contact_debug_enabled(was);
}

bool Simulation::native_physics_snapshot(opennova::devtools::PhysicsSnapshot &out) const {
	using CW = opennova::world::CollisionWorld;
	out = opennova::devtools::PhysicsSnapshot{};
	static_assert(opennova::devtools::kContactKindCount ==
	              static_cast<int>(CW::ContactDebugKind::kCount));
	for (int k = 0; k < opennova::devtools::kContactKindCount; ++k) {
		out.kinds[k].name =
				CW::contact_debug_kind_name(static_cast<CW::ContactDebugKind>(k));
	}
	if (!kernel_) return false;
	out.valid = true;
	out.logic_tick = kernel_->world.logic_tick;
	out.capturing = kernel_->collision.contact_debug_enabled();
	out.kind_mask = kernel_->collision.contact_debug_mask();
	const CW::ContactDebugRing &ring = kernel_->collision.contact_debug_ring();
	for (int k = 0; k < opennova::devtools::kContactKindCount; ++k) {
		out.kinds[k].total = ring.kind_totals[static_cast<size_t>(k)];
	}
	// Held-per-kind and the TTL-recent count come from one walk of the ring
	// (256 events at the 0.25 s window cadence).
	const uint32_t now = kernel_->world.logic_tick;
	if (ring.count > 0) {
		int idx = (ring.next - ring.count + 2 * CW::kContactDebugCap) %
		          CW::kContactDebugCap;
		for (int i = 0; i < ring.count;
				++i, idx = (idx + 1) % CW::kContactDebugCap) {
			const CW::ContactDebugEvent &ev = ring.events[static_cast<size_t>(idx)];
			if (ev.kind < opennova::devtools::kContactKindCount)
				++out.kinds[ev.kind].held;
			const uint32_t age = now - ev.tick;
			if (age <= static_cast<uint32_t>(CW::kContactDebugTtlTicks)) ++out.recent;
		}
	}
	return true;
}
