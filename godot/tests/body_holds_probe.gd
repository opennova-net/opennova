extends Node

# Live probe for the weapon-channel HOLD POSES + ATTACK STAMPS (D-INF-11 §14.8.4): boots
# the standalone game (JOX root, 05TR) exactly like body_reload_probe, switches to third
# person (F4), and asserts per equipped def (NOVA_VM_WEAPON picks it; the mode branches
# on the def's kinds):
#   pistol (WPN_colt45, special_hold 2): steady-state body_anim_key = anim_pistol; a
#     burst + R plays anim_reload2 (NOT anim_reload — the kind-2 selection), then back.
#   knife (WPN_KNIFE, special_hold 1 / attack_anim 1): steady anim_knife; a click stamps
#     anim_knife_attack immediately, and the locked clip settles back to anim_knife.
# All completion waits are BY STATE, never frame counts. Screenshots -> .scratch/body.
# [orig: the kind ladder @0x4b5dc0..0x4b5e6f; the fire stamp @0x542bcb/0x542be0]

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const StandaloneProbe := preload("res://tests/standalone_game_probe.gd")
const OUT_DIR := "res://../.scratch/body"

var _play_viewport: Viewport = null
var _out_abs := ""
var _fail := 0


func _ready() -> void:
	_out_abs = ProjectSettings.globalize_path(OUT_DIR)
	DirAccess.make_dir_recursive_absolute(_out_abs)
	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
	var weapon := OS.get_environment("NOVA_VM_WEAPON").strip_edges()
	if weapon.is_empty():
		weapon = "WPN_colt45"
	var knife_mode := weapon.to_upper().contains("KNIFE")

	NovaWindow.set_fullscreen(get_window(), true)
	await _settle(6)

	var bms := OS.get_environment("NOVA_MISSION_BMS").strip_edges()
	if bms.is_empty():
		bms = "05TR.bms"
	var session: Dictionary = await StandaloneProbe.boot(
		self, root, bms, ResourceDirSettings.get_expansion())
	if not String(session.get("error", "")).is_empty():
		push_error("[holds] " + String(session.error)); get_tree().quit(1); return
	_play_viewport = session.viewport

	var world: GameWorld = session.world
	var presenter := _find_by_method(get_tree().root, "set_debug_force_viewmodel")
	if world == null or presenter == null:
		push_error("[holds] no weapon world/presenter"); get_tree().quit(1); return

	# Third person (F4 through the real key path), clear the spawn tents.
	_hold(KEY_F4, true); await _settle(2); _hold(KEY_F4, false)
	_hold(KEY_W, true)
	await _settle(240)
	_hold(KEY_W, false)
	await _settle(40)

	# Steady-state hold pose: the special_hold kind keeps the channel OFF the mirror,
	# so the key is exposed directly after equip [orig: @0x4b5dc0..0x4b5e35].
	var hold_key := "anim_knife" if knife_mode else "anim_pistol"
	var v0 = world.local_player_weapon_view()
	print("[holds] %s steady: %s" % [weapon, _body_str(v0)])
	_check(String(v0.body_anim_key) == hold_key,
			"steady-state hold pose is %s (got '%s')" % [hold_key, v0.body_anim_key])
	await _capture("01_hold_%s.png" % ("knife" if knife_mode else "pistol"))

	if knife_mode:
		# The fire click stamps 62 knife_attack IMMEDIATELY [orig: @0x542bcb].
		_mouse_btn(MOUSE_BUTTON_LEFT, true)
		await _settle(4)
		_mouse_btn(MOUSE_BUTTON_LEFT, false)
		var hit := await _wait_body_key(world, "anim_knife_attack", 30)
		var mid = world.local_player_weapon_view()
		print("[holds] attack: ", _body_str(mid))
		_check(hit, "fire stamps anim_knife_attack")
		if hit:
			var pa := int(mid.body_anim_phase)
			await _settle(6)
			var pb := int(world.local_player_weapon_view().body_anim_phase)
			_check(pb > pa, "attack playhead advances (%d -> %d)" % [pa, pb])
		await _capture("02_knife_attack.png")
		# The locked 62 exits at clip end back to the deferred hold [orig: @0x40b77b].
		_check(await _wait_body_key(world, hold_key, 60),
				"attack settles back to %s" % hold_key)
	else:
		# Burst (magazine not full = the reload input gate), then R: the pistol kind
		# selects 66 reload2, not 65 [orig: @0x4b5e67..0x4b5e6f].
		_mouse_btn(MOUSE_BUTTON_LEFT, true)
		await _settle(20)
		_mouse_btn(MOUSE_BUTTON_LEFT, false)
		await _settle(12)
		_hold(KEY_R, true)
		await _settle(3)
		_hold(KEY_R, false)
		var hit := await _wait_body_key(world, "anim_reload2", 40)
		var mid = world.local_player_weapon_view()
		print("[holds] mid-reload: ", _body_str(mid))
		_check(hit, "pistol reload plays anim_reload2")
		_check(String(mid.body_anim_key) != "anim_reload", "NOT the rifle anim_reload")
		await _capture("02_pistol_reload2.png")
		# Completion BY STATE, then the hold pose returns.
		var waits := 0
		while world.local_player_weapon_view().current_action != 0 and waits < 60:
			await _settle(10)
			waits += 1
		await _settle(90)  # drain the 80-tick window past the FSM exit
		var post = world.local_player_weapon_view()
		print("[holds] post-reload: ", _body_str(post))
		_check(String(post.body_anim_key) == hold_key,
				"post-reload back to %s" % hold_key)
	await _capture("03_post.png")

	print("[holds] done -> ", _out_abs, "  failures=", _fail)
	get_tree().quit(1 if _fail > 0 else 0)


# Poll (by state, bounded) until the exposed body key equals `key`.
func _wait_body_key(world: Node, key: String, tries: int) -> bool:
	for _i in tries:
		if String(world.local_player_weapon_view().body_anim_key) == key:
			return true
		await _settle(2)
	return false


func _check(ok: bool, what: String) -> void:
	if ok:
		print("[holds] OK: ", what)
	else:
		_fail += 1
		push_error("[holds] FAIL: " + what)


func _body_str(v) -> String:
	if v == null:
		return "<null>"
	return "act=%d clip=%d res=%d body_key=%s body_phase=%d" % [
		v.current_action, v.clip, v.reserve, v.body_anim_key, v.body_anim_phase]


func _mouse_btn(b: MouseButton, down: bool) -> void:
	var e := InputEventMouseButton.new()
	e.button_index = b
	e.pressed = down
	Input.parse_input_event(e)


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


func _capture(name: String) -> void:
	await RenderingServer.frame_post_draw
	var vp: Viewport = _play_viewport if _play_viewport != null else get_viewport()
	var img: Image = vp.get_texture().get_image()
	if img != null:
		img.save_png(_out_abs.path_join(name))
		print("[holds] wrote ", name)
