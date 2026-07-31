extends Node

# Standalone key-routing probe: boots the real game shell, then drives F4
# (camera mode), F3 (debug overlay), C/Z (stance),
# and Shift (armory / use-item) through Input.parse_input_event — the same global pipeline the
# user's keyboard feeds — and reports which gestures actually land. Reproduces (or
# refutes) dead runtime-key reports at the routing layer the GUT unit tests bypass.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const StandaloneProbe := preload("res://tests/standalone_game_probe.gd")

var _fails := 0


func _ready() -> void:
	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
	var bms := OS.get_environment("NOVA_MISSION_BMS").strip_edges()
	if bms.is_empty():
		bms = "00TRa.bms"
	var session: Dictionary = await StandaloneProbe.boot(
		self, root, bms, ResourceDirSettings.get_expansion())
	if not String(session.get("error", "")).is_empty():
		push_error("[keys] " + String(session.error))
		get_tree().quit(1)
		return

	var game: Node = session.game
	var world: GameWorld = session.world
	var presenter = game.find_child("LocalPlayerPresenter", true, false)
	if presenter == null:
		push_error("[keys] no player presenter")
		get_tree().quit(1)
		return

	# F4: first person -> third person on the shared presenter.
	var tp_before := bool(presenter.get("_third_person"))
	_tap(KEY_F4)
	await _settle(4)
	var tp_after := bool(presenter.get("_third_person"))
	_check(tp_after != tp_before, "F4 flips the camera mode (%s -> %s)" % [tp_before, tp_after])

	# F3: the game debug overlay opens.
	var overlay_before: bool = game.is_debug_overlay_open()
	_tap(KEY_F3)
	await _settle(4)
	var overlay_after: bool = game.is_debug_overlay_open()
	_check(overlay_after and not overlay_before, "F3 opens the game debug overlay")
	if overlay_after:
		_tap(KEY_F3)
		await _settle(4)
		_check(not game.is_debug_overlay_open(), "F3 again closes it")

	# C: stance crouch toggles on the presenter.
	var crouch_before := bool(presenter.get("_crouch"))
	_tap(KEY_C)
	await _settle(4)
	_check(bool(presenter.get("_crouch")) != crouch_before, "C toggles crouch")

	# Shift: the armory — the USE-ITEM key (action 177, retail default SHIFT), via
	# the shared NovaArmoryPresenter (zone-gated). Out of zone the key is ignored (the
	# original's silent gate); in zone the WEAPON overlay opens.
	var sim = world.get_sim() if world != null and world.has_method("get_sim") else null
	var in_zone: bool = sim != null and sim.has_method("local_player_in_armory_zone") \
			and sim.local_player_in_armory_zone()
	var armory = game.find_child("ArmoryPresenter", true, false)
	_check(armory != null, "the game mounts the shared armory presenter")
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
	_check(game.find_child("GameHudPresenter", true, false) != null,
		"the game mounts the shared HUD presenter")

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
