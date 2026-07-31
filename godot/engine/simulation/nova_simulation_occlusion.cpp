// NovaSimulation — the occlusion runtime (building portals, iris march, sound
// occlusion) and the world debug views (collision/hitbox/round/occlusion
// dictionaries + debug round spawn).
#include "simulation/nova_simulation_internal.h"

using namespace novasim;

void NovaSimulation::occlusion_init_mission() {
	// [orig: Terrain_InitBuildingPortals @ 0x5c7480 from Game_StartMission
	// @ 0x525e11 — runs over the static prox tables, so make sure they exist
	// before the register pass walks the building prefix.]
	if (!world_) return;
	collision_world_.build_initial_tables(*world_);
	occlusion_world_.init_mission(*world_, collision_world_);
}

void NovaSimulation::run_occlusion_frame(const Transform3D &p_camera, double p_fov_y_deg,
                                         double p_aspect, double p_near,
                                         double p_fog_dist_units, double p_water_z_units,
                                         bool p_force_indoors) {
	if (!world_) return;
	using opennova::world::to_fixed;
	opennova::world::OcclusionFrameCamera cam;

	// Godot world (x, up, z) -> mission fixed (x, -z, up) 16.16.
	const Vector3 gp = p_camera.origin;
	cam.pos_fixed[0] = to_fixed(gp.x);
	cam.pos_fixed[1] = to_fixed(-gp.z);
	cam.pos_fixed[2] = to_fixed(gp.y);
	opennova::world::render_float_from_fixed(cam.pos_fixed, cam.pos_float);

	// Camera axes. Godot camera looks -Z; render float = Godot with X/Z swapped
	// ((-my, mz, mx)/65536 == (gz, gy, gx)); mission dirs = (x, -z, y) of Godot.
	const Vector3 fwd_g = -p_camera.basis.get_column(2).normalized();
	const Vector3 right_g = p_camera.basis.get_column(0).normalized();
	const Vector3 up_g = p_camera.basis.get_column(1).normalized();
	auto render_dir = [](const Vector3 &v) {
		return Vector3(v.z, v.y, v.x);
	};
	const Vector3 f = render_dir(fwd_g);
	const Vector3 r = render_dir(right_g);
	const Vector3 u = render_dir(up_g);
	const Vector3 c(cam.pos_float[0], cam.pos_float[1], cam.pos_float[2]);

	// The 5-plane view frustum (near + 4 sides), inward normals, in render
	// float space — the reimpl stand-in for the retail viewport projector
	// [orig: g_CameraFrustumPlanes5 @ 0xA7849C; D-OCC-12].
	const double half_v = Math::deg_to_rad(p_fov_y_deg) * 0.5;
	const double tan_v = std::tan(half_v);
	const double tan_h = tan_v * (p_aspect > 0.0 ? p_aspect : 1.0);
	Vector3 normals[5];
	normals[0] = f;
	normals[1] = (f * static_cast<real_t>(tan_h) + r).normalized();  // left
	normals[2] = (f * static_cast<real_t>(tan_h) - r).normalized();  // right
	normals[3] = (f * static_cast<real_t>(tan_v) + u).normalized();  // bottom
	normals[4] = (f * static_cast<real_t>(tan_v) - u).normalized();  // top
	cam.frustum_count = 5;
	for (int i = 0; i < 5; ++i) {
		const Vector3 anchor = (i == 0) ? c + f * static_cast<real_t>(p_near) : c;
		cam.frustum[i][0] = normals[i].x;
		cam.frustum[i][1] = normals[i].y;
		cam.frustum[i][2] = normals[i].z;
		cam.frustum[i][3] = -normals[i].dot(anchor);
	}

	// World->view rotation rows (mission axes, Q22): row 0 = forward (the depth
	// cull axis), rows 1/2 = the lateral axes the three-ray probe offsets along.
	// [orig: the fixed view matrix @ 0xA7841C]
	auto mission_dir_q22 = [](const Vector3 &v, int32_t out[3]) {
		out[0] = static_cast<int32_t>(std::lround(v.x * 4194304.0));
		out[1] = static_cast<int32_t>(std::lround(-v.z * 4194304.0));
		out[2] = static_cast<int32_t>(std::lround(v.y * 4194304.0));
	};
	mission_dir_q22(fwd_g, cam.view_rows_q22[0]);
	mission_dir_q22(right_g, cam.view_rows_q22[1]);
	mission_dir_q22(up_g, cam.view_rows_q22[2]);

	cam.fog_dist = to_fixed(p_fog_dist_units);
	cam.water_z = to_fixed(p_water_z_units);
	// The mission-attribute force-indoors override ORs the indoors bit into the
	// frame's accum view. [orig: Bms_AttribFlags & 0x10 @ 0x5ca1c8 -> |= 2]
	cam.local_blink_flags =
			collision_world_.local_player_blink_flags |
			(p_force_indoors ? opennova::world::kBlinkIndoorsBit : 0u);

	const uint64_t occl_build_start =
			runtime_profiling_enabled_ ? perf_now_us() : 0;
	occlusion_world_.build_frame(*world_, collision_world_, cam);
	const uint64_t occl_probe_start =
			runtime_profiling_enabled_ ? perf_now_us() : 0;
	if (runtime_profiling_enabled_)
		last_occlusion_build_us_ = occl_probe_start - occl_build_start;

	// The entity collectors' render gates over the non-building entities the
	// host draws. [orig: Terrain_CollectVisibleEntities_0 @ 0x5c6f20 /
	// collect_visible_entities_for_terrain @ 0x5c8c60]
	occlusion_culled_bms_.clear();
	std::vector<opennova::world::EntityHandle> handles;
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind == opennova::world::EntityKind::Building ||
		    e.kind == opennova::world::EntityKind::Marker)
			return;
		if (e.bms_id == 0) return; // wire avatars ride their own present path
		handles.push_back(e.handle);
	});
	for (const opennova::world::EntityHandle h : handles) {
		opennova::world::Entity *e = world_->registry.get(h);
		if (e == nullptr) continue;
		if (!occlusion_world_.entity_render_visible(*world_, collision_world_, *e, cam))
			occlusion_culled_bms_.push_back(e->bms_id);
	}
	if (runtime_profiling_enabled_)
		last_occlusion_probe_us_ = perf_now_us() - occl_probe_start;
}

