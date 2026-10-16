// One view's entity collect (see occlusion_collect.h).
#include <runtime/inmatch/occlusion_collect.h>

#include <runtime/mission/mission_kernel.h>
#include <runtime/renderer/object_lod.h> // the render-model sphere a bare row's model leg scales
#include <runtime/replication/client_state.h>
#include <runtime/replication/client_world_materializer.h> // replica_row_entity
#include <runtime/world/collision.h> // ResolvedCollisionShape, BlinkAccum, collision_matrix_from_euler
#include <runtime/world/occlusion.h>
#include <base/io/fixed.h> // kFp16OneD

#include <unordered_set>

namespace opennova::inmatch {

void collect_view_verdicts(const PresentRowsContext &context, WireOcclusionLatchMap &r_wire_latch,
		std::vector<world::EntityHandle> &r_handles, const world::OcclusionFrameCamera &cam,
		std::vector<int32_t> &r_culled_bms, std::vector<int32_t> &r_culled_wire,
		std::vector<world::Vec3> &r_anchors) {
	mission::MissionKernel &kernel = context.kernel;
	ClientRuntime *const runtime = context.runtime;
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
	std::vector<world::EntityHandle> &handles = r_handles;
	handles.clear();
	kernel.world.registry.for_each([&](const world::Entity &e) {
		// Every row but the Building-type defs (they take the OcclusionWorld's
		// batch verdicts): the pool-2 decorations and foliage are
		// entity-collected too.
		if ((e.kind == world::EntityKind::Building &&
		     world::building_def_row(e)) ||
		    e.kind == world::EntityKind::Marker)
			return;
		if (e.bms_id == 0) return; // wire avatars ride their own present path
		handles.push_back(e.handle);
	});
	for (const world::EntityHandle h : handles) {
		world::Entity *e = kernel.world.registry.get(h);
		if (e == nullptr) continue;
		if (!kernel.occlusion.entity_render_visible(kernel.world, kernel.collision, *e, cam))
			r_culled_bms.push_back(e->bms_id);
		else
			anchor_entity(*e);
	}
	// The decoded rows the wire pass draws — remote organics and runtime
	// spawns with no placed identity — pass the SAME collector gate: retail's
	// client walks the pool entities it built from the wire exactly as the
	// host walks its own (the witness lives on OcclusionWorld::
	// sphere_render_visible). A row with a registry twin (the host's own
	// runtime spawns) takes the twin's live verdict, exactly like the placed
	// rows above; a bare row without a usable collision model takes its
	// type's render-model sphere, and one without a render model is never
	// collected.
	r_culled_wire.clear();
	if (runtime != nullptr) {
		// A latch belongs to one row lifetime: drop the counters of handles
		// that left the state so a reused handle starts fresh (retail memsets
		// the destroyed entity, latch included).
		std::unordered_set<uint16_t> live_handles;
		for (const replication::ClientEntityState &es :
				runtime->state().entities)
			live_handles.insert(es.handle);
		for (auto it = r_wire_latch.begin();
				it != r_wire_latch.end();) {
			if (live_handles.count(it->first) == 0)
				it = r_wire_latch.erase(it);
			else
				++it;
		}
		const uint16_t self_handle = runtime->has_self_handle()
				? runtime->self_handle()
				: world::EntityHandle::kInvalid;
		for (const replication::ClientEntityState &es :
				runtime->state().entities) {
			const uint16_t handle = es.handle;
			if (handle == world::EntityHandle::kInvalid ||
					es.type_id == 0 || handle == self_handle)
				continue;
			// A hidden row is never collected; the present pass hides it
			// itself, and its latch does not tick (the gate's bit0 test in
			// OcclusionWorld::entity_render_visible).
			if (es.state_flags_known && (es.state_flags & 0x01u) != 0) continue;
			// A joiner's twin is the decoded row's own native row by the replica
			// rule, never a later same-type occupant of the handle (D-NET-676).
			const world::EntityHandle h{handle};
			world::Entity *twin = !joiner || h.pool() != 0
					? replication::replica_row_entity(materializer, kernel.world, h,
							es.type_id, kernel.wire_header_world)
					: nullptr;
			if (twin != nullptr && (twin->bms_id != 0 ||
					twin->spawn_origin != world::kSpawnOriginNone))
				continue; // a placed row: the registry walk above gated it
			if (h == kernel.world.cached.local_player) continue;
			if (twin != nullptr) {
				// The host's own runtime spawn (an addeweap gun child, a
				// runtime-placed item): the listen host walks its OWN pool entity
				// with the same live pose the placed walk above uses. Its decoded
				// row is a spawn image — the loopback 0x0A is header-only, so
				// es.x/y/z never follow a moving carrier, and a sphere pinned
				// there culled the gun the moment the driven buggy left it.
				if (!kernel.occlusion.entity_render_visible(
							kernel.world, kernel.collision, *twin, cam))
					r_culled_wire.push_back(static_cast<int32_t>(handle));
				else
					anchor_entity(*twin);
				continue;
			}
			// A bare row is the client-built pool entity retail's collector
			// walks: its type's entity+0 bound radius from the shared items.def/
			// model resolve (the replica pipeline's own source), through the
			// person leg for Player/Infantry rows (the parachute flag swaps in
			// the item-185 radius) or the model leg's bound sphere placed by the
			// row's full Euler pose.
			const world::ResolvedCollisionShape shape =
					kernel.wire_collision_shape_for_type(es.type_id);
			const int32_t pos[3] = {es.x, es.y, es.z};
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
				continue;
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
				// model's CMDL sphere (radius 0 without the block); a row whose
				// graphic did not load has no render model and is never
				// collected (world/occlusion.h assign_render_model carries the
				// witness, the same rule the placed rows take).
				int32_t center_local[3] = {0, 0, 0};
				int32_t radius = 0;
				if (const world::CollisionModel *model =
								kernel.collision.model(shape.model_id);
						model != nullptr && model->valid()) {
					world::OcclusionWorld::bound_sphere_fixed(
							*model, center_local, radius, shape.uniform_scale_q16);
				} else if (shape.has_render_model) {
					renderer::ObjectProjectionSphere sphere;
					sphere.valid = true;
					sphere.center_q16 = {shape.render_sphere_center_q16.x,
							shape.render_sphere_center_q16.y, shape.render_sphere_center_q16.z};
					sphere.radius_q16 = shape.render_sphere_radius_q16;
					sphere = renderer::scale_object_projection_sphere_q16(
							sphere, shape.uniform_scale_q16);
					for (int axis = 0; axis < 3; ++axis) center_local[axis] = sphere.center_q16[axis];
					radius = sphere.radius_q16;
				} else {
					r_culled_wire.push_back(static_cast<int32_t>(handle));
					continue;
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
				toc.model = kernel.collision.model(shape.model_id);
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
		}
	}
	// The local player's body is presented outside the walks above (the local
	// view presenter), but retail's pool walk collects it like any other
	// person: the collector has no local-player exception (first person only
	// skips the body's draw later), so it takes the same gate before its
	// stance can anchor the MODEL tier.
	// [orig: Terrain_CollectVisibleEntitiesForTerrain @ 0x5c8c60 (pool walk
	// @ 0x5c8caa..0x5c8cd9, the gates @ 0x5c8cef..0x5c8eab)]
	if (world::Entity *local =
				kernel.world.registry.get(kernel.world.cached.local_player);
			local != nullptr && local->bms_id == 0 &&
			kernel.occlusion.entity_render_visible(
					kernel.world, kernel.collision, *local, cam))
		anchor_entity(*local);
}

} // namespace opennova::inmatch
