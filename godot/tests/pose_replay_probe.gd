extends Node

# Pose-replay hit-detection probe: boots the standalone game on the mission a
# NOVA_POSE_JSON snapshot names, then re-fires the dumped camera ray through
# the REAL RoundSim (NovaSimulation.debug_spawn_round) as a small fan around
# the dumped forward, and reports every outcome the F3 Rounds debug ring
# recorded — face hits with section/face/material, sphere stand-ins, terrain
# stops, and the face-miss fly-ons. The tool for "I was standing HERE and the
# shot did something weird": dump a snapshot (F3 -> Player -> Dump snapshot),
# then
#   NOVA_POSE_JSON=<snapshot.json> [NOVA_PR_AMMO=AMMO_M203_40MM_NADE] \
#     [NOVA_PR_PICK=<bms_id>|first] \
#     "$GODOT_BIN" --path godot res://tests/pose_replay_probe.tscn
# NOVA_PR_PICK re-aims the whole fan at a PICKED entity's exact hit point
# (the snapshot's picks[] section) and live-checks that entity before firing
# — the "I picked THAT thing and the shot did something weird" loop.
# Also lists the mission entities within 80 u of the camera as context for
# what SHOULD be along the ray.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const StandaloneProbe := preload("res://tests/standalone_game_probe.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const OUT_DIR := "res://../.scratch/pose_replay"

# Yaw/pitch offsets (degrees) around the dumped forward — the "sometimes" net.
const FAN := [
	Vector2(0, 0),
	Vector2(-2, 0), Vector2(2, 0), Vector2(0, -2), Vector2(0, 2),
	Vector2(-5, 0), Vector2(5, 0), Vector2(0, -5), Vector2(0, 5),
]

var _out_abs := ""


