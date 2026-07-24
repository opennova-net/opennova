extends SceneTree

const ItemSeatSpecs := preload("res://engine/world/item_seat_specs.gd")


func _init() -> void:
	var resource_dir := OS.get_environment("JO_RESOURCE_DIR")
	if resource_dir.is_empty():
		resource_dir = "C:/GAMES/JOTAC/Game/JO"
	var expansion := OS.get_environment("JO_EXPANSION")
	if expansion.is_empty():
		expansion = "revx02"
	var mission_name := OS.get_environment("JO_MISSION")
	if mission_name.is_empty():
		mission_name = "00TRa.bms"

	var root := NovaResourceRoot.new()
	var root_err := int(root.mount_runtime(resource_dir, expansion, true))
	if root_err != OK:
		push_error("mount_runtime failed: %s" % root.get_last_error())
		quit(1)
		return

	var mission := NovaMissionData.new()
	if mission.open_from_resource_root(root, mission_name) != OK:
		push_error("mission open failed: %s" % mission_name)
		quit(1)
		return

	var item_db := NovaItemDatabase.new()
	if item_db.load_from_resource_root(root, "items.def") != OK:
		push_error("items.def load failed: %s" % item_db.get_last_error())
		quit(1)
		return

	var entities: Array = mission.get_all_entities()
	var by_ssn := {}
	for raw in entities:
		var entity: Dictionary = raw
		var ssn := int(entity.get("bms_id", entity.get("id", 0)))
		if ssn != 0:
			by_ssn[ssn] = entity

	var rows: Array = []
	var seat_cache := {}
	for raw in entities:
		var organic: Dictionary = raw
		if int(organic.get("kind", -1)) != NovaMissionData.KIND_ORGANIC:
			continue
		var command_id := int(organic.get("waypoint_id", 0))
		if command_id < 123 or command_id > 125:
			continue
		var target_ssn := int(organic.get("wp_number", 0))
		var target: Dictionary = by_ssn.get(target_ssn, {})
		var target_summary := {"bms_id": target_ssn, "found": not target.is_empty()}
		var seats: Array = []
		var spec := {}
		if not target.is_empty():
			var type_id := int(target.get("type_id", 0))
			if not seat_cache.has(type_id):
				seat_cache[type_id] = ItemSeatSpecs.seat_specs_for_item(
						root, item_db, int(target.get("item_id", 0)), type_id, true)
			spec = seat_cache[type_id]
			seats = spec.get("seats", [])
			target_summary.merge(_entity_summary(target, item_db), true)
			target_summary["seat_count"] = seats.size()
			target_summary["seat_error"] = String(spec.get("error", ""))
			target_summary["model"] = String(spec.get("model", ""))
			target_summary["model_seat_points"] = _model_seat_points(root, String(spec.get("model", "")))
		var prediction := ItemSeatSpecs.predict_best_seat(seats, command_id)
		rows.append({
			"organic": _entity_summary(organic, item_db),
			"command": ItemSeatSpecs.command_rule(command_id),
			"target": target_summary,
			"prediction": {
				"seat_index": prediction.get("seat_index", -1),
				"seat": prediction.get("seat", {}),
			},
			"seat_candidates": prediction.get("candidates", []),
		})

	var sim := NovaSimulation.new()
	sim.set_item_seat_specs(ItemSeatSpecs.build_item_seat_specs(mission, root, item_db, true))
	if sim.load_from_mission_data(mission):
		for i in range(int(sim.get_entity_count())):
			var card: Dictionary = sim.get_entity_debug(i)
			var command_id := int(card.get("waypoint_id", 0))
			if command_id >= 123 and command_id <= 125:
				print("MOUNT_LIVE %s" % JSON.stringify(_sanitize(card)))
				print("MOUNT_BODY_ANCHOR %s" % JSON.stringify(_sanitize(_body_anchor_diag(root, item_db, card, by_ssn))))
				print("MOUNT_VISUAL_ANCHOR %s" % JSON.stringify(_sanitize(_body_visual_anchor_diag(root, item_db, card, by_ssn))))
	else:
		print("MOUNT_LIVE_ERROR load_from_mission_data failed")
	sim.free()

	print("MOUNT_DIAG_SUMMARY %s" % JSON.stringify({
		"mission": mission_name,
		"resource_dir": resource_dir,
		"expansion": expansion,
		"mount_commands": rows.size(),
	}))
	for row in rows:
		print("MOUNT_DIAG %s" % JSON.stringify(_sanitize(row)))
	quit(0)


