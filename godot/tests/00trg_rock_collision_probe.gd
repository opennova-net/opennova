extends SceneTree

# Deterministic retail-asset regression for the rock directly in front of the
# sole 00TRg single-player start. This bypasses rendering/UI while keeping the
# production mission promotion -> .3di collision IR -> RoundSim path.
#
# Run:
#   NOVA_RESOURCE_DIR=<loose JOX> godot --headless --path godot \
#     -s res://tests/00trg_rock_collision_probe.gd

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

const MISSION := "00TRg.bms"
const SPAWN_BMS_ID := 1197
const TARGET_BMS_ID := 650
const TARGET_ITEM_ID := 101471
const TARGET_GRAPHIC := "RckS05"
const TARGET_FACE_COUNT := 52
const MAX_VERTEX_ERROR := 0.02
const AMMO := "AMMO_M16_556MM"


func _init() -> void:
	call_deferred("_run")


func _run() -> void:
	var resource_dir := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if resource_dir.is_empty():
		_fail("set NOVA_RESOURCE_DIR to a loose or mounted retail JOX corpus")
		return

	var root := NovaResourceRoot.new()
	var mount_err := int(root.mount_runtime(resource_dir, "", false, "jo"))
	if mount_err != OK:
		# The extracted JOX corpus is a loose directory, not a retail PFF install.
		root.set_root_dir(resource_dir)
		if not root.has_file(MISSION):
			_fail("resource root failed (%d): %s" % [mount_err, root.get_last_error()])
			return

	var mission := NovaMissionData.new()
	if mission.open_from_resource_root(root, MISSION) != OK:
		_fail("cannot open %s" % MISSION)
		return
	var item_db := NovaItemDatabase.new()
	if item_db.load_from_resource_root(root, "items.def") != OK:
		_fail("cannot load items.def: %s" % item_db.get_last_error())
		return

	var spawn_record: Dictionary = {}
	var target_record: Dictionary = {}
	for raw in mission.get_all_entities():
		var entity: Dictionary = raw
		var bms_id := int(entity.get("bms_id", 0))
		if bms_id == SPAWN_BMS_ID:
			spawn_record = entity
		elif bms_id == TARGET_BMS_ID:
			target_record = entity
	if spawn_record.is_empty() or target_record.is_empty():
		_fail("fixture records missing (spawn=%s target=%s)" % [
			str(not spawn_record.is_empty()), str(not target_record.is_empty())])
		return
	var target_item := int(target_record.get("item_id", 0))
	var target_graphic := String(item_db.get_graphic(target_item))
	if target_item != TARGET_ITEM_ID or target_graphic.nocasecmp_to(TARGET_GRAPHIC) != 0:
		_fail("entity %d changed identity: item=%d graphic=%s" % [
			TARGET_BMS_ID, target_item, target_graphic])
		return
	var rotation_mode := OS.get_environment("NOVA_ROCK_ROTATION_MODE").strip_edges()
	if rotation_mode == "identity" or rotation_mode == "yaw_only":
		var original_rotation: Vector3 = target_record.get("rotation_deg", Vector3.ZERO)
		var probe_rotation := Vector3.ZERO
		if rotation_mode == "yaw_only":
			probe_rotation.y = original_rotation.y
		if not mission.set_entity_transform(
				int(target_record.get("kind", -1)), int(target_record.get("index", -1)),
				target_record.get("position", Vector3.ZERO), probe_rotation):
			_fail("cannot apply %s diagnostic rotation" % rotation_mode)
			return
		target_record = mission.get_entity(
			int(target_record.get("kind", -1)), int(target_record.get("index", -1)))
		print("[rock] diagnostic rotation mode=%s authored=%s probe=%s" % [
			rotation_mode, str(original_rotation), str(probe_rotation)])

	var sim := NovaSimulation.new()
	if not sim.load_from_mission_data(mission):
		sim.free()
		_fail("NovaSimulation rejected %s" % MISSION)
		return
	var spawn_status := int(sim.spawn_local_player_at_start())
	if spawn_status != 1:
		sim.free()
		_fail("spawn_local_player_at_start returned %d" % spawn_status)
		return
	sim.resolve_item_traits(item_db)
	var placer := MissionObjectPlacer.new(root, item_db)
	var attached := int(sim.resolve_collision_instances(item_db, placer))
	if attached <= 0:
		sim.free()
		_fail("no collision instances attached")
		return
	if sim.load_ammo_table(root, "ammo.def") != OK:
		sim.free()
		_fail("cannot load ammo.def")
		return

	var player_pos: Vector3 = sim.get_local_player_position()
	var player_yaw := float(sim.get_local_player_yaw_deg())
	var eye_height := float(OS.get_environment("NOVA_ROCK_EYE_HEIGHT").to_float())
	if eye_height <= 0.0:
		eye_height = 1.6
	var eye := player_pos + Vector3(0.0, eye_height, 0.0)
	var yaw_rad := deg_to_rad(player_yaw)
	var spawn_forward := Vector3(sin(yaw_rad), 0.0, -cos(yaw_rad)).normalized()

	var target_pos_bms: Vector3 = target_record.get("position", Vector3.ZERO)
	var target_rot: Vector3 = target_record.get("rotation_deg", Vector3.ZERO)
	var target_xform := MissionObjectPlacer.entity_transform(target_pos_bms, target_rot)
	var visual_aabb := _visual_world_aabb(placer.object_data_for(TARGET_GRAPHIC), target_xform)
	var target_debug := _target_hitbox(sim.get_hitbox_debug(), target_xform.origin)
	if target_debug.is_empty():
		sim.free()
		_fail("entity %d has no projectile hitbox entry" % TARGET_BMS_ID)
		return
	var target_handle := int(target_debug.get("entity_handle", -1))
	var target_tris: PackedVector3Array = target_debug.get("tris", PackedVector3Array())
	var hitbox_aabb := _points_aabb(target_tris)
	var vertex_parity := _collision_to_render_vertex_error(
		placer.object_data_for(TARGET_GRAPHIC), target_xform, target_tris)

	print("[rock] mission=%s spawn_bms=%d pos=%s yaw=%.3f eye=%s" % [
		MISSION, SPAWN_BMS_ID, str(player_pos), player_yaw, str(eye)])
	print("[rock] target_bms=%d item=%d graphic=%s pos=%s rot=%s handle=%d" % [
		TARGET_BMS_ID, target_item, target_graphic, str(target_xform.origin),
		str(target_rot), target_handle])
	print("[rock] target_world_debug=%s" % str(sim.get_world_entity_debug(TARGET_BMS_ID)))
	print("[rock] target_effect_state=%s" % str(
		sim.get_entity_effect_state_for_ssn(TARGET_BMS_ID)))
	print("[rock] attached=%d faces=%d/%d visual_aabb=%s hitbox_aabb=%s" % [
		attached, int((target_debug.get("tris", PackedVector3Array()) as PackedVector3Array).size() / 3),
		int(target_debug.get("face_total", 0)), str(visual_aabb), str(hitbox_aabb)])
	print("[rock] collision_to_render_vertex_error=%s" % str(vertex_parity))
	var emitted_faces := int(target_tris.size() / 3)
	var total_faces := int(target_debug.get("face_total", 0))
	if emitted_faces != TARGET_FACE_COUNT or total_faces != TARGET_FACE_COUNT:
		sim.free()
		_fail("RckS05 collision faces changed: emitted=%d total=%d expected=%d" % [
			emitted_faces, total_faces, TARGET_FACE_COUNT])
		return
	if float(vertex_parity.get("max", INF)) > MAX_VERTEX_ERROR:
		sim.free()
		_fail("RckS05 collision/render vertex drift %.6f exceeds %.3f" % [
			float(vertex_parity.get("max", INF)), MAX_VERTEX_ERROR])
		return

	var center := visual_aabb.get_center()
	var screen_right := spawn_forward.cross(Vector3.UP).normalized()
	var lateral := 0.18 * minf(visual_aabb.size.x, visual_aabb.size.z)
	var vertical := 0.18 * visual_aabb.size.y
	var rays := [
		{"name": "spawn_forward", "dir": spawn_forward, "required": false},
		{"name": "visual_center", "dir": (center - eye).normalized(), "required": false},
		{"name": "visual_left", "dir": (center - screen_right * lateral - eye).normalized(),
			"required": false},
		{"name": "visual_right", "dir": (center + screen_right * lateral - eye).normalized(),
			"required": false},
		{"name": "visual_upper", "dir": (center + Vector3.UP * vertical - eye).normalized(),
			"required": false},
		{"name": "visual_lower", "dir": (center - Vector3.UP * vertical - eye).normalized(),
			"required": false},
	]
	var silhouette_rays := _visual_silhouette_rays(
		placer.object_data_for(TARGET_GRAPHIC), target_xform, eye, spawn_forward)
	if silhouette_rays.size() != 4:
		sim.free()
		_fail("expected four render-triangle silhouette rays, got %d" % silhouette_rays.size())
		return
	for ray_name in silhouette_rays:
		rays.append({"name": ray_name, "dir": silhouette_rays[ray_name], "required": true})

	var misses: Array[String] = []
	for ray_v in rays:
		var ray: Dictionary = ray_v
		var debug_cross := _ray_hits_triangles(eye, ray["dir"], target_tris)
		var result := _fire_one(sim, eye, ray["dir"], target_handle)
		var required := bool(ray.get("required", false))
		print("[rock] ray=%-14s required=%s debug_cross=%s hit=%s target_face_misses=%d terminal=%s" % [
			String(ray["name"]), str(required), str(debug_cross), str(bool(result["hit"])),
			int(result["face_misses"]), String(result["terminal"])])
		if required and (not debug_cross or not bool(result["hit"])):
			misses.append(String(ray["name"]))

	sim.free()
	if not misses.is_empty():
		_fail("RckS05 entity %d missed render-triangle rays: %s" % [
			TARGET_BMS_ID, ", ".join(misses)])
		return
	print("[rock] PASS: all %d render-triangle rays hit entity %d's CFAC mesh" % [
		silhouette_rays.size(), TARGET_BMS_ID])
	quit(0)