PackedInt64Array NovaSimulation::get_building_visibility() const {
	PackedInt64Array out;
	if (!world_) return out;
	// Pairs [bms_id, visible<<32 | mask] for every building with an OCCLUSION
	// instance, plus collision-backed de-batched buildings that still entered
	// the retail building batch. OOBJ instances apply their section mask.
	// Without OOBJ there is no safe reimpl part-to-section map, so those buildings
	// keep all render parts while still receiving batch/frustum/TOC visibility.
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind != opennova::world::EntityKind::Building || e.bms_id == 0) return;
		const bool has_occlusion = occlusion_world_.has_instance(e.handle);
		if (!has_occlusion &&
				collision_world_.model_for(*world_, e.handle) == nullptr)
			return;
		const bool visible = occlusion_world_.building_visible(e.handle);
		const uint32_t mask =
		    has_occlusion ? occlusion_world_.section_mask(e.handle) : 0xFFFFFFFFu;
		out.push_back(e.bms_id);
		out.push_back(static_cast<int64_t>(mask) | (visible ? (int64_t(1) << 32) : 0));
	});
	return out;
}

PackedInt32Array NovaSimulation::get_render_culled_bms_ids() const {
	PackedInt32Array out;
	for (const int32_t id : occlusion_culled_bms_) out.push_back(id);
	return out;
}

// Same verdict walk as get_building_visibility(), emitting only pairs whose
// packed visible<<32|mask changed since the last call. The GDScript apply
// walks changes instead of the whole building set, so a steady frame does no
// per-building node work at all.
PackedInt64Array NovaSimulation::get_building_visibility_changes() {
	PackedInt64Array out;
	if (!world_) return out;
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind != opennova::world::EntityKind::Building || e.bms_id == 0) return;
		const bool has_occlusion = occlusion_world_.has_instance(e.handle);
		if (!has_occlusion &&
				collision_world_.model_for(*world_, e.handle) == nullptr)
			return;
		const bool visible = occlusion_world_.building_visible(e.handle);
		const uint32_t mask =
		    has_occlusion ? occlusion_world_.section_mask(e.handle) : 0xFFFFFFFFu;
		const int64_t packed =
				static_cast<int64_t>(mask) | (visible ? (int64_t(1) << 32) : 0);
		const uint32_t key = e.handle.packed;
		auto it = occl_apply_building_last_.find(key);
		if (it != occl_apply_building_last_.end() && it->second == packed) return;
		occl_apply_building_last_[key] = packed;
		out.push_back(e.bms_id);
		out.push_back(packed);
	});
	return out;
}

PackedInt32Array NovaSimulation::get_render_culled_changes() {
	std::vector<int32_t> current = occlusion_culled_bms_;
	std::sort(current.begin(), current.end());
	std::vector<int32_t> added;
	std::vector<int32_t> removed;
	std::set_difference(current.begin(), current.end(),
			occl_apply_culled_last_.begin(), occl_apply_culled_last_.end(),
			std::back_inserter(added));
	std::set_difference(occl_apply_culled_last_.begin(),
			occl_apply_culled_last_.end(), current.begin(), current.end(),
			std::back_inserter(removed));
	occl_apply_culled_last_ = std::move(current);
	PackedInt32Array out;
	out.push_back(static_cast<int32_t>(added.size()));
	for (const int32_t id : added) out.push_back(id);
	out.push_back(static_cast<int32_t>(removed.size()));
	for (const int32_t id : removed) out.push_back(id);
	return out;
}

// Mirrors the PF_HIDDEN row source (ent->hidden). The local-view-suppression
// and joiner lifecycle folds only apply to rows without a bms_id, which the
// occlusion frame never manages, so plain hidden is the whole intent here.
bool NovaSimulation::entity_present_visible(int p_bms_id) const {
	if (!world_ || p_bms_id == 0) return true;
	bool visible = true;
	bool found = false;
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (found || e.bms_id != p_bms_id) return;
		found = true;
		visible = !e.hidden;
	});
	return visible;
}