func _entity_summary(entity: Dictionary, item_db: NovaItemDatabase) -> Dictionary:
	var item_id := int(entity.get("item_id", 0))
	var out := {
		"kind": int(entity.get("kind", -1)),
		"index": int(entity.get("index", -1)),
		"bms_id": int(entity.get("bms_id", entity.get("id", 0))),
		"item_id": item_id,
		"type_id": int(entity.get("type_id", 0)),
		"display_name": String(item_db.get_display_name(item_id)) if item_db.has_item(item_id) else "",
		"graphic": String(item_db.get_graphic(item_id)) if item_db.has_item(item_id) else "",
		"position": entity.get("position", Vector3.ZERO),
		"rotation_deg": entity.get("rotation_deg", Vector3.ZERO),
		"waypoint_id": int(entity.get("waypoint_id", 0)),
		"wp_number": int(entity.get("wp_number", 0)),
	}
	return out


func _model_seat_points(root: NovaResourceRoot, model_name: String) -> Array:
	var out: Array = []
	if model_name.is_empty():
		return out
	var data := NovaObjectData.new()
	if data.open_from_resource_root(root, model_name) != OK:
		return out
	var part_xforms: Dictionary = data.evaluate_panm(0, 0, {})
	for i in range(data.get_user_point_count()):
		var up: Dictionary = data.get_user_point_info(i)
		var source_name := String(up.get("name", ""))
		var seat_type := ItemSeatSpecs.seat_type_for_user_point(source_name)
		if seat_type == ItemSeatSpecs.SEAT_NONE:
			continue
		var raw_position: Vector3 = up.get("position", Vector3.ZERO)
		var raw_bms := ItemSeatSpecs.seat_local_from_user_point_position(raw_position)
		var subobject := int(up.get("subobject", -1))
		var transformed_position := raw_position
		var xform_found := false
		if part_xforms.has(subobject):
			var xf: Transform3D = part_xforms[subobject]
			transformed_position = xf * raw_position
			xform_found = true
		var transformed_bms := ItemSeatSpecs.seat_local_from_user_point_position(transformed_position)
		out.append({
			"userpoint_index": i,
			"source_name": source_name,
			"seat_type": seat_type,
			"pose_index": ItemSeatSpecs.seat_pose_index_for_user_point(source_name),
			"subobject": subobject,
			"point_type": int(up.get("point_type", 0)),
			"raw_godot": raw_position,
			"raw_bms": raw_bms,
			"part_transform_found": xform_found,
			"part_transform_origin": (part_xforms[subobject] as Transform3D).origin if xform_found else Vector3.ZERO,
			"transformed_godot": transformed_position,
			"transformed_bms": transformed_bms,
			"delta_bms": transformed_bms - raw_bms,
		})
	return out


