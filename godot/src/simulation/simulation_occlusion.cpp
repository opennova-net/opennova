// Simulation — the occlusion runtime (building portals, iris march, sound
// occlusion), the hitbox oracle and the entity pick (the Shift+F6 pick and
// the entity_pick probe), the debug round spawn, and the ray/contact capture
// seams the engine's F3 Rays and Physics windows drive natively.
#include "simulation/simulation_internal.h"
#include <runtime/mission/debug_oracles.h> // the hitbox view and the entity pick
#include "simulation/debug_pick_card.h"
#include "simulation/hitbox_debug_report.h"

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
                                         double p_aspect, double p_viewport_width,
                                         double p_near, double p_fog_dist_units,
                                         double p_water_z_units, bool p_force_indoors) {
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
	view.viewport_width = static_cast<float>(p_viewport_width);
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
		present_.last_occlusion_build_us = occl_probe_start - occl_build_start;

	// The entity collectors' render gates over the non-building entities the
	// host draws. [orig: Terrain_CollectVisibleEntities_0 @ 0x5c6f20 /
	// collect_visible_entities_for_terrain @ 0x5c8c60]
	// The death pieces ride the same collect, after the scar caches (retail
	// collect_visible_minimap_slots @ 0x57b560, called from
	// Terrain_CollectVisibleEntities @ 0x5c91bc).
	kernel_->occlusion.collect_death_piece_draws(kernel_->world, cam,
			present_.death_piece_draws);
	present_.occlusion_culled_bms.clear();
	// The BySide walks update the MODEL foliage tiles around the collected
	// person entities whose MoveOrder carries a stance bit (0x100 prone /
	// 0x200 crouch) and whose groundEntity is empty; the visible-entity walk
	// below is that collection, so the anchors ride its verdicts (no second
	// gate call: the latch ticks once per collected entity).
	// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded
	// (flags & 0x300), groundEntity gate @ 0x5c7dd5..0x5c7df7; stance writers
	// Player_PackInputStateToEntity @ 0x4df6a7..0x4df6cd,
	// NapiNPServerMsg_HandleStanceChange @ 0x501c60]
	present_.foliage_mask_anchors.clear();
	const auto anchor_entity = [&](const opennova::world::Entity &e) {
		if (e.kind != opennova::world::EntityKind::Organic ||
				(e.net_stance_bits & 0x3u) == 0 || e.ground_target.valid())
			return;
		present_.foliage_mask_anchors.push_back(
				Vector3(e.position.x, e.position.z, -e.position.y));
	};
	std::vector<opennova::world::EntityHandle> &handles =
			present_.occlusion_probe_handles;
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
			present_.occlusion_culled_bms.push_back(e->bms_id);
		else
			anchor_entity(*e);
	}
	// The decoded rows the wire pass draws — remote organics and runtime
	// spawns with no placed identity — pass the SAME collector gate: retail's
	// client walks the pool entities it built from the wire exactly as the
	// host walks its own (the witness lives on OcclusionWorld::
	// sphere_render_visible). A row with a registry twin (the host's own
	// runtime spawns) takes the twin's live verdict, exactly like the placed
	// rows above; a bare row is the position-centred unit sphere the organics
	// leg above falls back to.
	present_.occlusion_culled_wire.clear();
	if (runtime_ != nullptr) {
		// A latch belongs to one row lifetime: drop the counters of handles
		// that left the state so a reused handle starts fresh (retail memsets
		// the destroyed entity, latch included).
		std::unordered_set<uint16_t> live_handles;
		for (const opennova::replication::ClientEntityState &es :
				runtime_->state().entities)
			live_handles.insert(es.handle);
		for (auto it = present_.wire_occlusion_latch.begin();
				it != present_.wire_occlusion_latch.end();) {
			if (live_handles.count(it->first) == 0)
				it = present_.wire_occlusion_latch.erase(it);
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
			if (!is_joiner() || h.pool() != 0) {
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
			if (twin != nullptr) {
				// The host's own runtime spawn (an addeweap gun child, a
				// runtime-placed item): the listen host walks its OWN pool entity
				// with the same live pose the placed walk above uses. Its decoded
				// row is a spawn image — the loopback 0x0A is header-only, so
				// es.x/y/z never follow a moving carrier, and a sphere pinned
				// there culled the gun the moment the driven buggy left it.
				opennova::world::Entity *live = kernel_->world.registry.get(h);
				if (live == nullptr) continue;
				if (!kernel_->occlusion.entity_render_visible(
							kernel_->world, kernel_->collision, *live, cam))
					present_.occlusion_culled_wire.push_back(static_cast<int32_t>(handle));
				else
					anchor_entity(*live);
				continue;
			}
			// A bare row is the client-built pool entity retail's collector
			// walks: its type's entity+0 bound radius from the shared items.def/
			// model resolve (the replica pipeline's own source), through the
			// person leg for Player/Infantry rows (the parachute flag swaps in
			// the item-185 radius) or the model leg's bound sphere placed by the
			// row's full Euler pose.
			const opennova::world::ResolvedCollisionShape shape =
					kernel_->wire_collision_shape_for_type(es.type_id);
			const int32_t pos[3] = {es.x, es.y, es.z};
			// The row's blink quad from the client's own blink walk (the one
			// the lighting feed runs for a twin-less row): the collector's
			// blink-hits gate reads it before the legs, and the render waves'
			// contained test after them.
			opennova::world::BlinkAccum blink;
			const bool person_source = h.pool() == 0 &&
					(es.cls == opennova::EntityClass::Player ||
							es.cls == opennova::EntityClass::Infantry);
			kernel_->collision.query_wire_blink_boxes_at_point(kernel_->world, handle, pos,
					person_source || shape.item_type == 1 || shape.item_type == 3, blink);
			if (!kernel_->occlusion.blink_hits_render_active(blink.hits)) {
				present_.occlusion_culled_wire.push_back(static_cast<int32_t>(handle));
				continue;
			}
			uint8_t &latch = present_.wire_occlusion_latch[handle];
			bool visible = true;
			if (es.cls == opennova::EntityClass::Player ||
					es.cls == opennova::EntityClass::Infantry) {
				const int32_t radius = kernel_->occlusion.person_collector_radius(
						shape.bound_radius_q16,
						(es.rm_entity_flags & opennova::world::kEntityFlagParachute) != 0);
				visible = kernel_->occlusion.person_render_visible(kernel_->collision, cam,
						pos, radius, latch, kernel_->world.logic_tick);
			} else {
				int32_t center_world[3] = {es.x, es.y, es.z};
				int32_t radius = 0x10000;
				if (const opennova::world::CollisionModel *model =
								kernel_->collision.model(shape.model_id);
						model != nullptr && model->valid()) {
					int32_t center_local[3];
					opennova::world::OcclusionWorld::bound_sphere_fixed(
							*model, center_local, radius, shape.uniform_scale_q16);
					opennova::world::collision_matrix_from_euler(
							es.heading_bam, es.pitch_bam, es.roll_bam, pos)
							.transform_point(center_local, center_world);
				}
				visible = kernel_->occlusion.sphere_render_visible(kernel_->collision, cam,
						center_world, radius, latch, kernel_->world.logic_tick);
			}
			if (visible) {
				// render_TOC over the row's entity+4 position, entity+0 radius,
				// model bounds and pose (skipped when contained).
				opennova::world::OcclusionWorld::TocCandidate toc;
				toc.self = h;
				toc.pos_fixed[0] = es.x;
				toc.pos_fixed[1] = es.y;
				toc.pos_fixed[2] = es.z;
				toc.radius_q16 = shape.bound_radius_q16;
				toc.model = kernel_->collision.model(shape.model_id);
				toc.heading_bam = es.heading_bam;
				toc.pitch_bam = es.pitch_bam;
				toc.roll_bam = es.roll_bam;
				visible = !kernel_->occlusion.render_wave_toc_occluded(blink.hits, toc);
			}
			if (!visible) {
				present_.occlusion_culled_wire.push_back(static_cast<int32_t>(handle));
			} else if (h.pool() == 0 && (es.net_stance_bits & 0x3u) != 0 &&
					es.carrier_handle == 0xFFFFu) {
				// A bare organics-pool row: the received MoveOrder stance bits,
				// and no carrier for the standing-on-terrain test.
				present_.foliage_mask_anchors.push_back(godot_from_fixed3(pos));
			}
		}
	}
	// The local player's body is presented outside the walks above (the local
	// view presenter), but retail's pool walk collects it like any other
	// person: the collector has no local-player exception (first person only
	// skips the body's draw later), so it takes the same gate before its
	// stance can anchor the MODEL tier.
	// [orig: collect_visible_entities_for_terrain @ 0x5c8c60 (pool walk
	// @ 0x5c8caa..0x5c8cd9, the gates @ 0x5c8cef..0x5c8eab)]
	if (opennova::world::Entity *local =
				kernel_->world.registry.get(kernel_->world.cached.local_player);
			local != nullptr && local->bms_id == 0 &&
			kernel_->occlusion.entity_render_visible(
					kernel_->world, kernel_->collision, *local, cam))
		anchor_entity(*local);
	if (runtime_profiling_enabled_)
		present_.last_occlusion_probe_us = opennova::io::perf_now_us() - occl_probe_start;
}