void NovaSimulation::reset_occlusion_apply_baseline() {
	occl_apply_building_last_.clear();
	occl_apply_culled_last_.clear();
}

bool NovaSimulation::occlusion_water_visible() const {
	return occlusion_world_.water_visible();
}

bool NovaSimulation::occlusion_camera_indoors() const {
	return occlusion_world_.camera_indoors();
}

bool NovaSimulation::local_player_indoors() const {
	if (!world_) return false;
	const opennova::world::Entity *e = world_->registry.get(world_->cached.local_player);
	return e != nullptr && (e->flags & opennova::world::kEntityFlagIndoors) != 0;
}

static_assert(NovaSimulation::BLINK_INDOORS == opennova::world::kBlinkIndoorsBit,
              "BLINK_INDOORS drifted from collision.h");
static_assert(NovaSimulation::BLINK_WATER_OFF == opennova::world::kBlinkWaterOffBit,
              "BLINK_WATER_OFF drifted from collision.h");

int NovaSimulation::local_player_blink_flags() const {
	return static_cast<int>(collision_world_.local_player_blink_flags);
}

int NovaSimulation::local_player_interior_item_id() const {
	if (!world_) return 0;
	const opennova::world::Entity *player =
			world_->registry.get(world_->cached.local_player);
	if (player == nullptr || player->blink_hits[0] == 0) return 0;
	const opennova::world::EntityHandle building =
			opennova::world::EntityHandle::make(
					2, static_cast<int32_t>(player->blink_hits[0] >> 20));
	const opennova::world::Entity *parent = world_->registry.get(building);
	return parent == nullptr
			? 0
			: static_cast<int>(parent->item_id)
					+ opennova::mission::kItemIdOffset;
}

int64_t NovaSimulation::sound_occlusion_distance_q16(const Vector3 &listener_pos,
                                                     const Vector3 &source_pos,
                                                     int64_t distance_q16,
                                                     int source_bms_id) {
	// [orig: Sound_ApplyOcclusionDistance @ 0x529970] — the audio layer feeds
	// the AUDIO listener (camera), emitter/one-shot position, and source
	// identity when known. -1 denotes the local player, positive values are
	// authored BMS ids, and zero keeps the no-entity path. Godot world
	// (x, up, z) -> mission fixed (x, -z, up) 16.16.
	if (!world_) return distance_q16;
	const int32_t lp[3] = {opennova::world::to_fixed(listener_pos.x),
	                       opennova::world::to_fixed(-listener_pos.z),
	                       opennova::world::to_fixed(listener_pos.y)};
	const int32_t sp[3] = {opennova::world::to_fixed(source_pos.x),
	                       opennova::world::to_fixed(-source_pos.z),
	                       opennova::world::to_fixed(source_pos.y)};
	opennova::world::EntityHandle source;
	if (source_bms_id < 0) {
		source = world_->cached.local_player;
	} else if (source_bms_id > 0) {
		world_->registry.for_each([&](const opennova::world::Entity &e) {
			if (!source.valid() && e.bms_id == source_bms_id) source = e.handle;
		});
	}
	// Static/env emitters do not ride the moving-entity collision resolver.
	// Refresh their blink/indoors state at the audio query boundary so the
	// both-indoors terrain bypass sees the source state retail registered.
	if (source.valid() && source != world_->cached.local_player) {
		if (opennova::world::Entity *source_entity = world_->registry.get(source))
			collision_world_.refresh_blink(*world_, *source_entity);
	}
	return collision_world_.sound_occlusion_inflate(*world_, world_->cached.local_player,
	                                                source, lp, sp,
	                                                static_cast<int32_t>(distance_q16));
}

