extends Node

# Visual probe for the weapon-round slice (PR #226): captures the adjudicated
# crosshair protocol (D-HUD-9/10), the M82/Barrett SIGHTS scope card, and
# unscope-on-move, driven through the REAL input path in ONED play-in-editor
# (same boot shape as fp_clean_probe). Windowed run:
#   NOVA_RESOURCE_DIR=<assets> "$GODOT_BIN" --path godot res://tests/weapon_round_probe.tscn
# Captures land in .scratch/weapon_round/. Note the JOX id is WPN_Barret (one T);
# the ease reads ~2 sim ticks per render frame fullscreen (62.5 Hz vs ~30-45 fps),
# so the mid-ease captures sit a few FRAMES after the RMB edge.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const EditorScene := preload("res://modtools/editor/editor_main.tscn")
const OUT_DIR := "res://../.scratch/weapon_round"
const ENV_NAME := "full_00.env"
const RIG_WEAPON := "WPN_Barret"  # Scoped; M82_1st carries the empty/dup bone rows (rig-fix demo)
const CARD_WEAPON := "WPN_RPG"    # Scoped (flags 1) + 2 sights rows in JOX -> the SIGHTS card

var _out_abs := ""
var _world = null
var _host = null


func _ready() -> void:
	_out_abs = ProjectSettings.globalize_path(OUT_DIR)
	DirAccess.make_dir_recursive_absolute(_out_abs)
	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
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
	if app.environment_editor != null:
		var env_path := NovaPaths.resolve_file(root, ENV_NAME)
		if not env_path.is_empty():
			app.environment_editor.open_env(env_path)

	var bms := OS.get_environment("NOVA_MISSION_BMS").strip_edges()
	if bms.is_empty():
		bms = "05TR.bms"
	var ws = ws_station.get_workspace_adapter(EditorWorkstation.Workspace.MISSION)
	var path := NovaPaths.resolve_file(root, bms)
	if ws.open_file(path) != OK:
		push_error("[wr] open failed"); get_tree().quit(1); return
	ws_station.set_active_workspace(EditorWorkstation.Workspace.MISSION)
	await _settle(30)
	if int(ws.play_mission()) != OK:
		push_error("[wr] play failed"); get_tree().quit(1); return
	await _settle(120)
	_world = _find_by_method(get_tree().root, "local_player_weapon_view")
	_host = _find_by_method(get_tree().root, "set_debug_force_viewmodel")
	if _world == null:
		push_error("[wr] no weapon world"); get_tree().quit(1); return

	# Sanity: the GameplayOverlay must carry the play panel's rect (the PIE
	# overlay-layer fix); a zero size here clips the HUD and armory to nothing.
	var hud_host := _find_by_method(get_tree().root, "hud_objective_line")
	if hud_host != null:
		hud_host.tick()  # force the lazy HUD build
		var hud = hud_host.get_hud()
		if hud != null:
			var ov: Control = hud.get_parent()
			print("[wr] overlay=%s rect=%s hud_rect=%s" % [
				ov.name, str(ov.get_global_rect()), str(hud.get_global_rect())])

	# Try the armory at the spawn tents (zone-gated) BEFORE walking out.
	var armory_done := await _armory_sequence()

	# Open ground, level look.
	_hold(KEY_W, true)
	await _settle(300)
	_hold(KEY_W, false)
	await _settle(30)

	# Baseline: the spread crosshair at the 1P design-center pin (whatever's equipped).
	_log_view("hip baseline")
	await _capture("01_hip_crosshair.png")

	if not armory_done:
		await _equip_direct(RIG_WEAPON)
	_log_view("barrett hip (rig fix)")
	await _capture("02_barrett_rig_hip.png")

	# The card demo weapon: Scoped + authored sights rows (the snipers use the
	# separate magnified-scope overlay — the recorded hud-re deferral).
	await _equip_direct(CARD_WEAPON)
	_log_view("rpg hip")
	await _capture("02b_rpg_hip.png")

	# Mid-ease: the crosshair MUST still draw a few frames after the RMB edge
	# (D-HUD-9: it yields only at the settled sight view). ~2 sim ticks/frame.
	_mouse_btn(MOUSE_BUTTON_RIGHT, true)
	await _settle(2)
	_mouse_btn(MOUSE_BUTTON_RIGHT, false)
	await _settle(1)
	_log_view("mid-ease")
	await _capture("03_ads_ease_crosshair_up.png")

	# Settled: fraction 1 -> the crosshair yields AND the Scoped weapon draws its
	# SIGHTS card INSTEAD of the FP viewmodel.
	await _settle(50)
	_log_view("settled (card)")
	await _capture("04_sights_card_settled.png")

	# Unscope-on-move: a movement key while SETTLED on a Scoped weapon forces the
	# full unscope [orig: @0x4df4c9]. Capture mid-drop, then at rest.
	_hold(KEY_W, true)
	await _settle(4)
	_log_view("moving (unscope firing)")
	await _capture("05_unscope_on_move_drop.png")
	await _settle(40)
	_hold(KEY_W, false)
	await _settle(20)
	_log_view("stopped (back at hip)")
	await _capture("06_back_at_hip.png")

	# Third person: the crosshair anchors at the PROJECTED aim point, not the
	# screen center (D-HUD-10) — pitch down a touch so the offset is visible.
	_look(Vector2(0, 140))
	await _settle(12)
	_hold(KEY_F4, true)
	await _settle(2)
	_hold(KEY_F4, false)
	await _settle(40)
	_log_view("third person")
	await _capture("07_3p_projected_aim.png")

	print("[wr] done -> ", _out_abs)
	get_tree().quit()


