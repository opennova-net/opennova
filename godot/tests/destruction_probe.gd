extends Node

# Live destruction probe (world-wac-ai-re §24): boots ONED play-in-editor on a
# real mission (weapon_round_probe's shape), plants a retail destructible ~10 u
# in front of the player start, then holds fire on it through the REAL input
# path and reports every gate of the chain — the entity's sim state
# (bound_radius / traits / health via NovaSimulation.get_entity_debug), the
# health drain, the husk-swap/burst/sound events (destruction present stats),
# and before/after captures. Windowed run:
#   NOVA_RESOURCE_DIR=<assets> "$GODOT_BIN" --path godot res://tests/destruction_probe.tscn
# Optional: NOVA_MISSION_BMS (default 05TR.bms), NOVA_DP_ITEM (def id, default
# 104301 "Group of metal barrels" — hp 155, armor 10 10, husk mbarel2x).

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const EditorScene := preload("res://modtools/editor/editor_main.tscn")
const OUT_DIR := "res://../.scratch/destruction"

var _out_abs := ""
var _world = null


func _ready() -> void:
	_out_abs = ProjectSettings.globalize_path(OUT_DIR)
	DirAccess.make_dir_recursive_absolute(_out_abs)
	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
	var expn := OS.get_environment("NOVA_WR_EXPANSION").strip_edges()
	ResourceDirSettings.set_expansion(expn)
	ResourceDirSettings.set_resource_dir(root)

	var app = EditorScene.instantiate()
	add_child(app)
	await get_tree().process_frame
	for _i in 8:
		await get_tree().process_frame
	var ws_station = app.workstation
	ws_station.set_resource_root_dir(root)
	NovaWindow.set_fullscreen(get_window(), true)
	await _settle(6)

	var bms := OS.get_environment("NOVA_MISSION_BMS").strip_edges()
	if bms.is_empty():
		bms = "05TR.bms"
	var ws = ws_station.get_workspace_adapter(EditorWorkstation.Workspace.MISSION)
	var path := NovaPaths.resolve_file(root, bms)
	if ws.open_file(path) != OK:
		push_error("[dp] open failed")
		get_tree().quit(1)
		return
	ws_station.set_active_workspace(EditorWorkstation.Workspace.MISSION)
	await _settle(30)

	# Plant the destructible in front of the player start (the 60xx start-family
	# marker the spawn selector uses), facing the spawn look direction.
	var controller = ws.get_editor_document()
	var mission: NovaMissionData = controller.get_mission() if controller != null else null
	if mission == null:
		push_error("[dp] no mission document")
		get_tree().quit(1)
		return
	var start_pos := Vector3.ZERO
	var start_yaw := 0.0
	var found_start := false
	for e_v in mission.get_all_entities():
		var e: Dictionary = e_v
		var iid := int(e.get("item_id", 0))
		if iid >= 106000 and iid <= 106099:  # the start-marker family
			start_pos = e.get("position", Vector3.ZERO)
			start_yaw = float((e.get("rotation_deg", Vector3.ZERO) as Vector3).y)
			found_start = true
			break
	if not found_start:
		print("[dp] no 60xx start marker found — using origin")
	# The spawn faces an unknown way (05TR starts INSIDE a tent) — plant a RING
	# of barrel groups around the start at close range: their ~3.3 u bound
	# spheres subtend the whole eye line at 4 u, so a level burst hits one in
	# ANY facing, with no aim control and nothing intervening. Mission
	# coordinates: x = east, y = north, z = UP — the ring lies on x/y.
	var item_def_id := int(OS.get_environment("NOVA_DP_ITEM").to_int())
	if item_def_id == 0:
		item_def_id = 104301  # Group of metal barrels: hp 155, armor 10 10, husk mbarel2x
	# The default barrel group is a decoration; NOVA_DP_KIND overrides the
	# entity kind for non-decoration NOVA_DP_ITEM picks.
	var kind := int(OS.get_environment("NOVA_DP_KIND").to_int())
	if kind == 0:
		kind = NovaMissionData.kind_for_item_type(NovaItemDatabase.TYPE_DECORATION)
	var planted_ids: Array[int] = []
	for offset in [Vector3(4, 0, 0), Vector3(-4, 0, 0), Vector3(0, 4, 0), Vector3(0, -4, 0)]:
		var rec: Dictionary = mission.add_entity(kind, item_def_id, start_pos + offset, Vector3.ZERO)
		if not rec.is_empty():
			planted_ids.append(int(rec.get("bms_id", 0)))
	print("[dp] planted %d x item %d around start %s (yaw %.0f): bms %s" % [
			planted_ids.size(), item_def_id, str(start_pos), start_yaw, str(planted_ids)])
	var planted_bms_id := planted_ids[0] if not planted_ids.is_empty() else 0

	if int(ws.play_mission()) != OK:
		push_error("[dp] play failed")
		get_tree().quit(1)
		return
	await _settle(120)
	_world = _find_by_method(get_tree().root, "get_destruction_present_stats")
	var sim = null
	var runtime = _find_by_method(get_tree().root, "get_sim")
	if runtime != null:
		sim = runtime.get_sim()
	if _world == null or sim == null:
		push_error("[dp] no game world/sim")
		get_tree().quit(1)
		return

	# --- The gate dump: what does the sim know about the planted targets? ---
	var dbg: Dictionary = {}
	if sim.has_method("get_destruction_debug") and planted_bms_id != 0:
		dbg = sim.get_destruction_debug(planted_bms_id)
	print("[dp] target sim state: ", dbg if not dbg.is_empty() else "<entity not found!>")
	await _capture("00_before.png")

	# --- Fire on it: level the look slightly down, hold LMB. ---
	_look(Vector2(0, 120))
	await _settle(10)
	_mouse_btn(MOUSE_BUTTON_LEFT, true)
	for burst in range(10):
		await _settle(62)  # ~1 s of auto fire per report
		var d2: Dictionary = sim.get_destruction_debug(planted_bms_id) \
				if sim.has_method("get_destruction_debug") and planted_bms_id != 0 else {}
		var stats = _world.get_destruction_present_stats()
		print("[dp] t+%ds health=%s swaps=%s bursts=%s sounds=%s fx=%s" % [
				burst + 1, str(d2.get("health", "?")),
				str(stats.husk_swaps) if stats != null else "-",
				str(stats.bursts) if stats != null else "-",
				str(stats.sounds) if stats != null else "-",
				str(stats.effects) if stats != null else "-"])
		# Reload halfway so the magazine doesn't gate the volley.
		if burst == 4:
			_mouse_btn(MOUSE_BUTTON_LEFT, false)
			_hold(KEY_R, true)
			await _settle(2)
			_hold(KEY_R, false)
			await _settle(150)
			_mouse_btn(MOUSE_BUTTON_LEFT, true)
		if stats != null and int(stats.husk_swaps) > 0:
			break
	_mouse_btn(MOUSE_BUTTON_LEFT, false)
	await _settle(60)
	await _capture("01_after.png")

	# The husk VISUAL, not just the swap event: the batched-static path carves
	# the MultiMesh instance and grafts a "HuskModel_<bms>" node into the
	# mission container; per-entity nodes get a "HuskModel" child.
	var husk_node: Node3D = null
	for n in get_tree().root.find_children("HuskModel*", "Node3D", true, false):
		husk_node = n as Node3D
		break
	if husk_node != null:
		print("[dp] husk graft: %s at %s" % [husk_node.name,
				str(husk_node.global_position)])
		await _capture_at(husk_node, "02_husk.png")
	else:
		print("[dp] husk graft: NONE found in tree")

	var final_stats = _world.get_destruction_present_stats()
	var final_dbg: Dictionary = sim.get_destruction_debug(planted_bms_id) \
			if sim.has_method("get_destruction_debug") and planted_bms_id != 0 else {}
	print("[dp] FINAL health=%s alive=%s flags=%s | swaps=%s no_husk=%s bursts=%s pieces_peak=%s sounds=%s fx=%s crackles=%s" % [
			str(final_dbg.get("health", "?")), str(final_dbg.get("alive", "?")),
			str(final_dbg.get("engine_flags", "?")),
			str(final_stats.husk_swaps) if final_stats != null else "-",
			str(final_stats.no_husk) if final_stats != null else "-",
			str(final_stats.bursts) if final_stats != null else "-",
			str(final_stats.pieces_peak) if final_stats != null else "-",
			str(final_stats.sounds) if final_stats != null else "-",
			str(final_stats.effects) if final_stats != null else "-",
			str(final_stats.crackles) if final_stats != null else "-"])
	var destroyed := final_stats != null and int(final_stats.husk_swaps) > 0 \
			and husk_node != null
	print("[dp] VERDICT: %s" % ("DESTROYED — husk model presented" if destroyed
			else "NOT DESTROYED — see the gate dump above"))
	print("[dp] done -> ", _out_abs)
	get_tree().quit(0 if destroyed else 2)


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


