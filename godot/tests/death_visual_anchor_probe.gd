extends SceneTree

# Asset-gated diagnostic for the visible victim jump on the first death frame.
# Builds one real retail-presented NPC, kills it with a local-owned RoundSim
# projectile through its live posed collision, then separates:
#   1. authoritative/presented outer entity movement,
#   2. root-bone movement, and
#   3. the lowest posed bone (feet/body-bottom) movement.
#
# OPENNOVA_JO_DIR=<retail JO install> "$GODOT_BIN" --headless \
#   --path godot -s res://tests/death_visual_anchor_probe.gd

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const MissionEntityRegistry := preload("res://engine/world/mission_entity_registry.gd")
const MissionPresentPass := preload("res://engine/world/mission_present_pass.gd")

const DEATH_STATE_MIN := 173
# The entity itself may legitimately settle/move on the kill tick. The bug is
# extra skeletal displacement relative to that authoritative outer movement.
const MAX_SKELETAL_EXTRA_U := 0.02
const MAX_TICKS := 16


func _initialize() -> void:
	call_deferred("_run")


func _fail(message: String) -> void:
	printerr("DEATH_ANCHOR FAIL: %s" % message)
	quit(1)


func _choose_target_item(item_db: NovaItemDatabase, res_root: NovaResourceRoot) -> Dictionary:
	var fallback: Dictionary = {}
	var soldier_fallback: Dictionary = {}
	for value in item_db.get_items():
		var item: Dictionary = value
		if int(item.get("type", -1)) != NovaItemDatabase.TYPE_PERSON:
			continue
		var graphic := String(item.get("graphic", ""))
		var anim_def := String(item.get("anim_def", ""))
		if graphic.is_empty() or anim_def.is_empty():
			continue
		if not res_root.has_file("%s.3di" % graphic):
			continue
		var adm_file := anim_def if anim_def.get_extension().to_lower() == "adm" \
				else "%s.adm" % anim_def
		if not res_root.has_file(adm_file):
			continue
		var display_name := String(item.get("display_name", "")).to_lower()
		if fallback.is_empty() and int(item.get("id", 0)) != 105310:
			fallback = item
		if display_name.contains("enemy") and display_name.contains("soldier"):
			return item
		if soldier_fallback.is_empty() and display_name.contains("soldier") \
				and not display_name.contains("friendly"):
			soldier_fallback = item
	return soldier_fallback if not soldier_fallback.is_empty() else fallback


func _lowest_bone(skeleton: Skeleton3D) -> Dictionary:
	var lowest := Vector3.INF
	var lowest_index := -1
	for bone_index in range(skeleton.get_bone_count()):
		var point := (
				skeleton.global_transform
				* skeleton.get_bone_global_pose(bone_index)).origin
		if lowest_index < 0 or point.y < lowest.y:
			lowest = point
			lowest_index = bone_index
	return {
		"index": lowest_index,
		"name": String(skeleton.get_bone_name(lowest_index))
				if lowest_index >= 0 else "",
		"point": lowest,
	}


func _anchors(model: Node3D, skeleton: Skeleton3D) -> Dictionary:
	var root_point := model.global_position
	if skeleton.get_bone_count() > 0:
		root_point = (
				skeleton.global_transform
				* skeleton.get_bone_global_pose(0)).origin
	return {
		"outer": model.global_position,
		"root": root_point,
		"lowest": _lowest_bone(skeleton),
	}


func _collision_sections(sim: NovaSimulation, wire_handle: int) -> Dictionary:
	var sections := {}
	for value in sim.get_hitbox_debug().get("organics", []):
		var row: Dictionary = value
		if int(row.get("entity_handle", -1)) == wire_handle:
			sections[int(row.get("section", -1))] = (
					row.get("pos", Vector3.INF) as Vector3)
	return sections


func _present_row(sim: NovaSimulation, wire_handle: int) -> Dictionary:
	var snapshot := sim.get_present_snapshot()
	var stride := sim.get_present_stride()
	for row in range(snapshot.size() / stride):
		var base := row * stride
		if int(snapshot[base + NovaSimulation.PF_WIRE_HANDLE]) != wire_handle:
			continue
		return {
			"anim_state": int(snapshot[base + NovaSimulation.PF_ANIM_STATE]),
			"source_state": int(
					snapshot[base + NovaSimulation.PF_ANIM_SOURCE_STATE]),
			"source_phase": int(
					snapshot[base + NovaSimulation.PF_ANIM_SOURCE_PHASE_TICKS]),
			"target_phase": int(
					snapshot[base + NovaSimulation.PF_ANIM_PHASE_TICKS]),
			"blend_weight": float(
					snapshot[base + NovaSimulation.PF_ANIM_BLEND_WEIGHT]),
			"position": Vector3(
					snapshot[base + NovaSimulation.PF_POS_X],
					snapshot[base + NovaSimulation.PF_POS_Y],
					snapshot[base + NovaSimulation.PF_POS_Z]),
		}
	return {}