int64_t Simulation::building_visibility_mask(int64_t p_packed) {
	return static_cast<int64_t>(opennova::world::building_visibility_mask(p_packed));
}

bool Simulation::building_visibility_visible(int64_t p_packed) {
	return opennova::world::building_visibility_visible(p_packed);
}

// The building verdict walk, emitting only triples whose packed
// visible<<32|mask changed since the last call. The apply walks changes
// instead of the whole building set, so a steady frame does no per-building
// node work at all.
PackedInt64Array Simulation::get_building_visibility_changes() {
	PackedInt64Array out;
	if (!kernel_) return out;
	// Triples [bms_id, visible<<32 | raw mask, forced mask] for every building
	// with an OCCLUSION instance or a collision model (every retail building
	// batch member): the raw g_BuildingSectionVisMask word the other readers
	// share, and the def's forced sections the part draw ORs over it
	// (OcclusionWorld::section_draw_mask).
	kernel_->world.registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind != opennova::world::EntityKind::Building || e.bms_id == 0) return;
		if (!kernel_->occlusion.has_instance(e.handle) &&
				kernel_->collision.model_for(kernel_->world, e.handle) == nullptr)
			return;
		const bool visible = kernel_->occlusion.building_visible(e.handle);
		const int64_t packed = opennova::world::pack_building_visibility(
				kernel_->occlusion.section_mask(e.handle), visible);
		const uint32_t key = e.handle.packed;
		auto it = present_.occl_apply_building_last.find(key);
		if (it != present_.occl_apply_building_last.end() && it->second == packed) return;
		present_.occl_apply_building_last[key] = packed;
		out.push_back(e.bms_id);
		out.push_back(packed);
		out.push_back(static_cast<int64_t>(kernel_->occlusion.forced_section_mask(e.handle)));
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
	return culled_changes_since(present_.occlusion_culled_wire, present_.occl_apply_culled_wire_last);
}