PackedInt32Array NovaSimulation::compute_iris_samples(const Vector3 &p_cam_pos,
                                                      const Vector3 &p_cam_forward,
                                                      const Vector3 &p_light_dir) {
	PackedInt32Array out;
	if (!world_) return out;

	// Godot world (x, up, z) -> mission fixed (x, -z, up) 16.16.
	const int32_t cam[3] = {opennova::world::to_fixed(p_cam_pos.x),
	                        opennova::world::to_fixed(-p_cam_pos.z),
	                        opennova::world::to_fixed(p_cam_pos.y)};
	// end = camera + forward * 8.0 [orig: the (0x80000, 0, 0) forward vector
	// rotated through the camera matrix @ 0x5c7a56..0x5c7a6f].
	int32_t end[3] = {cam[0] + opennova::world::to_fixed(p_cam_forward.x * 8.0f),
	                  cam[1] + opennova::world::to_fixed(-p_cam_forward.z * 8.0f),
	                  cam[2] + opennova::world::to_fixed(p_cam_forward.y * 8.0f)};

	// Terrain clip of the camera ray [orig: raycast_entity_collision @ 0x413760
	// -> Terrain_RaycastHeightmapHiRes_0 @ 0x60e710, end clipped in place; the
	// entity nearest-hit clip is a tracked D-RLIT-2 residual].
	if (terrain_field_.valid()) {
		int32_t hit[3];
		if (opennova::world::terrain_clip_segment(terrain_field_, cam, end, hit)) {
			end[0] = hit[0];
			end[1] = hit[1];
			end[2] = hit[2];
		}
	}

	// Sun-ray direction in mission fixed: light_dir * 200 u
	// [orig: end = sample + 200 * light_dir @ 0x5c776c..0x5c7780].
	const int32_t sun[3] = {opennova::world::to_fixed(p_light_dir.x * 200.0f),
	                        opennova::world::to_fixed(-p_light_dir.z * 200.0f),
	                        opennova::world::to_fixed(p_light_dir.y * 200.0f)};
	// The three ray clip radii [orig: the -0x2000/-0x5000/-0x8000 pushes
	// @ 0x5c7767/0x5c7792/0x5c77ac].
	static const int32_t kSunRayRadii[3] = {-0x2000, -0x5000, -0x8000};

	// Samples at end, end + (cam-end)/3, end + 2(cam-end)/3 [orig: the thirds
	// march @ 0x5c7ad8..0x5c7b30].
	const int32_t step[3] = {(cam[0] - end[0]) / 3, (cam[1] - end[1]) / 3,
	                         (cam[2] - end[2]) / 3};
	for (int s = 0; s < 3; ++s) {
		const int32_t p[3] = {end[0] + step[0] * s, end[1] + step[1] * s, end[2] + step[2] * s};
		opennova::world::BlinkAccum blink;
		collision_world_.query_blink_boxes_at_point(*world_, p, blink);
		if (blink.hits[0] != 0) {
			// Indoor sample: the hit's pool-2 entity carries interior data or
			// the curve runs on all-zero inputs (gain 255)
			// [orig: Pool_GetEntryUnchecked(2, hit >> 20) @ 0x5c7646; the
			//  pool_entry[12] == 0 skip @ 0x5c7652].
			const opennova::world::EntityHandle h = opennova::world::EntityHandle::make(
					2, static_cast<int32_t>(blink.hits[0] >> 20));
			out.append(occlusion_world_.has_instance(h)
							? NovaWeatherCore::kIrisSampleIndoor
							: NovaWeatherCore::kIrisSampleIndoorNoData);
			continue;
		}
		// Outdoor sample: level = 8 minus one per blocked sun ray
		// [orig: @ 0x5c7784..0x5c77d7; the player-sector entity-count ray gate
		//  is a tracked D-RLIT-2 residual — with no statics the rays cannot hit].
		int32_t level = 8;
		const int32_t ray_end[3] = {p[0] + sun[0], p[1] + sun[1], p[2] + sun[2]};
		for (int r = 0; r < 3; ++r) {
			if (collision_world_.segment_hits_static(*world_, p, ray_end, kSunRayRadii[r]))
				--level;
		}
		out.append(level);
	}
	return out;
}

Dictionary NovaSimulation::get_collision_debug() const {
	Dictionary out;
	Array instances;
	Dictionary player;
	player["valid"] = false;
	out["instances"] = instances;
	out["player"] = player;
	if (!world_) return out;

	// Anchor the sweep on the local player when one is spawned (150u box, the
	// resolver's own neighborhood scale); a direct test/tooling instance with
	// no player sweeps the whole table up to the instance cap.
	int32_t anchor[3] = {0, 0, 0};
	int32_t range = -1;
	const opennova::world::Entity *lp =
	    world_->cached.local_player.valid() ? world_->registry.get(world_->cached.local_player)
	                                        : nullptr;
	if (lp != nullptr) {
		anchor[0] = opennova::world::to_fixed(lp->position.x);
		anchor[1] = opennova::world::to_fixed(lp->position.y);
		anchor[2] = opennova::world::to_fixed(lp->position.z);
		range = 150 << 16;
	}
	const std::vector<opennova::world::CollisionWorld::DebugInstance> insts =
	    collision_world_.debug_instances(*world_, anchor, range, 128);
	for (const opennova::world::CollisionWorld::DebugInstance &inst : insts) {
		Dictionary d;
		d["entity_handle"] = static_cast<int>(inst.handle.packed);
		d["pos"] = godot_from_fixed3(inst.pos);
		d["heading"] = static_cast<float>(
		    opennova::world::mission_yaw_deg_from_bam_heading(inst.heading_bam));
		Array vols;
		for (const opennova::world::CollisionWorld::DebugVolume &v : inst.volumes) {
			Dictionary vd;
			vd["type"] = v.type;
			vd["min_x"] = static_cast<float>(v.min[0] / kFixed16);
			vd["max_x"] = static_cast<float>(v.max[0] / kFixed16);
			vd["min_y"] = static_cast<float>(v.min[1] / kFixed16);
			vd["max_y"] = static_cast<float>(v.max[1] / kFixed16);
			vd["min_z"] = static_cast<float>(v.min[2] / kFixed16);
			vd["max_z"] = static_cast<float>(v.max[2] / kFixed16);
			PackedVector3Array corners;
			corners.resize(8);
			Vector3 *cw = corners.ptrw();
			for (int c = 0; c < 8; ++c) cw[c] = godot_from_fixed3(v.corners[c]);
			vd["corners"] = corners;
			vols.push_back(vd);
		}
		d["volumes"] = vols;
		instances.push_back(d);
	}

	// The local player's last full resolve: the capsule test points the resolver
	// queried and the returned foot clearance (CollisionWorld::LocalResolveDebug).
	const opennova::world::CollisionWorld::LocalResolveDebug &lrd =
	    collision_world_.local_resolve_debug;
	if (lrd.valid) {
		player["valid"] = true;
		player["position"] = godot_from_fixed3(lrd.pos);
		PackedVector3Array pts;
		pts.resize(3);
		Vector3 *pw = pts.ptrw();
		PackedFloat32Array radii;
		radii.resize(3);
		float *rw = radii.ptrw();
		for (int i = 0; i < 3; ++i) {
			pw[i] = godot_from_fixed3(lrd.points[i]);
			rw[i] = static_cast<float>(lrd.radii[i] / kFixed16);
		}
		player["points"] = pts;
		player["radii"] = radii;
		player["capsule_bottom"] = static_cast<float>(lrd.capsule_bottom / kFixed16);
		player["capsule_top"] = static_cast<float>(lrd.capsule_top / kFixed16);
		player["foot_clearance"] = static_cast<float>(lrd.foot_clearance / kFixed16);
	}
	return out;
}