func _body_anchor_diag(root: NovaResourceRoot, item_db: NovaItemDatabase, card: Dictionary, by_ssn: Dictionary) -> Dictionary:
	var bms_id := int(card.get("bms_id", card.get("net_id", 0)))
	var mission_entity: Dictionary = by_ssn.get(bms_id, {})
	var item_id := int(mission_entity.get("item_id", card.get("item_id", 0)))
	var anim_key := String(card.get("anim_key", ""))
	var anim_phase_ticks := int(card.get("anim_phase_ticks", 0))
	var out := {
		"bms_id": bms_id,
		"item_id": item_id,
		"live_item_id": int(card.get("item_id", 0)),
		"anim_key": anim_key,
		"anim_phase_ticks": anim_phase_ticks,
	}
	if item_db == null or not item_db.has_item(item_id):
		out["error"] = "missing_item"
		return out
	var adm_name := String(item_db.get_anim_def(item_id))
	if adm_name.is_empty():
		out["error"] = "empty_anim_def"
		return out
	if not adm_name.to_lower().ends_with(".adm"):
		adm_name += ".adm"
	out["adm_name"] = adm_name
	var skeletal := NovaSkeletalAnim.new()
	if not skeletal.load_from_resource_root(root, adm_name):
		out["error"] = skeletal.get_last_error()
		return out
	if anim_key.is_empty() or not skeletal.has_clip(anim_key):
		out["error"] = "missing_clip"
		out["clip_keys"] = skeletal.get_clip_keys()
		return out
	var fps := float(skeletal.get_clip_fps(anim_key))
	var seconds := 0.0
	if fps > 0.0:
		seconds = float(maxi(anim_phase_ticks, 0)) / (2.0 * fps)
	out["clip_fps"] = fps
	out["playhead_seconds"] = seconds
	var bones: Array = skeletal.get_skeleton_bones()
	var pose: Array = skeletal.eval_pose(anim_key, seconds)
	var world := _accumulate_bone_worlds(bones, pose)
	out["bone_count"] = bones.size()
	out["interesting_bones"] = _interesting_bones(bones, world)
	out["sit_clip_summary"] = _sit_clip_summary(skeletal, bones)
	out["all_bones"] = _bone_name_list(bones)
	return out


func _body_visual_anchor_diag(root: NovaResourceRoot, item_db: NovaItemDatabase, card: Dictionary, by_ssn: Dictionary) -> Dictionary:
	var bms_id := int(card.get("bms_id", card.get("net_id", 0)))
	var mission_entity: Dictionary = by_ssn.get(bms_id, {})
	var item_id := int(mission_entity.get("item_id", card.get("item_id", 0)))
	var anim_key := String(card.get("anim_key", ""))
	var anim_phase_ticks := int(card.get("anim_phase_ticks", 0))
	var out := {
		"bms_id": bms_id,
		"item_id": item_id,
		"anim_key": anim_key,
		"anim_phase_ticks": anim_phase_ticks,
	}
	if item_db == null or not item_db.has_item(item_id):
		out["error"] = "missing_item"
		return out
	var adm_name := String(item_db.get_anim_def(item_id))
	var graphic := String(item_db.get_graphic(item_id))
	out["graphic"] = graphic
	out["adm_name"] = adm_name
	if adm_name.is_empty() or graphic.is_empty():
		out["error"] = "missing_anim_or_graphic"
		return out
	if not adm_name.to_lower().ends_with(".adm"):
		adm_name += ".adm"
	var model_name := ItemSeatSpecs.model_name_for_graphic(graphic)
	var data := NovaObjectData.new()
	if data.open_from_resource_root(root, model_name) != OK:
		out["error"] = "model_load_failed"
		out["model"] = model_name
		return out
	var skeletal := NovaSkeletalAnim.new()
	if not skeletal.load_from_resource_root(root, adm_name):
		out["error"] = skeletal.get_last_error()
		return out
	if anim_key.is_empty() or not skeletal.has_clip(anim_key):
		out["error"] = "missing_clip"
		return out
	var fps := float(skeletal.get_clip_fps(anim_key))
	var seconds := 0.0
	if fps > 0.0:
		seconds = float(maxi(anim_phase_ticks, 0)) / (2.0 * fps)
	var bones: Array = skeletal.get_skeleton_bones()
	var rest_world := _accumulate_rest_worlds(bones)
	var pose_world := _accumulate_bone_worlds(bones, skeletal.eval_pose(anim_key, seconds))
	var body := _skinned_mesh_stats(data, bones.size(), rest_world, pose_world)
	var hips := _bone_world_position(bones, pose_world, "BN01 Hips")
	var spine := _bone_world_position(bones, pose_world, "BN02 Lower Spine")
	var head := _bone_world_position(bones, pose_world, "BN15 Head")
	out["model"] = model_name
	out["clip_fps"] = fps
	out["playhead_seconds"] = seconds
	out["hips"] = hips
	out["lower_spine"] = spine
	out["head"] = head
	out["mesh"] = body
	if body.has("centroid"):
		out["centroid_from_hips"] = (body["centroid"] as Vector3) - hips
	if body.has("aabb_center"):
		out["aabb_center_from_hips"] = (body["aabb_center"] as Vector3) - hips
	if OS.get_environment("JO_SIT_VISUAL_SUMMARY") == "1":
		out["sit_visual_summary"] = _sit_visual_summary(skeletal, data, bones, rest_world)
	return out


