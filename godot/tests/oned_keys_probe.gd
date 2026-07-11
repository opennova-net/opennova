extends Node

# ONED play-in-editor key-parity probe: boots the REAL editor (editor_main.tscn),
# plays a mission, then drives F4 (camera mode), F3 (debug overlay), C/Z (stance),
# and Shift (armory / use-item) through Input.parse_input_event — the same global pipeline the
# user's keyboard feeds — and reports which gestures actually land. Reproduces (or
# refutes) the "F3/F4/armory dead in editor preview" report at the routing layer
# the GUT unit tests bypass (they call handle_viewport_input directly).

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const EditorScene := preload("res://modtools/editor/editor_main.tscn")

var _fails := 0


func _ready() -> void:
	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
	ResourceDirSettings.set_resource_dir(root)

	var app = EditorScene.instantiate()
	add_child(app)
	for _i in 8:
		await get_tree().process_frame
	var ws_station = app.workstation
	ws_station.set_resource_root_dir(root)
	await _settle(6)

	var bms := OS.get_environment("NOVA_MISSION_BMS").strip_edges()
	if bms.is_empty():
		bms = "00TRa.bms"
	var ws = ws_station.get_workspace_adapter(EditorWorkstation.Workspace.MISSION)
	var path := NovaPaths.resolve_file(root, bms)
	if ws.open_file(path) != OK:
		push_error("[keys] open failed")
		get_tree().quit(1)
		return
	ws_station.set_active_workspace(EditorWorkstation.Workspace.MISSION)
	await _settle(30)
	if int(ws.play_mission()) != OK:
		push_error("[keys] play failed")
		get_tree().quit(1)
		return
	await _settle(120)

	var play = ws._play_node()
	var host = play.get_player_host() if play != null else null
	if host == null:
		push_error("[keys] no player host")
		get_tree().quit(1)
		return

	# F4: first person -> third person on the shared host.
	var tp_before := bool(host.get("_third_person"))
	_tap(KEY_F4)
	await _settle(4)
	var tp_after := bool(host.get("_third_person"))
	_check(tp_after != tp_before, "F4 flips the camera mode (%s -> %s)" % [tp_before, tp_after])

	# F3: the workspace debug overlay opens.
	var overlay_before: bool = ws.is_debug_overlay_open()
	_tap(KEY_F3)
	await _settle(4)
	var overlay_after: bool = ws.is_debug_overlay_open()
	_check(overlay_after and not overlay_before, "F3 opens the workspace debug overlay")
	if overlay_after:
		_tap(KEY_F3)
		await _settle(4)
		_check(not ws.is_debug_overlay_open(), "F3 again closes it")

	# C: stance crouch toggles on the host.
	var crouch_before := bool(host.get("_crouch"))
	_tap(KEY_C)
	await _settle(4)
	_check(bool(host.get("_crouch")) != crouch_before, "C toggles crouch")

	# Shift: the armory — the USE-ITEM key (action 177, retail default SHIFT), via
	# the shared NovaArmoryHost (zone-gated). Out of zone the key is ignored (the
	# original's silent gate); in zone the WEAPON overlay opens.
	var world = play.get_world()
	var sim = world.get_sim() if world != null and world.has_method("get_sim") else null
	var in_zone: bool = sim != null and sim.has_method("local_player_in_armory_zone") \
			and sim.local_player_in_armory_zone()
	var armory = play.get("_armory")
	_check(armory != null, "ONED play mounts the shared armory host")
	_tap(KEY_SHIFT)
	await _settle(6)
	if armory != null:
		if in_zone:
			_check(bool(armory.is_open()), "Shift opens the WEAPON overlay in an armory zone")
			_tap(KEY_ESCAPE)
			await _settle(4)
			_check(not bool(armory.is_open()), "Esc closes the armory overlay")
		else:
			_check(not bool(armory.is_open()), "Shift out of zone stays ignored [orig: @0x4e0b4d]")
	# The shared HUD host is mounted (crosshair/ammo/scope card parity in PIE).
	_check(play.get("_hud_host") != null, "ONED play mounts the shared HUD host")

	ws.stop_play_mission()
	print("[keys] result: %s" % ("ALL OK" if _fails == 0 else "%d FAILED" % _fails))
	get_tree().quit(1 if _fails > 0 else 0)


func _check(ok: bool, what: String) -> void:
	if ok:
		print("[keys] PASS: ", what)
	else:
		_fails += 1
		print("[keys] FAIL: ", what)


func _tap(k: Key) -> void:
	var down := InputEventKey.new()
	down.keycode = k
	down.physical_keycode = k
	down.pressed = true
	Input.parse_input_event(down)
	var up := InputEventKey.new()
	up.keycode = k
	up.physical_keycode = k
	up.pressed = false
	Input.parse_input_event(up)


func _settle(frames: int) -> void:
	for _i in frames:
		await get_tree().process_frame