Dictionary NovaSimulation::get_hitbox_debug() {
	constexpr int32_t kEntityCap = 96;
	Dictionary out;
	Array entities;
	Array organics;
	out["entities"] = entities;
	out["organics"] = organics;
	if (!world_) return out;

	// Anchor on the local player like the volume view. All hitbox payloads use
	// the same 80-unit debug budget; a preview with no player sweeps to the caps.
	int32_t anchor[3] = {0, 0, 0};
	int32_t debug_range = -1;
	const opennova::world::EntityHandle local_player =
	    world_->cached.local_player;
	const opennova::world::Entity *lp =
	    local_player.valid() ? world_->registry.get(local_player) : nullptr;
	if (lp != nullptr) {
		anchor[0] = opennova::world::to_fixed(lp->position.x);
		anchor[1] = opennova::world::to_fixed(lp->position.y);
		anchor[2] = opennova::world::to_fixed(lp->position.z);
		debug_range = 80 << 16;
	}
	const std::vector<opennova::world::CollisionWorld::DebugHitboxEntity> ents =
	    collision_world_.debug_hitboxes(*world_, anchor, debug_range, kEntityCap, 24000);
	for (const opennova::world::CollisionWorld::DebugHitboxEntity &ent : ents) {
		Dictionary d;
		d["entity_handle"] = static_cast<int>(ent.handle.packed);
		d["pos"] = godot_from_fixed3(ent.pos);
		d["bound_radius"] = static_cast<float>(ent.bound_radius / kFixed16);
		d["husk"] = ent.husk;
		d["has_faces"] = ent.has_faces;
		d["face_total"] = ent.face_total;
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
		d["tris"] = tris;
		d["materials"] = materials;
		d["flags"] = flags;
		entities.push_back(d);
	}

	// Posed pool-0 COBJ spheres from the exact person narrow phase. They share
	// the nearby 80-unit/96-actor debug budget. Preserve F3's late-spawn demand
	// bridge even though the local avatar is presentation-hidden; one spare query
	// slot then prevents its authored rows from consuming the target budget.
	// Entities whose graphic cannot supply usable authored sections are appended
	// below with the bounded compatibility fallback used by RoundSim.
	if (local_player.valid()) ensure_collision_instance(*world_, local_player);
	std::unordered_map<uint16_t, bool> posed_handles;
	const std::vector<opennova::world::CollisionWorld::DebugPersonSection> people =
	    collision_world_.debug_person_sections(
	        *world_, anchor, debug_range, kEntityCap + 1);
	for (const opennova::world::CollisionWorld::DebugPersonSection &person : people) {
		if (person.handle == local_player) continue;
		const bool new_handle =
		    posed_handles.find(person.handle.packed) == posed_handles.end();
		if (new_handle && posed_handles.size() >= static_cast<size_t>(kEntityCap)) break;
		Dictionary d;
		d["entity_handle"] = static_cast<int>(person.handle.packed);
		d["section"] = person.section;
		d["pos"] = godot_from_fixed3(person.center);
		d["radius"] = static_cast<float>(person.radius / kFixed16);
		d["authored_radius"] =
		    static_cast<float>(person.authored_radius / kFixed16);
		d["masked"] = person.masked;
		d["fallback"] = false;
		organics.push_back(d);
		posed_handles[person.handle.packed] = true;
	}
	const size_t pool0 = world_->registry.pool_capacity(0);
	int fallback_entity_count = static_cast<int>(posed_handles.size());
	for (size_t s = 0; s < pool0; ++s) {
		if (fallback_entity_count >= kEntityCap) break;
		const opennova::world::Entity *e =
		    world_->registry.get(opennova::world::EntityHandle{static_cast<uint16_t>(s)});
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
		Dictionary d;
		d["entity_handle"] = static_cast<int>(s);
		d["section"] = 1;
		d["pos"] = Vector3(e->position.x,
		                   e->position.z + opennova::world::kOrganicStandInCenterZ,
		                   -e->position.y);
		d["radius"] = opennova::world::kOrganicStandInRadius;
		d["authored_radius"] = opennova::world::kOrganicStandInRadius;
		d["masked"] = false;
		d["fallback"] = true;
		organics.push_back(d);
		++fallback_entity_count;
	}
	return out;
}

