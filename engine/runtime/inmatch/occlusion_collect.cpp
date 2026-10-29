// One view's entity collect (see occlusion_collect.h).
#include <runtime/inmatch/occlusion_collect.h>

#include <runtime/mission/mission_kernel.h>
#include <runtime/renderer/object_lod.h> // the render-model sphere a bare row's model leg scales
#include <runtime/replication/client_state.h>
#include <runtime/replication/client_world_materializer.h> // replica_row_entity
#include <runtime/world/collision.h> // ResolvedCollisionShape, BlinkAccum, collision_matrix_from_euler
#include <runtime/world/occlusion.h>
#include <runtime/world/occlusion_feed.h> // pack_building_visibility
#include <base/io/fixed.h> // kFp16OneD

#include <algorithm>

namespace opennova::inmatch {

void collect_view_verdicts(const PresentRowsContext &context, WireOcclusionLatchMap &r_wire_latch,
		const world::OcclusionFrameCamera &cam, std::vector<int32_t> &r_culled_bms,
		std::vector<int32_t> &r_culled_wire, std::vector<world::Vec3> &r_anchors,
		bool *r_local_collected) {
	mission::MissionKernel &kernel = context.kernel;
	ClientRuntime *const runtime = context.runtime;
	if (r_local_collected != nullptr) *r_local_collected = false;
	const bool joiner = context.joiner;
	const replication::ClientWorldMaterializer *const materializer = context.materializer;
	r_culled_bms.clear();
	// The BySide walks update the MODEL foliage tiles around the collected
	// person entities whose MoveOrder carries a stance bit (0x100 prone /
	// 0x200 crouch) and whose groundEntity is empty; the visible-entity walk
	// below is that collection, so the anchors ride its verdicts (no second
	// gate call: the latch ticks once per collected entity).
	// [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded
	// (flags & 0x300), groundEntity gate @ 0x5c7dd5..0x5c7df7; stance writers
	// Player_PackInputStateToEntity @ 0x4df6a7..0x4df6cd,
	// NapiNPServerMsg_HandleStanceChange @ 0x501c60]
	r_anchors.clear();
	const auto anchor_entity = [&](const world::Entity &e) {
		if (e.kind != world::EntityKind::Organic ||
				(e.net_stance_bits & 0x3u) == 0 || e.ground_target.valid())
			return;
		r_anchors.push_back(e.position);
	};
	// A placed row the walks gate: every row but the Building-type defs (they
	// take the OcclusionWorld's batch verdicts) and the markers, the pool-2
	// decorations and foliage included; a row with no placed identity is gated
	// only as a wire row or as the local player (wire avatars ride their own
	// present path).
	const auto placed_row = [](const world::Entity &e) {
		return !(e.kind == world::EntityKind::Building && world::building_def_row(e)) &&
				e.kind != world::EntityKind::Marker && e.bms_id != 0;
	};
	// `static_row` is the static prox table row a static is walked from (the
	// statics' box reads its words), null for a pool 0 / 1 row.
	const auto gate_placed = [&](world::Entity &e,
			const world::CollisionWorld::StaticSlotView *static_row) {
		if (!kernel.occlusion.entity_render_visible(kernel.world, kernel.collision, e, cam,
					static_row))
			r_culled_bms.push_back(e.bms_id);
		else
			anchor_entity(e);
	};
	// The decoded rows the wire pass draws — remote organics and runtime
	// spawns with no placed identity — pass the SAME collector gate: retail's
	// client walks the pool entities it built from the wire exactly as the
	// host walks its own (the witness lives on OcclusionWorld::
	// sphere_render_visible). A row with a registry twin (the host's own
	// runtime spawns) takes the twin's live verdict, exactly like the placed
	// rows; a bare row without a usable collision model takes its
	// type's render-model sphere, and one without a render model is never
	// collected.
	r_culled_wire.clear();
	const uint16_t self_handle = runtime != nullptr && runtime->has_self_handle()
			? runtime->self_handle()
			: world::EntityHandle::kInvalid;
	const auto gate_wire = [&](const replication::ClientEntityState &es) {
		const uint16_t handle = es.handle;
		if (handle == world::EntityHandle::kInvalid ||
				es.type_id == 0 || handle == self_handle)
			return;
		// A hidden row is never collected; the present pass hides it
		// itself, and its latch does not tick (the gate's bit0 test in
		// OcclusionWorld::entity_render_visible).
		if (es.state_flags_known && (es.state_flags & 0x01u) != 0) return;
		// A joiner's twin is the decoded row's own native row by the replica
		// rule, never a later same-type occupant of the handle (D-NET-676).
		const world::EntityHandle h{handle};
		world::Entity *twin = !joiner || h.pool() != 0
				? replication::replica_row_entity(materializer, kernel.world, h,
						es.type_id)
				: nullptr;
		if (twin != nullptr && (twin->bms_id != 0 ||
				twin->spawn_origin != world::kSpawnOriginNone))
			return; // a placed row: its own leg gates it
		// The listen host's own player at its own handle; a joiner's self row
		// went above by the self handle, and its H is never compared with L
		// (D-NET-698): retail collects every drawn entity it built from the
		// wire [orig: Terrain_CollectVisibleEntities_0 @0x5C6F20].
		if (!joiner && h == kernel.world.cached.local_player) return;
		if (twin != nullptr) {
			// The host's own runtime spawn (an addeweap gun child, a
			// runtime-placed item): the listen host walks its OWN pool entity
			// with the same live pose the placed leg uses. Its decoded
			// row is a spawn image — the loopback 0x0A is header-only, so
			// es.x/y/z never follow a moving carrier, and a sphere pinned
			// there culled the gun the moment the driven buggy left it.
			if (!kernel.occlusion.entity_render_visible(
						kernel.world, kernel.collision, *twin, cam))
				r_culled_wire.push_back(static_cast<int32_t>(handle));
			else
				anchor_entity(*twin);
			return;
		}
		// A bare row is the client-built pool entity retail's collector
		// walks: its type's entity+0 bound radius from the shared items.def/
		// model resolve (the replica pipeline's own source), through the
		// person leg for Player/Infantry rows (the parachute flag swaps in
		// the item-185 radius) or the model leg's bound sphere placed by the
		// row's full Euler pose.
		const world::ResolvedCollisionShape shape =
				kernel.wire_collision_shape_for_type(es.type_id);
		// The entity+0x30 model gate, every row ahead of the type split, a
		// person row included: the collision model's bounds, else the render
		// model's CMDL sphere; a row whose graphic did not load has no render
		// model and is never collected, its latch never started
		// (world/occlusion.h assign_render_model and
		// OcclusionWorld::entity_render_visible carry the witness, the same rule
		// the placed rows take).
		const world::CollisionModel *model = kernel.collision.model(shape.model_id);
		const bool collision_bounds = model != nullptr && model->valid();
		if (!collision_bounds && !shape.has_render_model) {
			r_culled_wire.push_back(static_cast<int32_t>(handle));
			return;
		}
		// The walk's Flags match (the water mirror's collect alone sets one)
		// over the client-built entity's Flags word, the vehicle init's
		// Reflective bit included, ahead of the blink gate and the latch
		// (world/occlusion.h collect_flags_match carries the witness).
		if (!world::OcclusionWorld::collect_flags_match(cam,
					es.rm_entity_flags |
							(shape.item_type == 1 ? world::kEntityFlagReflective : 0u))) {
			r_culled_wire.push_back(static_cast<int32_t>(handle));
			return;
		}
		const int32_t pos[3] = {es.x, es.y, es.z};
		// The XY fog box over the client-built entity's entity+0 radius and
		// position, ahead of the blink walk (world/occlusion.h collect_fog_box
		// carries the witness).
		if (!world::OcclusionWorld::collect_fog_box(cam, pos[0], pos[1],
					shape.bound_radius_q16)) {
			r_culled_wire.push_back(static_cast<int32_t>(handle));
			return;
		}
		// The row's blink quad from the client's own blink walk (the one
		// the lighting feed runs for a twin-less row): the collector's
		// blink-hits gate reads it before the legs, and the render waves'
		// contained test after them.
		world::BlinkAccum blink;
		const bool person_source = h.pool() == 0 &&
				(es.cls == EntityClass::Player ||
						es.cls == EntityClass::Infantry);
		kernel.collision.query_wire_blink_boxes_at_point(kernel.world, handle, pos,
				person_source || shape.item_type == 1 || shape.item_type == 3, blink);
		if (!kernel.occlusion.blink_hits_render_active(blink.hits)) {
			r_culled_wire.push_back(static_cast<int32_t>(handle));
			return;
		}
		uint8_t &latch = r_wire_latch[handle];
		bool visible = true;
		if (es.cls == EntityClass::Player ||
				es.cls == EntityClass::Infantry) {
			const int32_t radius = kernel.occlusion.person_collector_radius(
					shape.bound_radius_q16,
					(es.rm_entity_flags & world::kEntityFlagParachute) != 0);
			visible = kernel.occlusion.person_render_visible(kernel.collision, cam,
					pos, radius, latch, kernel.world.logic_tick);
		} else {
			// The model leg: the collision model's bounds, else the render
			// model's CMDL sphere (radius 0 without the block).
			int32_t center_local[3] = {0, 0, 0};
			int32_t radius = 0;
			if (collision_bounds) {
				world::OcclusionWorld::bound_sphere_fixed(
						*model, center_local, radius, shape.uniform_scale_q16);
			} else {
				renderer::ObjectProjectionSphere sphere;
				sphere.valid = true;
				sphere.center_q16 = {shape.render_sphere_center_q16.x,
						shape.render_sphere_center_q16.y, shape.render_sphere_center_q16.z};
				sphere.radius_q16 = shape.render_sphere_radius_q16;
				sphere = renderer::scale_object_projection_sphere_q16(
						sphere, shape.uniform_scale_q16);
				for (int axis = 0; axis < 3; ++axis) center_local[axis] = sphere.center_q16[axis];
				radius = sphere.radius_q16;
			}
			int32_t center_world[3];
			world::collision_matrix_from_euler(
					es.heading_bam, es.pitch_bam, es.roll_bam, pos)
					.transform_point(center_local, center_world);
			visible = kernel.occlusion.sphere_render_visible(kernel.collision, cam,
					center_world, radius, latch, kernel.world.logic_tick);
		}
		if (visible) {
			// render_TOC over the row's entity+4 position, entity+0 radius,
			// model bounds and pose (skipped when contained).
			world::OcclusionWorld::TocCandidate toc;
			toc.self = h;
			toc.pos_fixed[0] = es.x;
			toc.pos_fixed[1] = es.y;
			toc.pos_fixed[2] = es.z;
			toc.radius_q16 = shape.bound_radius_q16;
			toc.model = model;
			toc.heading_bam = es.heading_bam;
			toc.pitch_bam = es.pitch_bam;
			toc.roll_bam = es.roll_bam;
			visible = !kernel.occlusion.render_wave_toc_occluded(blink.hits, toc);
		}
		if (!visible) {
			r_culled_wire.push_back(static_cast<int32_t>(handle));
		} else if (h.pool() == 0 && (es.net_stance_bits & 0x3u) != 0 &&
				es.carrier_handle == 0xFFFFu) {
			// A bare organics-pool row: the received MoveOrder stance bits,
			// and no carrier for the standing-on-terrain test.
			r_anchors.push_back(world::Vec3{static_cast<float>(pos[0] / io::kFp16OneD),
					static_cast<float>(pos[1] / io::kFp16OneD),
					static_cast<float>(pos[2] / io::kFp16OneD)});
		}
	};
	if (runtime != nullptr) {
		// A latch belongs to one row lifetime: drop the counters of handles
		// that left the state so a reused handle starts fresh (retail memsets
		// the destroyed entity, latch included). Each latch's row is found by
		// its handle (ClientState::find's per-handle row), with no set built
		// over every row each frame (D-NET-1353, LF-s18).
		const replication::ClientState &state = runtime->state();
		for (auto it = r_wire_latch.begin();
				it != r_wire_latch.end();) {
			if (state.find(it->first) == nullptr)
				it = r_wire_latch.erase(it);
			else
				++it;
		}
	}

	// The walk order is the collectors': the statics first (the static prox
	// table's tail, the pool-2 rows that are not Building-type defs, in pool-2
	// slot order), then pool 0, then pool 1, each pool in slot order. Every
	// re-arm draws the one PRNG stream, so the order decides which entity takes
	// which draw. [orig: Terrain_CollectVisibleEntities @ 0x5c9160 -
	// Terrain_CollectVisibleEntities_0 @ 0x5c9199, Terrain_CollectVisibleEntitiesForTerrain
	// pool 0 @ 0x5c91a1, pool 1 @ 0x5c91aa (the push 1 @ 0x5c91a8); the mirror's
	// Terrain_CollectVisibleEntitiesForReflection @ 0x5c913d / @ 0x5c9145 / @ 0x5c914e;
	// ForTerrain's slot walk @ 0x5c8caa..0x5c8ced]
	// The statics' walk is the table itself, rows [static_building_count, static_count)
	// as its last Pool2 build listed them (at most kStaticProxCountBound rows, the
	// Building-type ones first; world/collision.h static_prox_listed carries the
	// witness), each row's entity gated with the row, whose quantized words its box reads.
	// [orig: Terrain_CollectVisibleEntities_0 @ 0x5c6f20 - the walk
	//  g_StaticProxBuildingCount..g_StaticProxCount @ 0x5c6f48..0x5c721e, the row's box
	//  @ 0x5c6f82..0x5c6fcb ahead of its entity g_StaticProxEntity[i] @ 0x5c6fd1; the
	//  table's pass 2, Entity_BuildProximityLists_Pool2 @ 0x4b9430 @ 0x4b94e0..0x4b9568]
	// The lists hold what the walks ADMIT and a pass draws only them, so a placed
	// pool-2 row the table does not list (spawned since its last build, past its
	// bound, or with no item type) is in no list and never drawn: it drops as a
	// culled row does. Every pool-0 / pool-1 row is walked below.
	// [orig: Terrain_CollectVisibleEntities @ 0x5c9160 - g_SectorEntityCount zeroed
	//  @ 0x5c9177; Terrain_CollectVisibleEntities_0 @ 0x5c6f20 - its appends
	//  @ 0x5c71bf / @ 0x5c7207; Terrain_RenderSectorEntities @ 0x5c7b60 - the draw's
	//  walk to the count @ 0x5c7b65]
	static thread_local std::vector<uint8_t> statics_walked;
	statics_walked.assign(kernel.world.registry.pool_capacity(2), 0);
	const int32_t statics = kernel.collision.static_count();
	for (int32_t i = kernel.collision.static_building_count(); i < statics; ++i) {
		const world::CollisionWorld::StaticSlotView row = kernel.collision.static_slot(i);
		world::Entity *e = kernel.world.registry.get(row.h);
		if (e != nullptr && placed_row(*e)) {
			if (static_cast<size_t>(row.h.slot()) < statics_walked.size())
				statics_walked[static_cast<size_t>(row.h.slot())] = 1;
			gate_placed(*e, &row);
		}
	}
	kernel.world.registry.for_each_in_pool(2, [&](const world::Entity &e) {
		const size_t slot = static_cast<size_t>(e.handle.slot());
		if (placed_row(e) && (slot >= statics_walked.size() || statics_walked[slot] == 0))
			r_culled_bms.push_back(e.bms_id);
	});

	// Pools 0 and 1: the placed rows, the decoded wire rows and the local player
	// each at its own pool slot. A wire row's slot is the one its handle names
	// (the client builds each record's entity at g_PoolList[handle >> 12], slot
	// handle & 0xFFF), so a joiner's local player sits at its self handle. A
	// wire row of another pool follows in handle order (no stock producer
	// streams one bare: the materializer twins them).
	// [orig: NapiNPClientMsg_0x00C @ 0x42e730 - the slot @ 0x42e7bf..0x42e7dd,
	//  the memset @ 0x42e7f1]
	enum class Leg : uint8_t { Placed, Wire, Local };
	struct Visit {
		uint16_t slot;
		Leg leg;
		uint32_t index;
		uint32_t order; // the visit's place in the lists below: a slot's visits keep it
	};
	// The visit list is the walk's own scratch, kept across calls (every view
	// of every frame walks it), so a frame's walk allocates nothing; the walks
	// run one at a time on the frame's thread (D-NET-1353, LF-s18).
	static thread_local std::vector<Visit> visits;
	visits.clear();
	for (const int pool : {0, 1})
		kernel.world.registry.for_each_in_pool(pool, [&](const world::Entity &e) {
			if (placed_row(e))
				visits.push_back({e.handle.packed, Leg::Placed, e.handle.packed,
						static_cast<uint32_t>(visits.size())});
		});
	if (runtime != nullptr) {
		const std::vector<replication::ClientEntityState> &rows = runtime->state().entities;
		for (uint32_t i = 0; i < rows.size(); ++i)
			visits.push_back({rows[i].handle, Leg::Wire, i, static_cast<uint32_t>(visits.size())});
	}
	// The local player's body is presented outside the walks above (the local
	// view presenter), but retail's pool walk collects it like any other
	// person: the collector has no local-player exception (first person only
	// skips the body's draw later), so it takes the same gate before its
	// stance can anchor the MODEL tier.
	// [orig: Terrain_CollectVisibleEntitiesForTerrain @ 0x5c8c60 (pool walk
	// @ 0x5c8caa..0x5c8cd9, the gates @ 0x5c8cef..0x5c8eab)]
	const world::EntityHandle local_handle = kernel.world.cached.local_player;
	if (const world::Entity *local = kernel.world.registry.get(local_handle);
			local != nullptr && local->bms_id == 0)
		visits.push_back({joiner && self_handle != world::EntityHandle::kInvalid
						? self_handle
						: local_handle.packed,
				Leg::Local, 0, static_cast<uint32_t>(visits.size())});
	// Slot order, a slot's visits in their list order (the order a stable sort
	// keeps), sorted in place.
	std::sort(visits.begin(), visits.end(), [](const Visit &a, const Visit &b) {
		return a.slot != b.slot ? a.slot < b.slot : a.order < b.order;
	});
	for (const Visit &v : visits) {
		switch (v.leg) {
			case Leg::Placed:
				if (world::Entity *e = kernel.world.registry.get(
							world::EntityHandle{static_cast<uint16_t>(v.index)}))
					gate_placed(*e, nullptr);
				break;
			case Leg::Wire:
				gate_wire(runtime->state().entities[v.index]);
				break;
			case Leg::Local:
				if (world::Entity *local = kernel.world.registry.get(local_handle);
						local != nullptr &&
						kernel.occlusion.entity_render_visible(
								kernel.world, kernel.collision, *local, cam)) {
					anchor_entity(*local);
					if (r_local_collected != nullptr) *r_local_collected = true;
				}
				break;
		}
	}
}

void collect_building_verdicts(const world::World &world, const world::CollisionWorld &collision,
		const world::OcclusionWorld &occlusion, std::vector<BuildingVerdict> &r_out) {
	r_out.clear();
	world.registry.for_each([&](const world::Entity &e) {
		if (e.kind != world::EntityKind::Building || e.bms_id == 0) return;
		if (!world::building_def_row(e)) return;
		if (!occlusion.has_instance(e.handle) && collision.model_for(world, e.handle) == nullptr)
			return;
		BuildingVerdict v;
		v.bms_id = e.bms_id;
		v.handle = e.handle.packed;
		v.packed = world::pack_building_visibility(occlusion.section_mask(e.handle),
				occlusion.building_visible(e.handle));
		v.forced = static_cast<int64_t>(occlusion.forced_section_mask(e.handle));
		r_out.push_back(v);
	});
}

bool collect_mirror_view(const PresentRowsContext &context, WireOcclusionLatchMap &r_wire_latch,
		const world::OcclusionFrameCamera &cam, MirrorViewLists &r_lists) {
	mission::MissionKernel &kernel = context.kernel;
	r_lists.water_visible = kernel.occlusion.mirror_water_visible(kernel.world, kernel.collision,
			cam, [&](const world::OcclusionFrameCamera &mirror) {
				collect_view_verdicts(context, r_wire_latch, mirror, r_lists.culled_bms,
						r_lists.culled_wire, r_lists.anchors, &r_lists.local_collected);
				// The reflected subscene draws the collect's own batch at its own
				// section masks [orig: Water_RenderReflectedWorldScene @ 0x5c8510 -
				// Terrain_RenderSectorModels @ 0x5c8576].
				collect_building_verdicts(kernel.world, kernel.collision, kernel.occlusion,
						r_lists.buildings);
			});
	return r_lists.water_visible;
}

} // namespace opennova::inmatch