func _ready() -> void:
	_out_abs = ProjectSettings.globalize_path(OUT_DIR)
	DirAccess.make_dir_recursive_absolute(_out_abs)

	var pose_path := OS.get_environment("NOVA_POSE_JSON").strip_edges()
	if pose_path.is_empty():
		push_error("[pr] NOVA_POSE_JSON not set")
		get_tree().quit(1)
		return
	var pose_file := FileAccess.open(pose_path, FileAccess.READ)
	if pose_file == null:
		push_error("[pr] cannot read pose dump: " + pose_path)
		get_tree().quit(1)
		return
	var pose: Dictionary = JSON.parse_string(pose_file.get_as_text())
	var mission_file := String((pose.get("mission", {}) as Dictionary).get("file", ""))
	var cam: Dictionary = (pose.get("view", {}) as Dictionary).get("camera", {})
	var from_g: Vector3 = _v3(cam.get("position_godot", {}))
	var fwd_g: Vector3 = _v3(cam.get("forward_godot", {})).normalized()
	print("[pr] pose: mission=%s cam=%s fwd=%s" % [mission_file, str(from_g), str(fwd_g)])
	if mission_file.is_empty() or fwd_g == Vector3.ZERO:
		push_error("[pr] pose dump missing mission/camera fields")
		get_tree().quit(1)
		return

	# NOVA_PR_PICK: select a picks[] row (bms_id, or "first") and aim the fan
	# at its recorded hit point instead of the dumped forward.
	var pick_target: Dictionary = {}
	var pick_sel := OS.get_environment("NOVA_PR_PICK").strip_edges()
	if not pick_sel.is_empty():
		for p_v in (pose.get("picks", []) as Array):
			var p: Dictionary = p_v
			var identity: Dictionary = p.get("identity", {})
			if pick_sel.to_lower() == "first" \
					or int(identity.get("bms_id", -1)) == pick_sel.to_int():
				pick_target = p
				break
		if pick_target.is_empty():
			push_error("[pr] NOVA_PR_PICK '%s' matched no snapshot pick" % pick_sel)
			get_tree().quit(1)
			return
		var pid: Dictionary = pick_target.get("identity", {})
		var pick_info: Dictionary = pick_target.get("pick", {})
		print("[pr] pick target: %s #%d (%d:%d) item %d handle %d %s" % [
				String(pid.get("name", "?")), int(pid.get("bms_id", 0)),
				int(pid.get("kind", -1)), int(pid.get("index", -1)),
				int(pid.get("item_id", 0)), int(pid.get("entity_handle", -1)),
				String(pick_info.get("hit_class", ""))])
		var hit_g := _v3(pick_info.get("hit_position_godot", {}))
		if hit_g == Vector3.ZERO or (hit_g - from_g).length() < 0.01:
			push_error("[pr] pick carries no usable hit point")
			get_tree().quit(1)
			return
		fwd_g = (hit_g - from_g).normalized()
		print("[pr] fan re-aimed at the picked hit point %s (dist %.1fu)" % [
				str(hit_g), (hit_g - from_g).length()])

	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
	NovaWindow.set_fullscreen(get_window(), true)
	await _settle(6)

	var mission := NovaMissionData.new()
	if mission.open_file(root.path_join(mission_file)) != OK:
		push_error("[pr] open failed: " + mission_file)
		get_tree().quit(1)
		return

	# Context: what does the mission place within 80 u of the camera?
	var cam_bms := Vector3(from_g.x, -from_g.z, from_g.y)
	var near: Array = []
	for e_v in mission.get_all_entities():
		var e: Dictionary = e_v
		var p: Vector3 = e.get("position", Vector3.ZERO)
		var d := Vector2(p.x - cam_bms.x, p.y - cam_bms.y).length()
		if d <= 80.0:
			near.append([d, e])
	near.sort_custom(func(a, b): return a[0] < b[0])
	print("[pr] %d mission entities within 80 u (nearest 20):" % near.size())
	for row in near.slice(0, 20):
		var e2: Dictionary = row[1]
		print("[pr]   %6.1fu  bms %s item %s kind %s at %s rot %s" % [
				row[0], str(e2.get("bms_id", "?")), str(e2.get("item_id", "?")),
				str(e2.get("kind", "?")), str(e2.get("position", Vector3.ZERO)),
				str(e2.get("rotation_deg", Vector3.ZERO))])
		if int(e2.get("bms_id", 0)) == int(OS.get_environment("NOVA_PR_BMS").to_int()):
			var xf := MissionObjectPlacer.entity_transform(
					e2.get("position", Vector3.ZERO),
					e2.get("rotation_deg", Vector3.ZERO))
			print("[pr]   VISUAL xform basis x=%s y=%s z=%s origin=%s" % [
					str(xf.basis.x), str(xf.basis.y), str(xf.basis.z), str(xf.origin)])

	var session: Dictionary = await StandaloneProbe.boot(
		self, root, mission_file,
		OS.get_environment("NOVA_WR_EXPANSION").strip_edges())
	if not String(session.get("error", "")).is_empty():
		push_error("[pr] " + String(session.error))
		get_tree().quit(1)
		return
	var world: GameWorld = session.world
	var sim = world.get_sim()
	if sim == null or not sim.has_method("debug_spawn_round"):
		push_error("[pr] no sim / debug_spawn_round missing (stale DLL?)")
		get_tree().quit(1)
		return

	# Visual-vs-collision cross-check for one placed static (NOVA_PR_BMS): the
	# placer's cached visual world transform against the collision instance's
	# heading + world volume corners — the same-entity two-shell comparison.
	var probe_bms := int(OS.get_environment("NOVA_PR_BMS").to_int())
	if probe_bms != 0:
		var placer = _find_by_method(get_tree().root, "get_static_instance_transform")
		if placer != null:
			var rec: Variant = placer.get_static_instance_transform(probe_bms)
			print("[pr] visual instance bms %d: %s" % [probe_bms, str(rec)])
		var cd: Dictionary = sim.get_collision_debug()
		for inst_v in cd.get("instances", []):
			var inst: Dictionary = inst_v
			var corners0: PackedVector3Array = \
					(inst.get("volumes", []) as Array).front().get("corners", PackedVector3Array()) \
					if not (inst.get("volumes", []) as Array).is_empty() else PackedVector3Array()
			print("[pr] collision inst handle %d pos %s heading %.1f vols %d c0 %s" % [
					int(inst.get("entity_handle", -1)), str(inst.get("pos")),
					float(inst.get("heading", 0.0)),
					(inst.get("volumes", []) as Array).size(),
					str(corners0.slice(0, 2))])

	# NOVA_PR_HITBOXES=1: build the hitbox view (the CFAC wireframes rounds
	# test) and park an orbit camera at the DUMPED pose for a screenshot —
	# the visual-vs-collision eyeball check at the reported spot.
	if OS.get_environment("NOVA_PR_HITBOXES").to_int() == 1:
		var world_node = _find_by_method(get_tree().root, "set_hitbox_debug")
		if world_node != null:
			world_node.set_hitbox_debug(true)
			await _settle(30)
			var vp: Viewport = null
			for n in get_tree().root.find_children("HitboxDebug", "", true, false):
				vp = (n as Node3D).get_viewport()
				break
			if vp != null:
				var prev := vp.get_camera_3d()
				var shot_cam := Camera3D.new()
				vp.add_child(shot_cam)
				shot_cam.global_position = from_g
				shot_cam.look_at(from_g + fwd_g, Vector3.UP)
				shot_cam.current = true
				await _settle(20)
				await RenderingServer.frame_post_draw
				var img: Image = vp.get_texture().get_image()
				if img != null:
					img.save_png(_out_abs.path_join("hitboxes.png"))
					print("[pr] wrote hitboxes.png")
				if prev != null:
					prev.current = true
				shot_cam.queue_free()
		else:
			print("[pr] no world with set_hitbox_debug found")

	# The picked entity's live state before firing: is it still there, and in
	# what shape? (A husked/removed target explains a changed outcome.)
	if not pick_target.is_empty():
		var pid2: Dictionary = (pick_target.get("identity", {}) as Dictionary)
		var pick_bms := int(pid2.get("bms_id", 0))
		if pick_bms != 0 and sim.has_method("get_destruction_debug"):
			var live_card: Dictionary = sim.get_destruction_debug(pick_bms)
			if live_card.is_empty():
				print("[pr] picked entity bms %d: NOT PRESENT in the live world" % pick_bms)
			else:
				print("[pr] picked entity bms %d live: health %s/%s husk %s collision %s" % [
						pick_bms, str(live_card.get("health", "?")),
						str(live_card.get("health_max", "?")),
						str(live_card.get("has_husk", "?")),
						str(live_card.get("has_collision_instance", "?"))])

	var ammo := OS.get_environment("NOVA_PR_AMMO").strip_edges()
	if ammo.is_empty():
		ammo = "AMMO_556"
	# Fire the fan: the exact dumped ray + offsets, one round per 8 ticks so
	# the ring keeps distinct segments.
	var fired := 0
	for off in FAN:
		var dir := _rotated(fwd_g, off.x, off.y)
		var slot: int = sim.debug_spawn_round(from_g, dir, ammo)
		if slot < 0:
			print("[pr] spawn FAILED (ammo '%s' unknown or pool full)" % ammo)
		else:
			fired += 1
		await _settle(8)
	print("[pr] fired %d/%d rays with %s" % [fired, FAN.size(), ammo])
	await _settle(180)  # let long flights resolve (terrain/expiry)

	var debug: Dictionary = sim.get_round_debug()
	var events: Array = debug.get("events", [])
	print("[pr] ---- round debug ring (%d events, oldest first) ----" % events.size())
	for ev_v in events:
		var ev: Dictionary = ev_v
		var line := "[pr] t%-6d %-11s" % [int(ev.get("tick", 0)), String(ev.get("kind_name", "?"))]
		var ent := int(ev.get("entity_handle", 0xFFFF))
		if ent != 0xFFFF:
			line += " ent %d/%-4d" % [(ent >> 12) & 0xF, ent & 0xFFF]
			var nm := String(ev.get("entity_name", ""))
			if not nm.is_empty():
				line += " " + nm
		if bool(ev.get("husk", false)):
			line += " HUSK"
		match int(ev.get("kind", 4)):
			1:
				line += "  sec %d face %d mat %d -> %s" % [int(ev.get("section", -1)),
						int(ev.get("face", -1)), int(ev.get("material", 0)),
						String(ev.get("effect_tag_name", ""))]
			2:
				line += "  sphere -> %s" % String(ev.get("effect_tag_name", ""))
			3:
				line += "  terrain -> %s" % String(ev.get("effect_tag_name", ""))
			5:
				line += "  GRAZE flew on (t %.3f)" % float(ev.get("t", 0.0))
			_:
				pass
		line += "  hit %s" % str(ev.get("hit", Vector3.ZERO))
		print(line)
	print("[pr] done -> ", _out_abs)
	get_tree().quit(0)


static func _v3(d: Dictionary) -> Vector3:
	return Vector3(float(d.get("x", 0.0)), float(d.get("y", 0.0)), float(d.get("z", 0.0)))


# Rotate the forward by yaw_deg around Godot +Y, then pitch_deg around the
# resulting right axis.
static func _rotated(fwd: Vector3, yaw_deg: float, pitch_deg: float) -> Vector3:
	var v := fwd.rotated(Vector3.UP, deg_to_rad(yaw_deg))
	var right := v.cross(Vector3.UP).normalized()
	if right != Vector3.ZERO:
		v = v.rotated(right, deg_to_rad(pitch_deg))
	return v.normalized()


func _find_by_method(node: Node, method: String) -> Node:
	if node.has_method(method):
		return node
	for ch in node.get_children():
		var f := _find_by_method(ch, method)
		if f != null:
			return f
	return null


func _settle(n: int) -> void:
	for _i in n:
		await get_tree().process_frame