int NovaSimulation::debug_spawn_round(const Vector3 &p_from_godot, const Vector3 &p_dir_godot,
                                      const String &p_ammo_name) {
	if (!world_) return -1;
	const int ammo_index = world_->ammo.index_of(p_ammo_name.utf8().get_data());
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
	params.owner = world_->cached.local_player;
	params.shooter_handle = params.owner.valid() ? params.owner.packed : 0xFFFF;
	params.ammo_index = ammo_index;
	params.origin = opennova::world::Vec3{p_from_godot.x, -p_from_godot.z, p_from_godot.y};
	params.dir_yaw_bam =
	    static_cast<int32_t>(std::llround(std::atan2(dy, dx) * kBamPerRad));
	params.dir_pitch_bam = static_cast<int32_t>(std::llround(std::asin(sz) * kBamPerRad));
	return world_->round_sim.spawn(*world_, params);
}

Dictionary NovaSimulation::debug_pick_entity(const Vector3 &p_from_godot,
                                             const Vector3 &p_dir_godot,
                                             float p_max_range_units) {
	Dictionary out;
	// Typed defaults on every key so the shape is stable for every outcome
	// (the get_entity_debug convention).
	out["hit"] = false;
	out["blocked"] = String();
	out["hit_class"] = String();
	out["entity_handle"] = -1;
	out["pool"] = -1;
	out["kind"] = -1;
	out["index"] = -1;
	out["bms_id"] = 0;
	out["net_id"] = 0;
	out["item_id"] = 0;
	out["name"] = String();
	out["position_godot"] = Vector3();
	out["bound_radius"] = 0.0f;
	out["hit_position_godot"] = Vector3();
	out["hit_normal_godot"] = Vector3();
	out["distance_units"] = 0.0f;
	out["section"] = -1;
	out["face"] = -1;
	out["bone"] = -1;
	out["hit_zone"] = -1;
	out["surface_type"] = -1;
	out["material_flags"] = 0;
	out["tick"] = 0;
	if (!world_) return out;
	out["tick"] = static_cast<int64_t>(world_->logic_tick);

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
	trace.owner = world_->cached.local_player;
	trace.radius_q16 = 0;
	trace.ammo_flags = 0;
	const opennova::world::ProjectileHit hit =
	    collision_world_.trace_projectile(*world_, trace);
	if (!hit.hit()) return out;

	const int32_t hp[3] = {hit.position_q16.x, hit.position_q16.y, hit.position_q16.z};
	const int32_t hn[3] = {hit.normal_q16.x, hit.normal_q16.y, hit.normal_q16.z};
	out["hit_position_godot"] = godot_from_fixed3(hp);
	out["hit_normal_godot"] = godot_from_fixed3(hn);
	out["distance_units"] =
	    static_cast<float>(range * (static_cast<double>(hit.t_q16) / 65536.0));
	out["section"] = hit.section_index;
	out["face"] = hit.face_index;
	out["bone"] = hit.bone_index;
	out["hit_zone"] = hit.hit_zone;
	out["surface_type"] = hit.surface_type;
	out["material_flags"] = static_cast<int64_t>(hit.material_flags);

	switch (hit.hit_class) {
		case opennova::world::ProjectileHitClass::Terrain:
			out["blocked"] = "terrain";
			return out;
		case opennova::world::ProjectileHitClass::Water:
			out["blocked"] = "water";
			return out;
		default:
			break;
	}
	const opennova::world::Entity *ent = world_->registry.get(hit.geometry_entity);
	if (ent == nullptr) {
		// A decoded wire proxy or an already-freed slot: the geometry hit but
		// carries no pickable identity (joined visual-only clients).
		out["blocked"] = "proxy";
		return out;
	}
	out["hit"] = true;
	switch (hit.hit_class) {
		case opennova::world::ProjectileHitClass::StaticEntity:
			out["hit_class"] = "static";
			break;
		case opennova::world::ProjectileHitClass::DynamicEntity:
			out["hit_class"] = "dynamic";
			break;
		default:
			out["hit_class"] = "person";
			break;
	}
	out["entity_handle"] = static_cast<int>(hit.geometry_entity.packed);
	out["pool"] = hit.geometry_entity.pool();
	out["kind"] = static_cast<int>(ent->spawn_origin >> 24);
	out["index"] = static_cast<int>(ent->spawn_origin & 0xFFFFFF);
	out["bms_id"] = ent->bms_id;
	out["net_id"] = static_cast<int>(ent->net_id);
	out["item_id"] = ent->item_id;
	out["name"] = String(ent->name.c_str());
	out["position_godot"] = godot_from_mission_vec3(ent->position);
	out["bound_radius"] = ent->bound_radius;
	return out;
}