func _fire_one(sim: NovaSimulation, origin: Vector3, direction: Vector3,
		target_handle: int) -> Dictionary:
	var before: Array = sim.get_round_debug().get("events", [])
	var slot := int(sim.debug_spawn_round(origin, direction, AMMO))
	if slot < 0:
		return {"hit": false, "face_misses": 0, "terminal": "spawn_failed"}
	for _tick in range(384):
		sim.step()
	var after: Array = sim.get_round_debug().get("events", [])
	var hit := false
	var face_misses := 0
	var terminal := "no_event"
	for i in range(before.size(), after.size()):
		var event: Dictionary = after[i]
		var kind := int(event.get("kind", -1))
		var event_handle := int(event.get("entity_handle", -1))
		if event_handle == target_handle and kind == 1:
			hit = true
			terminal = "item_face"
		elif event_handle == target_handle and kind == 5:
			face_misses += 1
		if kind != 5:
			terminal = String(event.get("kind_name", terminal))
	return {"hit": hit, "face_misses": face_misses, "terminal": terminal}


func _target_hitbox(debug: Dictionary, target_origin: Vector3) -> Dictionary:
	var best: Dictionary = {}
	var best_dist := INF
	for raw in debug.get("entities", []):
		var entity: Dictionary = raw
		var distance := (entity.get("pos", Vector3.ZERO) as Vector3).distance_to(target_origin)
		if distance < best_dist:
			best_dist = distance
			best = entity
	return best if best_dist < 0.05 else {}