# The armory over live play + the Shift ACCEPT accelerator. Returns true when the
# sniper was applied through the armory (false -> caller falls back to the direct
# apply, still demoing the same ACCEPT plumbing).
func _armory_sequence() -> bool:
	_hold(KEY_SHIFT, true)
	await _settle(4)
	var menu := get_tree().root.find_child("ArmoryMenu", true, false)
	if menu == null or not menu.visible:
		_hold(KEY_SHIFT, false)
		print("[wr] armory did not open here (out of zone) — direct apply later")
		return false
	await _settle(20)
	await _capture("00_armory_open_live_play.png")

	var primary := menu.find_child("PRIMARY", true, false)
	var row := -1
	if primary != null:
		for i in range(primary.get_item_count()):
			if "BARRET" in String(primary.get_item_text(i)).to_upper():
				row = i
				break
		print("[wr] PRIMARY rows=%d barrett_row=%d" % [primary.get_item_count(), row])
	if row >= 0:
		primary.select(row)
		await _settle(10)

	# The ACCEPT hotkey: the opener is still HELD from the open — release once to
	# arm [orig: @0x4de2d0], press again to ACCEPT [orig: @0x5674a8].
	_hold(KEY_SHIFT, false)
	await _settle(5)
	_hold(KEY_SHIFT, true)
	await _settle(3)
	_hold(KEY_SHIFT, false)
	await _settle(60)
	print("[wr] Shift ACCEPT accelerator: menu.visible=%s" % str(menu.visible))
	return row >= 0 and not menu.visible


func _equip_direct(weapon: String) -> void:
	# The ACCEPT plumbing minus the UI: install the FSM + rebuild the viewmodel
	# (game_world.set_local_player_weapon_by_name = the armory apply path).
	if _world == null or not _world.has_method("set_local_player_weapon_by_name"):
		return
	if not _world.set_local_player_weapon_by_name(weapon):
		print("[wr] %s not in this root's weapon.def — staying on the default" % weapon)
		return
	if _host != null and _host.has_method("refresh_viewmodel"):
		_host.refresh_viewmodel()
	await _settle(90)


func _log_view(stage: String) -> void:
	var pv = _world.local_player_view() if _world != null and _world.has_method("local_player_view") else null
	var wv = _world.local_player_weapon_view() if _world != null else null
	print("[wr] %s: engaged=%s fraction=%.2f card=%s clip=%s" % [
		stage,
		str(pv.scope_engaged) if pv != null else "<null>",
		pv.scope_fraction if pv != null else -1.0,
		str(pv.scope_card_active) if pv != null else "<null>",
		str(wv.clip) if wv != null else "<null>"])


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
	var e := InputEventKey.new(); e.keycode = k; e.physical_keycode = k; e.pressed = down
	Input.parse_input_event(e)


func _mouse_btn(b: MouseButton, down: bool) -> void:
	var e := InputEventMouseButton.new()
	e.button_index = b
	e.pressed = down
	Input.parse_input_event(e)


func _look(total: Vector2) -> void:
	for _i in 10:
		var mm := InputEventMouseMotion.new(); mm.relative = total / 10.0
		Input.parse_input_event(mm)


func _capture(name: String) -> void:
	await RenderingServer.frame_post_draw
	var img: Image = get_viewport().get_texture().get_image()
	if img != null:
		img.save_png(_out_abs.path_join(name))
		print("[wr] wrote ", name)