func _hold(k: Key, down: bool) -> void:
	var e := InputEventKey.new()
	e.keycode = k
	e.physical_keycode = k
	e.pressed = down
	Input.parse_input_event(e)


func _mouse_btn(b: MouseButton, down: bool) -> void:
	var e := InputEventMouseButton.new()
	e.button_index = b
	e.pressed = down
	Input.parse_input_event(e)


func _look(total: Vector2) -> void:
	for _i in 10:
		var mm := InputEventMouseMotion.new()
		mm.relative = total / 10.0
		Input.parse_input_event(mm)


func _capture(name: String) -> void:
	await RenderingServer.frame_post_draw
	var img: Image = get_viewport().get_texture().get_image()
	if img != null:
		img.save_png(_out_abs.path_join(name))
		print("[dp] wrote ", name)


# Frame `target` from a temporary orbit camera in ITS viewport (the embedded
# play view), capture, then hand the view back to the player camera.
func _capture_at(target: Node3D, name: String) -> void:
	var vp := target.get_viewport()
	if vp == null:
		return
	var prev := vp.get_camera_3d()
	var cam := Camera3D.new()
	target.add_child(cam)
	cam.global_position = target.global_position + Vector3(5, 3, 5)
	cam.look_at(target.global_position + Vector3(0, 0.5, 0))
	cam.make_current()
	await _settle(3)
	await RenderingServer.frame_post_draw
	var img: Image = vp.get_texture().get_image()
	if img != null:
		img.save_png(_out_abs.path_join(name))
		print("[dp] wrote ", name)
	if prev != null and is_instance_valid(prev):
		prev.make_current()
	cam.queue_free()