func _visual_world_aabb(data: NovaObjectData, xform: Transform3D) -> AABB:
	var local := AABB()
	var first := true
	if data != null:
		for raw in data.build_lod_submeshes(0):
			var entry: Dictionary = raw
			var mesh: ArrayMesh = entry.get("mesh")
			if mesh == null:
				continue
			var bounds := mesh.get_aabb()
			bounds.position += entry.get("abs", Vector3.ZERO) as Vector3
			local = bounds if first else local.merge(bounds)
			first = false
	if first:
		return AABB()
	var world_points := PackedVector3Array()
	for i in range(8):
		world_points.append(xform * local.get_endpoint(i))
	return _points_aabb(world_points)


func _collision_to_render_vertex_error(data: NovaObjectData, xform: Transform3D,
		tris: PackedVector3Array) -> Dictionary:
	var render_points := PackedVector3Array()
	if data != null:
		for raw in data.build_lod_submeshes(0):
			var entry: Dictionary = raw
			var mesh: ArrayMesh = entry.get("mesh")
			if mesh == null:
				continue
			var part_offset: Vector3 = entry.get("abs", Vector3.ZERO)
			for surface in range(mesh.get_surface_count()):
				var arrays: Array = mesh.surface_get_arrays(surface)
				if arrays.size() <= Mesh.ARRAY_VERTEX:
					continue
				for point in arrays[Mesh.ARRAY_VERTEX] as PackedVector3Array:
					render_points.append(xform * (point + part_offset))
	var unique_collision := PackedVector3Array()
	for point in tris:
		var seen := false
		for prior in unique_collision:
			if prior.distance_squared_to(point) < 0.00000001:
				seen = true
				break
		if not seen:
			unique_collision.append(point)
	var total := 0.0
	var worst := 0.0
	for point in unique_collision:
		var nearest := INF
		for render_point in render_points:
			nearest = minf(nearest, point.distance_to(render_point))
		total += nearest
		worst = maxf(worst, nearest)
	return {
		"collision_vertices": unique_collision.size(),
		"render_vertices": render_points.size(),
		"mean": total / float(unique_collision.size()) if not unique_collision.is_empty() else INF,
		"max": worst,
	}