PackedInt32Array Simulation::get_render_culled_changes() {
	return culled_changes_since(present_.occlusion_culled_bms, present_.occl_apply_culled_last);
}

float Simulation::sun_quality_factor(int p_quality) const {
	// quality = 4 - blocked, so the owner's factor(blocked) inverts cleanly.
	return opennova::renderer::sun_visibility_factor(4 - p_quality);
}

// The per-drawn-entity lighting feed (D-RLIT-3 plus the interior lerp): the
// walk, the two identity domains and the per-identity diff are the engine's
// (inmatch/role_feeds.h EntityLightingFeed); this leg maps the light
// direction into mission fixed and hands over the occlusion pass's culled
// identities.
const std::vector<opennova::inmatch::EntityLightingChange> &
Simulation::draw_lighting_changes(const Vector3 &p_light_dir) {
	std::vector<opennova::inmatch::EntityLightingChange> &out =
			present_.entity_lighting_changes;
	out.clear();
	if (!kernel_) return out;
	// Sun step in mission fixed: light_dir * 200 u, the same tuple mapping the
	// iris march uses.
	const int32_t sun[3] = {
		opennova::world::to_fixed(p_light_dir.x * 200.0f),
		opennova::world::to_fixed(-p_light_dir.z * 200.0f),
		opennova::world::to_fixed(p_light_dir.y * 200.0f)};
	const std::unordered_set<int32_t> culled(present_.occlusion_culled_bms.begin(),
			present_.occlusion_culled_bms.end());
	const std::unordered_set<int32_t> wire_culled(present_.occlusion_culled_wire.begin(),
			present_.occlusion_culled_wire.end());
	present_.entity_lighting.collect(role_view(), sun, culled, wire_culled,
			static_cast<int64_t>(present_.layout_revision), out);
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
	present_.occl_apply_building_last.clear();
	present_.occl_apply_culled_last.clear();
	present_.occl_apply_culled_wire_last.clear();
	present_.entity_lighting = opennova::inmatch::EntityLightingFeed();
	present_.iris_interior_group_entity = opennova::world::EntityHandle{};
	present_.iris_interior_group_section = 0;
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

int Simulation::sound_source_bms_id(uint16_t p_handle) const {
    if (!kernel_) return 0;
    const opennova::world::EntityHandle handle{p_handle};
    if (handle.valid() && handle == kernel_->world.cached.local_player) return -1;
    const auto *entity = kernel_->world.registry.get(handle);
    return entity ? entity->bms_id : 0;
}

opennova::world::EntityHandle Simulation::handle_for_bms_id(int p_bms_id) const {
	// The authored id -> handle index (world/bms_handle_index.h), rebuilt on
	// the registry's spawn serial.
	if (!kernel_) return opennova::world::EntityHandle{};
	return present_.bms_handles.resolve(kernel_->world, p_bms_id);
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
	present_.iris_interior_group_entity = march.interior_group_entity;
	present_.iris_interior_group_section = march.interior_group_section;
	for (int i = 0; i < march.count; ++i) out.append(march.samples[i]);
	return out;
}

Ref<HitboxDebugReport> Simulation::get_hitbox_debug() {
	// The hitbox view is the engine's (mission/debug_oracles.h); this marshals
	// its rows into Godot space.
	Ref<HitboxDebugReport> out;
	out.instantiate();
	if (!kernel_) return out;
	opennova::mission::DebugHitboxReport report;
	opennova::mission::collect_debug_hitboxes(*kernel_, report);
	for (const opennova::world::CollisionWorld::DebugHitboxEntity &ent : report.entities) {
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
	for (const opennova::mission::DebugHitboxOrganic &o : report.organics) {
		out->add_organic(HitboxDebugOrganic::make(static_cast<int>(o.handle.packed), o.section,
				godot_from_fixed3(o.center), static_cast<float>(o.radius_q16 / kFixed16),
				static_cast<float>(o.authored_radius_q16 / kFixed16), o.masked, o.fallback));
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
	// Every field carries its typed default (the stable-card convention); the
	// pick itself is the engine's (mission/debug_oracles.h).
	Ref<DebugPickCard> out;
	out.instantiate();
	if (!kernel_) return out;
	// Godot world (x, up, z) -> mission (x, -z, up) — the debug_spawn_round
	// conversion.
	const double from[3] = { p_from_godot.x, -static_cast<double>(p_from_godot.z), p_from_godot.y };
	const double dir[3] = { p_dir_godot.x, -static_cast<double>(p_dir_godot.z), p_dir_godot.y };
	opennova::mission::DebugPick pick;
	opennova::mission::debug_pick_entity(*kernel_, from, dir, p_max_range_units, pick);
	out->set_tick(static_cast<int64_t>(pick.tick));
	out->set_hit_position_godot(godot_from_fixed3(pick.hit_position_q16));
	out->set_hit_normal_godot(godot_from_fixed3(pick.hit_normal_q16));
	out->set_distance_units(pick.distance_units);
	out->set_section(pick.section);
	out->set_face(pick.face);
	out->set_bone(pick.bone);
	out->set_hit_zone(pick.hit_zone);
	out->set_surface_type(pick.surface_type);
	out->set_material_flags(static_cast<int64_t>(pick.material_flags));
	switch (pick.blocked) {
		case opennova::mission::DebugPick::Blocked::Terrain: out->set_blocked("terrain"); break;
		case opennova::mission::DebugPick::Blocked::Water: out->set_blocked("water"); break;
		case opennova::mission::DebugPick::Blocked::Proxy: out->set_blocked("proxy"); break;
		case opennova::mission::DebugPick::Blocked::None: break;
	}
	if (!pick.hit) return out;
	out->set_hit(true);
	switch (pick.hit_class) {
		case opennova::mission::DebugPick::HitClass::Static: out->set_hit_class("static"); break;
		case opennova::mission::DebugPick::HitClass::Dynamic: out->set_hit_class("dynamic"); break;
		default: out->set_hit_class("person"); break;
	}
	out->set_entity_handle(static_cast<int>(pick.entity.packed));
	out->set_pool(pick.entity.pool());
	out->set_kind(pick.kind);
	out->set_index(pick.index);
	out->set_bms_id(pick.bms_id);
	out->set_net_id(pick.net_id);
	out->set_item_id(pick.item_id);
	out->set_name(String(pick.name.c_str()));
	out->set_position_godot(mission_to_godot(pick.position));
	out->set_bound_radius(pick.bound_radius);
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