func _accumulate_rest_worlds(bones: Array) -> Array:
	var world: Array = []
	world.resize(bones.size())
	for i in range(bones.size()):
		var bd: Dictionary = bones[i]
		var local: Transform3D = bd.get("rest", Transform3D.IDENTITY)
		var parent := int(bd.get("parent_index", -1))
		if parent >= 0 and parent < i and world[parent] is Transform3D:
			world[i] = (world[parent] as Transform3D) * local
		else:
			world[i] = local
	return world


func _skinned_mesh_stats(data: NovaObjectData, bone_count: int, rest_world: Array, pose_world: Array) -> Dictionary:
	var out := {
		"surface_count": 0,
		"vertex_count": 0,
	}
	var submeshes: Array = data.build_lod_submeshes(0, true, bone_count)
	var min_v := Vector3(INF, INF, INF)
	var max_v := Vector3(-INF, -INF, -INF)
	var sum := Vector3.ZERO
	for entry_v in submeshes:
		var entry: Dictionary = entry_v
		var mesh: ArrayMesh = entry.get("mesh")
		if mesh == null or mesh.get_surface_count() <= 0:
			continue
		out["surface_count"] = int(out["surface_count"]) + 1
		var arrays: Array = mesh.surface_get_arrays(0)
		if arrays.size() <= Mesh.ARRAY_VERTEX:
			continue
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var bone_indices: PackedInt32Array = arrays[Mesh.ARRAY_BONES] if arrays.size() > Mesh.ARRAY_BONES else PackedInt32Array()
		var weights: PackedFloat32Array = arrays[Mesh.ARRAY_WEIGHTS] if arrays.size() > Mesh.ARRAY_WEIGHTS else PackedFloat32Array()
		var has_skin := bone_indices.size() == vertices.size() * 4 and weights.size() == vertices.size() * 4
		for i in range(vertices.size()):
			var v := vertices[i]
			var p := v
			if has_skin:
				p = Vector3.ZERO
				for k in range(4):
					var bi := int(bone_indices[i * 4 + k])
					var w := float(weights[i * 4 + k])
					if w <= 0.0:
						continue
					if bi < 0 or bi >= rest_world.size() or bi >= pose_world.size():
						continue
					var rest: Transform3D = rest_world[bi]
					var pose: Transform3D = pose_world[bi]
					p += (pose * rest.affine_inverse() * v) * w
			min_v.x = minf(min_v.x, p.x)
			min_v.y = minf(min_v.y, p.y)
			min_v.z = minf(min_v.z, p.z)
			max_v.x = maxf(max_v.x, p.x)
			max_v.y = maxf(max_v.y, p.y)
			max_v.z = maxf(max_v.z, p.z)
			sum += p
			out["vertex_count"] = int(out["vertex_count"]) + 1
	var count := int(out["vertex_count"])
	if count > 0:
		var size := max_v - min_v
		var center := (min_v + max_v) * 0.5
		out["aabb_min"] = min_v
		out["aabb_max"] = max_v
		out["aabb_size"] = size
		out["aabb_center"] = center
		out["centroid"] = sum / float(count)
	return out