Dictionary NovaSimulation::get_round_debug() const {
	Dictionary out;
	Array events;
	out["events"] = events;
	if (!world_) return out;
	const opennova::world::RoundSim &rs = world_->round_sim;
	static const char *const kKindNames[] = {"organic", "item face", "item sphere",
	                                         "terrain",  "expired",   "face miss"};
	// Oldest -> newest so the view can draw newest-last (brightest).
	const int count = rs.debug_trail_count;
	int idx = (rs.debug_trail_next - count + opennova::world::RoundSim::kDebugTrailCap *
	          2) % opennova::world::RoundSim::kDebugTrailCap;
	for (int i = 0; i < count; ++i, idx = (idx + 1) % opennova::world::RoundSim::kDebugTrailCap) {
		const opennova::world::RoundDebugEvent &ev =
		    rs.debug_trail[static_cast<size_t>(idx)];
		Dictionary d;
		d["tick"] = static_cast<int64_t>(ev.tick);
		d["kind"] = static_cast<int>(ev.kind);
		d["kind_name"] = String(ev.kind <= 5 ? kKindNames[ev.kind] : "?");
		d["material"] = static_cast<int>(ev.material);
		d["section"] = static_cast<int>(ev.section);
		d["secondary_section"] = static_cast<int>(ev.secondary_section);
		d["fallback"] = ev.organic_fallback;
		d["face"] = static_cast<int>(ev.face);
		d["effect_tag"] = ev.effect_tag;
		d["effect_tag_name"] =
		    (ev.effect_tag >= 0 && ev.effect_tag < opennova::world::kImpactEffectTagCount)
		        ? String(opennova::world::kImpactEffectTagNames[ev.effect_tag])
		        : String("");
		d["entity_handle"] = static_cast<int>(ev.entity);
		d["shooter_handle"] = static_cast<int>(ev.shooter);
		d["ammo_index"] = ev.ammo_index;
		d["husk"] = ev.husk;
		d["t"] = ev.t;
		d["p0"] = godot_from_mission_vec3(ev.p0);
		d["p1"] = godot_from_mission_vec3(ev.p1);
		d["hit"] = godot_from_mission_vec3(ev.hit);
		// The struck entity's item name when it still resolves (wrecks keep
		// their slot until cleanup) — display sugar for the F3 list.
		String label;
		const opennova::world::Entity *te =
		    world_->registry.get(opennova::world::EntityHandle{ev.entity});
		if (te != nullptr && !te->name.empty())
			label = String(te->name.c_str());
		d["entity_name"] = label;
		events.push_back(d);
	}
	out["tick"] = static_cast<int64_t>(world_->logic_tick);
	return out;
}

Dictionary NovaSimulation::get_occlusion_debug() const {
	Dictionary out;
	Array buildings;
	Array welds;
	Dictionary counts;
	out["active"] = false;
	out["camera_indoors"] = occlusion_world_.camera_indoors();
	out["exterior_visible"] = occlusion_world_.exterior_visible();
	out["water_visible"] = occlusion_world_.water_visible();
	out["local_blink_flags"] = static_cast<int>(collision_world_.local_player_blink_flags);
	out["counts"] = counts;
	out["buildings"] = buildings;
	out["welds"] = welds;
	if (!world_) return out;

	int instances = 0, batched = 0, visible = 0;
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind != opennova::world::EntityKind::Building) return;
		if (!occlusion_world_.has_instance(e.handle)) return;
		++instances;
		const bool is_batched = occlusion_world_.building_batched(e.handle);
		const bool is_visible = occlusion_world_.building_visible(e.handle);
		if (is_batched) ++batched;
		if (is_visible) ++visible;
		if (buildings.size() >= 256) return;
		Dictionary b;
		b["bms_id"] = e.bms_id;
		const int32_t pos_fixed[3] = {opennova::world::to_fixed(e.position.x),
		                              opennova::world::to_fixed(e.position.y),
		                              opennova::world::to_fixed(e.position.z)};
		b["pos"] = godot_from_fixed3(pos_fixed);
		b["batched"] = is_batched;
		b["visible"] = is_visible;
		b["open_flagged"] = occlusion_world_.building_open_flagged(e.handle);
		b["mask"] = static_cast<int64_t>(occlusion_world_.section_mask(e.handle));
		const opennova::world::OcclusionWorld::BuildingFlags flags =
		    occlusion_world_.building_flags(e.handle);
		b["has_open"] = flags.has_open;
		b["has_windows"] = flags.has_windows;
		b["has_links"] = flags.has_links;
		// Record-type census off the (possibly weld-retyped) shared model.
		int windows = 0, portals = 0, links = 0, records = 0;
		const opennova::world::OcclusionModel *m =
		    occlusion_world_.model(occlusion_world_.instance_model_id(e.handle));
		if (m != nullptr) {
			records = static_cast<int>(m->records.size());
			for (const opennova::world::OcclusionPortalFace &rec : m->records) {
				if (rec.type == opennova::world::kOccRecWindow)
					++windows;
				else if (rec.type == opennova::world::kOccRecPortal)
					++portals;
				else if (rec.type == opennova::world::kOccRecWeldedLink)
					++links;
			}
		}
		b["records"] = records;
		b["windows"] = windows;
		b["portals"] = portals;
		b["links"] = links;
		buildings.push_back(b);
	});

	for (const opennova::world::OcclusionWorld::WeldRecord &wr : occlusion_world_.weld_records()) {
		if (welds.size() >= 64) break;
		Dictionary w;
		const opennova::world::Entity *own = world_->registry.get(wr.own_entity);
		const opennova::world::Entity *other = world_->registry.get(wr.other_entity);
		w["own_bms"] = own != nullptr ? own->bms_id : 0;
		w["own_section"] = wr.own_section;
		w["other_bms"] = other != nullptr ? other->bms_id : 0;
		w["other_section"] = wr.other_section;
		welds.push_back(w);
	}

	counts["instances"] = instances;
	counts["batched"] = batched;
	counts["visible"] = visible;
	counts["toc_culled"] = batched - visible;
	counts["slots"] = occlusion_world_.slot_count();
	counts["window_groups"] = occlusion_world_.window_frustum_group_count();
	counts["viewthru_groups"] = occlusion_world_.viewthru_group_count();
	counts["welds"] = static_cast<int>(occlusion_world_.weld_records().size());
	counts["culled_entities"] = static_cast<int>(occlusion_culled_bms_.size());
	out["active"] = instances > 0;
	return out;
}