# Rays through real LOD-0 render-triangle centroids at the four angular
# silhouette extremes. Each chosen direction is guaranteed to cross authored
# visible geometry; a CFAC miss there is a concrete render-vs-bullet hole, not
# an AABB approximation artifact.
func _visual_silhouette_rays(data: NovaObjectData, xform: Transform3D,
		eye: Vector3, forward: Vector3) -> Dictionary:
	var right := forward.cross(Vector3.UP).normalized()
	var best := {}
	var score := {
		"mesh_left": INF,
		"mesh_right": -INF,
		"mesh_lower": INF,
		"mesh_upper": -INF,
	}
	if data == null:
		return best
	for raw in data.build_lod_submeshes(0):
		var entry: Dictionary = raw
		var mesh: ArrayMesh = entry.get("mesh")
		if mesh == null:
			continue
		var part_offset: Vector3 = entry.get("abs", Vector3.ZERO)
		for surface in range(mesh.get_surface_count()):
			var arrays: Array = mesh.surface_get_arrays(surface)
			if arrays.size() <= Mesh.ARRAY_VERTEX:
				continue
			var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
			var indices: PackedInt32Array = arrays[Mesh.ARRAY_INDEX] \
					if arrays.size() > Mesh.ARRAY_INDEX else PackedInt32Array()
			var count := indices.size() if not indices.is_empty() else vertices.size()
			for base in range(0, count - 2, 3):
				var i0 := indices[base] if not indices.is_empty() else base
				var i1 := indices[base + 1] if not indices.is_empty() else base + 1
				var i2 := indices[base + 2] if not indices.is_empty() else base + 2
				if i0 < 0 or i1 < 0 or i2 < 0 \
						or i0 >= vertices.size() or i1 >= vertices.size() or i2 >= vertices.size():
					continue
				var centroid := xform * (
						(vertices[i0] + vertices[i1] + vertices[i2]) / 3.0 + part_offset)
				var direction := (centroid - eye).normalized()
				var front := direction.dot(forward)
				if front <= 0.0:
					continue
				var horizontal := atan2(direction.dot(right), front)
				var vertical := asin(clampf(direction.y, -1.0, 1.0))
				if horizontal < float(score["mesh_left"]):
					score["mesh_left"] = horizontal
					best["mesh_left"] = direction
				if horizontal > float(score["mesh_right"]):
					score["mesh_right"] = horizontal
					best["mesh_right"] = direction
				if vertical < float(score["mesh_lower"]):
					score["mesh_lower"] = vertical
					best["mesh_lower"] = direction
				if vertical > float(score["mesh_upper"]):
					score["mesh_upper"] = vertical
					best["mesh_upper"] = direction
	for name in best:
		print("[rock] %s angle_hv=(%.3f, %.3f) dir=%s" % [
			name, rad_to_deg(float(score[name])) if name.begins_with("mesh_l") \
					or name.begins_with("mesh_r") else 0.0,
			rad_to_deg(asin(clampf((best[name] as Vector3).y, -1.0, 1.0))),
			str(best[name])])
	return best


func _points_aabb(points: PackedVector3Array) -> AABB:
	if points.is_empty():
		return AABB()
	var out := AABB(points[0], Vector3.ZERO)
	for point in points:
		out = out.expand(point)
	return out


func _ray_hits_triangles(origin: Vector3, direction: Vector3,
		tris: PackedVector3Array) -> bool:
	for base in range(0, tris.size() - 2, 3):
		var edge1 := tris[base + 1] - tris[base]
		var edge2 := tris[base + 2] - tris[base]
		var h := direction.cross(edge2)
		var det := edge1.dot(h)
		if absf(det) < 0.000001:
			continue
		var inv_det := 1.0 / det
		var from_vertex := origin - tris[base]
		var u := from_vertex.dot(h) * inv_det
		if u < 0.0 or u > 1.0:
			continue
		var q := from_vertex.cross(edge1)
		var v := direction.dot(q) * inv_det
		if v < 0.0 or u + v > 1.0:
			continue
		if edge2.dot(q) * inv_det > 0.0001:
			return true
	return false


func _fail(message: String) -> void:
	push_error("[rock] FAIL: " + message)
	quit(1)
