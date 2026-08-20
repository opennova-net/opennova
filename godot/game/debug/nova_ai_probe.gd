class_name NovaAiProbe
extends RefCounted
## Env-gated sim-truth probe for the autonomous self-test loop (net-capture
## branch): with NW_AI_PROBE=<path> set, appends one JSON line every interval
## carrying a compact card for EVERY AI entity — position, anim, mount state —
## so a capture diff can separate "the sim never moved it" from "the 0x0A
## interest scheduler never resent it". Inert without the env var.

const INTERVAL_S := 5.0

var _path := OS.get_environment("NW_AI_PROBE")
var _accum := 0.0

# Screenshot hook (NW_SHOT_DIR + NW_SHOT_SSN): each interval a chase camera
# tracks the target entity and the main viewport is dumped to PNG, so the
# self-test loop hands out a human-checkable frame. Inert without the envs.
var _shot_dir := OS.get_environment("NW_SHOT_DIR")
var _shot_ssn := OS.get_environment("NW_SHOT_SSN").to_int()
var _shot_cam: Camera3D = null
var _shot_n := 0

func tick(world: GameWorld, delta: float) -> void:
	if world == null or (_path.is_empty() and _shot_dir.is_empty()):
		return
	_accum += delta
	if _accum < INTERVAL_S:
		return
	_accum = 0.0
	_shot_tick(world)
	if _path.is_empty():
		return
	var sim: Simulation = world.get_sim()
	if sim == null:
		return
	var cards: Array = []
	for i in range(int(sim.get_entity_count())):
		var card: Dictionary = sim.get_entity_debug(i)
		if card.is_empty():
			continue
		var pos: Vector3 = card.get("position", Vector3.ZERO)
		cards.append({
			"i": i,
			"bms": int(card.get("bms_id", 0)),
			"item": int(card.get("item_id", 0)),
			"team": int(card.get("team", -1)),
			"wp": int(card.get("waypoint_id", 0)),
			"num": int(card.get("wp_number", 0)),
			"hp": int(card.get("health", 0)),
			"mounted": bool(card.get("mounted", false)),
			"pos": [pos.x, pos.y, pos.z],
			"yaw": float(card.get("yaw_deg", 0.0)),
			"anim": int(card.get("anim_state", -1)),
			# think state: why a soldier is (not) walking
			"mm": int(card.get("infantry_move_mode", -1)),
			"ch": int(card.get("wp_channel", -1)),
			"node": int(card.get("wp_node", -1)),
			"wpd": int(card.get("wp_distance", -1)),
			"spd": int(card.get("out_speed", -1)),
			"st": int(card.get("state", -1)),
			"adm": String(card.get("adm_name", "")),
			"inf": bool(card.get("infantry", false)),
			"flags": int(card.get("engine_flags", 0)),
			"bflags": int(card.get("bms_flags", 0)),
			"alert": int(card.get("alert", -1)),
			"tgt": bool(card.get("combat_target_valid", false)),
		})
	if cards.is_empty():
		return
	var f: FileAccess
	if FileAccess.file_exists(_path):
		f = FileAccess.open(_path, FileAccess.READ_WRITE)
		if f != null:
			f.seek_end()
	else:
		f = FileAccess.open(_path, FileAccess.WRITE)
	if f == null:
		return
	f.store_line(JSON.stringify({"ms": Time.get_ticks_msec(), "ai": cards}))
	f.close()


func _shot_tick(world: GameWorld) -> void:
	if _shot_dir.is_empty() or _shot_ssn == 0:
		return
	var sim: Simulation = world.get_sim()
	if sim == null:
		return
	var stride: int = sim.get_present_stride()
	if stride <= 0:
		return
	var snap: PackedFloat32Array = sim.get_present_snapshot()
	var target := Vector3.ZERO
	var found := false
	var count: int = snap.size() / stride
	for i in range(count):
		var base := i * stride
		if int(snap[base + Simulation.PF_BMS_ID]) == _shot_ssn:
			target = Vector3(
					snap[base + Simulation.PF_POS_X],
					snap[base + Simulation.PF_POS_Y],
					snap[base + Simulation.PF_POS_Z])
			found = true
			break
	if not found:
		return
	# Capture the frame the previous placement rendered before re-aiming, so
	# every saved PNG shows the tracked entity, not a mid-swing camera.
	if _shot_cam != null and is_instance_valid(_shot_cam) and _shot_n > 0:
		var img: Image = world.get_viewport().get_texture().get_image()
		if img != null:
			img.save_png(_shot_dir.path_join("shot.%03d.png" % _shot_n))
	if _shot_cam == null or not is_instance_valid(_shot_cam):
		_shot_cam = Camera3D.new()
		_shot_cam.far = 8000.0
		world.add_child(_shot_cam)
	_shot_cam.make_current()
	_shot_cam.global_position = target + Vector3(18.0, 9.0, 18.0)
	_shot_cam.look_at(target)
	_shot_n += 1