Dictionary NovaSimulation::get_occlusion_portal_debug(const Vector3 &p_anchor,
                                                      double p_range_units) const {
	Dictionary out;
	Array buildings;
	out["buildings"] = buildings;
	if (!world_) return out;
	// Godot world (x, up, z) -> mission fixed (x, -z, up) 16.16.
	const int64_t anchor_x = opennova::world::to_fixed(p_anchor.x);
	const int64_t anchor_y = opennova::world::to_fixed(-p_anchor.z);
	const int64_t range =
	    p_range_units > 0.0 ? static_cast<int64_t>(p_range_units * kFixed16) : -1;
	world_->registry.for_each([&](const opennova::world::Entity &e) {
		if (e.kind != opennova::world::EntityKind::Building) return;
		if (buildings.size() >= 128) return;
		const opennova::world::OcclusionModel *m =
		    occlusion_world_.model(occlusion_world_.instance_model_id(e.handle));
		if (m == nullptr) return;
		const int32_t pos_fixed[3] = {opennova::world::to_fixed(e.position.x),
		                              opennova::world::to_fixed(e.position.y),
		                              opennova::world::to_fixed(e.position.z)};
		if (range >= 0 && (std::abs(pos_fixed[0] - anchor_x) > range ||
		                   std::abs(pos_fixed[1] - anchor_y) > range))
			return;
		// The same full authored building-pose path the engine's frame uses.
		const opennova::world::RenderMatrix mat =
		    opennova::world::render_matrix_from_entity_pose(e);
		Dictionary b;
		b["bms_id"] = e.bms_id;
		b["pos"] = godot_from_fixed3(pos_fixed);
		b["visible"] = occlusion_world_.building_visible(e.handle);
		Array records;
		for (const opennova::world::OcclusionPortalFace &rec : m->records) {
			Dictionary rd;
			rd["type"] = static_cast<int>(rec.type);
			rd["section_a"] = static_cast<int>(rec.section_a);
			rd["section_b"] = static_cast<int>(rec.section_b);
			float wp[3];
			mat.transform_point(rec.pos, wp);
			rd["pos"] = godot_from_render_float3(wp);
			rd["radius"] = rec.radius;
			rd["glow"] = rec.glow_scale;
			// The record's boundary outline: OFAC edge words whose low-15-bit
			// identity appears once (shared interior edges pair up and drop —
			// the same cancellation identity the occluder pass uses).
			std::vector<uint16_t> edges;
			std::vector<int32_t> hits;
			for (int32_t f = 0; f < rec.face_count; ++f) {
				const opennova::world::OcclusionFaceRec &face = m->faces[rec.face_start + f];
				for (int k = 0; k < 3; ++k) {
					const uint16_t w = face.edge[k];
					bool found = false;
					for (size_t x = 0; x < edges.size(); ++x) {
						if ((edges[x] & 0x7FFF) == (w & 0x7FFF)) {
							++hits[x];
							found = true;
							break;
						}
					}
					if (!found) {
						edges.push_back(w);
						hits.push_back(1);
					}
				}
			}
			PackedVector3Array segments;
			for (size_t x = 0; x < edges.size(); ++x) {
				if (hits[x] != 1) continue;
				const int32_t va = edges[x] & 0xFF;
				const int32_t vb = (edges[x] >> 8) & 0x7F;
				if (va >= rec.vert_count || vb >= rec.vert_count) continue;
				float aw[3], bw[3];
				mat.transform_point(m->vertices[rec.vert_start + va].p, aw);
				mat.transform_point(m->vertices[rec.vert_start + vb].p, bw);
				segments.push_back(godot_from_render_float3(aw));
				segments.push_back(godot_from_render_float3(bw));
			}
			rd["segments"] = segments;
			records.push_back(rd);
		}
		b["records"] = records;
		buildings.push_back(b);
	});
	return out;
}