func _sit_visual_summary(skeletal: NovaSkeletalAnim, data: NovaObjectData, bones: Array, rest_world: Array) -> Array:
	var out: Array = []
	var keys: PackedStringArray = skeletal.get_clip_keys()
	for key in keys:
		var name := String(key)
		if not name.begins_with("anim_sit"):
			continue
		var pose_world := _accumulate_bone_worlds(bones, skeletal.eval_pose(name, 0.0))
		var hips := _bone_world_position(bones, pose_world, "BN01 Hips")
		var head := _bone_world_position(bones, pose_world, "BN15 Head")
		var body := _skinned_mesh_stats(data, bones.size(), rest_world, pose_world)
		var row := {
			"key": name,
			"head_from_hips": head - hips,
		}
		if body.has("centroid"):
			row["centroid_from_hips"] = (body["centroid"] as Vector3) - hips
		if body.has("aabb_center"):
			row["aabb_center_from_hips"] = (body["aabb_center"] as Vector3) - hips
		out.append(row)
	out.sort_custom(func(a, b): return String(a["key"]).naturalnocasecmp_to(String(b["key"])) < 0)
	return out


func _accumulate_bone_worlds(bones: Array, pose: Array) -> Array:
	var world: Array = []
	world.resize(mini(bones.size(), pose.size()))
	for i in range(world.size()):
		var bd: Dictionary = bones[i]
		var local: Transform3D = pose[i]
		var parent := int(bd.get("parent_index", -1))
		if parent >= 0 and parent < i and world[parent] is Transform3D:
			world[i] = (world[parent] as Transform3D) * local
		else:
			world[i] = local
	return world


func _interesting_bones(bones: Array, world: Array) -> Array:
	var out: Array = []
	for i in range(mini(bones.size(), world.size())):
		var name := String((bones[i] as Dictionary).get("name", ""))
		var lower := name.to_lower()
		if i < 8 or lower.contains("pelv") or lower.contains("hip") or lower.contains("root") or lower.contains("spine") or lower.contains("bn"):
			var xf: Transform3D = world[i]
			out.append({
				"index": i,
				"name": name,
				"parent_index": int((bones[i] as Dictionary).get("parent_index", -1)),
				"position": xf.origin,
			})
	return out


func _sit_clip_summary(skeletal: NovaSkeletalAnim, bones: Array) -> Array:
	var out: Array = []
	var keys: PackedStringArray = skeletal.get_clip_keys()
	for key in keys:
		var name := String(key)
		if not name.begins_with("anim_sit"):
			continue
		var pose: Array = skeletal.eval_pose(name, 0.0)
		var world := _accumulate_bone_worlds(bones, pose)
		var hips := _bone_world_position(bones, world, "BN01 Hips")
		var head := _bone_world_position(bones, world, "BN15 Head")
		var r_foot := _bone_world_position(bones, world, "BN18 R Foot")
		var l_foot := _bone_world_position(bones, world, "BN19 L Foot")
		var r_hand := _bone_world_position(bones, world, "BN17 R Hand")
		var l_hand := _bone_world_position(bones, world, "BN16 L Hand")
		out.append({
			"key": name,
			"hips": hips,
			"head": head,
			"head_from_hips": head - hips,
			"r_foot_from_hips": r_foot - hips,
			"l_foot_from_hips": l_foot - hips,
			"r_hand_from_hips": r_hand - hips,
			"l_hand_from_hips": l_hand - hips,
		})
	out.sort_custom(func(a, b): return String(a["key"]).naturalnocasecmp_to(String(b["key"])) < 0)
	return out


func _bone_world_position(bones: Array, world: Array, wanted: String) -> Vector3:
	for i in range(mini(bones.size(), world.size())):
		if String((bones[i] as Dictionary).get("name", "")) == wanted:
			var xf: Transform3D = world[i]
			return xf.origin
	return Vector3.ZERO


func _bone_name_list(bones: Array) -> Array:
	var out: Array = []
	for i in range(bones.size()):
		var bd: Dictionary = bones[i]
		out.append({
			"index": i,
			"name": String(bd.get("name", "")),
			"parent_index": int(bd.get("parent_index", -1)),
		})
	return out


func _sanitize(value: Variant) -> Variant:
	if value is Vector3:
		var v: Vector3 = value
		return [v.x, v.y, v.z]
	if value is Dictionary:
		var out := {}
		for key in value:
			out[String(key)] = _sanitize(value[key])
		return out
	if value is Array:
		var out: Array = []
		for item in value:
			out.append(_sanitize(item))
		return out
	return value