func _run() -> void:
	var resource_dir := OS.get_environment("OPENNOVA_JO_DIR").strip_edges()
	if resource_dir.is_empty():
		resource_dir = OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if resource_dir.is_empty():
		_fail("set OPENNOVA_JO_DIR or NOVA_RESOURCE_DIR to a retail JO install")
		return

	var res_root := NovaResourceRoot.new()
	var mount_error := int(res_root.mount_runtime(resource_dir, "", false, "jo"))
	if mount_error != OK:
		_fail("retail resource mount failed (%d): %s" % [
			mount_error, res_root.get_last_error()])
		return
	var item_db := NovaItemDatabase.new()
	if item_db.load_from_resource_root(res_root, "items.def") != OK:
		_fail("items.def did not load")
		return

	var target_item := _choose_target_item(item_db, res_root)
	if target_item.is_empty():
		_fail("items.def has no Person with a resolvable model and ADM")
		return
	var target_item_id := int(target_item.get("id", 0))
	print("DEATH_ANCHOR target_item id=%d name=%s graphic=%s adm=%s" % [
		target_item_id,
		String(target_item.get("display_name", "")),
		String(target_item.get("graphic", "")),
		String(target_item.get("anim_def", "")),
	])

	var mission := NovaMissionData.new()
	if mission.create_default() != OK:
		_fail("could not create the synthetic mission")
		return
	var target_ref := mission.add_entity(
			NovaMissionData.KIND_ORGANIC, target_item_id,
			Vector3(0, 8, 0), Vector3.ZERO)
	if target_ref.is_empty():
		_fail("could not add the retail Person target")
		return

	var holder := Node3D.new()
	root.add_child(holder)
	var placer = MissionObjectPlacer.new(res_root, item_db)
	var placement: Dictionary = placer.place(mission, holder)
	if int(placement.get("animated", 0)) != 1:
		_fail("expected one real animated target; placement=%s" % str(placement))
		return
	var container := holder.get_node_or_null("MissionObjects") as Node3D
	if container == null:
		_fail("MissionObjectPlacer did not create MissionObjects")
		return
	var registry = MissionEntityRegistry.new()
	registry.build(container, mission)
	var model := registry.resolve(
			int(target_ref.get("bms_id", 0)),
			NovaMissionData.KIND_ORGANIC,
			int(target_ref.get("index", 0))) as Node3D
	if model == null:
		_fail("the placed target did not resolve through MissionEntityRegistry")
		return
	var skeleton := model.get_skeleton() as Skeleton3D
	if skeleton == null or skeleton.get_bone_count() == 0:
		_fail("the real target has no posed skeleton")
		return

	var sim := NovaSimulation.new()
	sim.enable_listen_server(true)
	if not sim.load_from_mission_data(mission):
		_fail("NovaSimulation did not load the synthetic mission")
		return
	if not sim.has_local_player():
		_fail("listen-server local player did not auto-spawn")
		return
	if int(sim.set_infantry_anim_map(res_root, "E_STAND.adm")) <= 0:
		_fail("the shared infantry animation map did not load")
		return
	sim.resolve_infantry_adm_ids(res_root, item_db)
	sim.resolve_item_traits(item_db)
	sim.resolve_collision_instances(item_db, placer)
	if sim.load_ammo_table(res_root, "ammo.def") != OK:
		_fail("ammo.def did not load")
		return

	var presenter = MissionPresentPass.new()
	presenter.setup(sim, registry)
	sim.step()
	presenter.present()
	var target_card: Dictionary = sim.get_entity_debug(0)
	var target_handle := int(target_card.get("wire_handle", -1))
	var presented := _present_row(sim, target_handle)
	if presented.is_empty():
		_fail("the target did not round-trip through the listen-server client view")
		return
	print("DEATH_ANCHOR present_stats=%s snapshot_rows=%d target_handle=%d" % [
		str(presenter.get_stats()),
		sim.get_present_snapshot().size() / sim.get_present_stride(),
		target_handle,
	])
	sim.debug_set_entity_health(0, 1)
	var hit_point := Vector3.INF
	for value in sim.get_hitbox_debug().get("organics", []):
		var section: Dictionary = value
		if int(section.get("entity_handle", -1)) != target_handle:
			continue
		hit_point = section.get("pos", Vector3.INF)
		if int(section.get("section", -1)) == 14:
			break
	if hit_point == Vector3.INF:
		_fail("the retail NPC has no live posed collision section")
		return
	if int(sim.debug_spawn_round(
			hit_point - Vector3(2, 0, 0), Vector3.RIGHT,
			"AMMO_CAR15_556MM")) < 0:
		_fail("could not spawn the local-owned projectile")
		return

	var previous := _anchors(model, skeleton)
	var previous_collision := _collision_sections(sim, target_handle)
	var previous_card: Dictionary = sim.get_entity_debug(0)
	var previous_present_anim := int(presented.get("anim_state", -1))
	print("DEATH_ANCHOR before tick=-1 alive=%s sim_anim=%d present_anim=%d clip=%s outer=%s root=%s lowest=%s/%s" % [
		str(previous_card.get("alive", true)),
		int(previous_card.get("anim_state", -1)),
		previous_present_anim,
		String(model.get_active_body_clip()),
		str(previous["outer"]),
		str(previous["root"]),
		String((previous["lowest"] as Dictionary)["name"]),
		str((previous["lowest"] as Dictionary)["point"]),
	])

	for tick in range(MAX_TICKS):
		sim.step()
		presenter.present()
		var card: Dictionary = sim.get_entity_debug(0)
		presented = _present_row(sim, target_handle)
		var current := _anchors(model, skeleton)
		var current_collision := _collision_sections(sim, target_handle)
		var previous_lowest: Dictionary = previous["lowest"]
		var current_lowest: Dictionary = current["lowest"]
		var outer_delta := (current["outer"] as Vector3) - (previous["outer"] as Vector3)
		var root_delta := (current["root"] as Vector3) - (previous["root"] as Vector3)
		var lowest_delta := (
				(current_lowest["point"] as Vector3)
				- (previous_lowest["point"] as Vector3))
		var anim_state := int(card.get("anim_state", -1))
		var present_anim := int(presented.get("anim_state", -1))
		print("DEATH_ANCHOR tick=%d alive=%s sim_anim=%d present_anim=%d blend=%d/%d -> %d/%d @%.9f clip=%s sim=%s decoded=%s outer_d=%s root_d=%s lowest=%s->%s lowest_d=%s" % [
			tick,
			str(card.get("alive", true)),
			anim_state,
			present_anim,
			int(presented.get("source_state", -1)),
			int(presented.get("source_phase", -1)),
			present_anim,
			int(presented.get("target_phase", -1)),
			float(presented.get("blend_weight", 1.0)),
			String(model.get_active_body_clip()),
			str(card.get("position", Vector3.INF)),
			str(presented.get("position", Vector3.INF)),
			str(outer_delta),
			str(root_delta),
			String(previous_lowest["name"]),
			String(current_lowest["name"]),
			str(lowest_delta),
		])
		if present_anim >= DEATH_STATE_MIN:
			var outer_distance := outer_delta.length()
			var root_distance := root_delta.length()
			var lowest_distance := lowest_delta.length()
			var root_extra := (root_delta - outer_delta).length()
			var lowest_extra := (lowest_delta - outer_delta).length()
			var collision_max_extra := 0.0
			for section_v in current_collision:
				var section := int(section_v)
				if not previous_collision.has(section):
					continue
				var collision_delta := (
						(current_collision[section] as Vector3)
						- (previous_collision[section] as Vector3))
				collision_max_extra = maxf(
						collision_max_extra,
						(collision_delta - outer_delta).length())
			print("DEATH_ANCHOR first_death tick=%d state=%d key=%s previous_anim=%d outer=%.6f root=%.6f lowest=%.6f skeletal_root_extra=%.6f skeletal_lowest_extra=%.6f collision_max_extra=%.6f" % [
				tick,
				present_anim,
				String(NovaSimulation.infantry_anim_key(present_anim)),
				previous_present_anim,
				outer_distance,
				root_distance,
				lowest_distance,
				root_extra,
				lowest_extra,
				collision_max_extra,
			])
			sim.free()
			if root_extra > MAX_SKELETAL_EXTRA_U \
					or lowest_extra > MAX_SKELETAL_EXTRA_U \
					or collision_max_extra > MAX_SKELETAL_EXTRA_U:
				_fail("first death frame has extra skeletal motion: outer=%.6fu root=%.6fu lowest=%.6fu root_extra=%.6fu lowest_extra=%.6fu collision_extra=%.6fu (limit %.3fu)" % [
					outer_distance, root_distance, lowest_distance,
					root_extra, lowest_extra, collision_max_extra,
					MAX_SKELETAL_EXTRA_U])
				return
			print("DEATH_ANCHOR PASS: no first-frame victim jump")
			quit(0)
			return
		previous = current
		previous_collision = current_collision
		previous_card = card
		previous_present_anim = present_anim

	sim.free()
	_fail("the real round did not reach a death-family animation in %d ticks" %
			MAX_TICKS)
