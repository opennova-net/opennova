extends SceneTree

# Remote prone-roll presentation probe — NOT a GUT test (needs retail assets;
# *_probe.gd files are manual, never collected). Builds the wire player avatar
# EXACTLY as WirePresentPass does for a remote player (runtime type 0x14B9 ->
# items.def 105310 "Player #1" -> graphic US01 + anim_def US01.adm), then drives
# the model through the retail remote body-state arbitration with the states the
# authority selects for a prone roll:
#
#   idle_prone(48) -> roll_left(41) -> roll_right(42, queued behind the
#   transition-locked 41, promoted at clip completion)
#
# and asserts the ACTIVE clip follows. Red-capable for the live symptom "the
# opennova client does not show remote players prone rolling": a missing clip
# key, a broken .bad load, or an arbitration drop all fail here.
#
#   NOVA_RESOURCE_DIR=<loose JOX corpus or retail install> "$GODOT_BIN" \
#       --headless --path godot -s res://tests/remote_prone_roll_probe.gd
#
# [orig: selection @0x4b731b-0x4b7354; remote apply arbitration @0x4c1153;
#  clip names g_animStateNameTable @0x8135F0 rows 41/42 = roll_left/roll_right]

const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

var _fails := 0


func _init() -> void:
	call_deferred("_run")


func _check(cond: bool, msg: String) -> void:
	if cond:
		print("[roll-probe] PASS: %s" % msg)
	else:
		_fails += 1
		printerr("[roll-probe] FAIL: %s" % msg)


func _fail_now(msg: String) -> void:
	printerr("[roll-probe] FAIL: %s" % msg)
	quit(1)


func _run() -> void:
	var resource_dir := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if resource_dir.is_empty():
		_fail_now("set NOVA_RESOURCE_DIR to a loose or mounted retail JOX corpus")
		return

	var res_root := NovaResourceRoot.new()
	var mount_err := int(res_root.mount_runtime(resource_dir, "", false, "jo"))
	if mount_err != OK:
		res_root.set_root_dir(resource_dir)
		if not res_root.has_file("US01.adm"):
			_fail_now("resource root failed (%d): %s" % [mount_err, res_root.get_last_error()])
			return

	var placer = MissionObjectPlacer.new()
	placer.resource_root = res_root

	var holder := Node3D.new()
	root.add_child(holder)

	# The same call WirePresentPass makes for a remote player row.
	var model: Node3D = placer.build_player_animated_model(0x14B9, holder, null)
	if model == null:
		_fail_now("build_player_animated_model(0x14B9) returned null")
		return
	print("[roll-probe] avatar node: %s" % model.name)

	var skeletal = model.get_skeletal_anim()
	if skeletal == null:
		_fail_now("avatar has no skeletal anim set (anim_def did not load)")
		return

	_check(NovaSimulation.infantry_anim_key(41) == "anim_roll_left",
			"state 41 maps to anim_roll_left")
	_check(NovaSimulation.infantry_anim_key(42) == "anim_roll_right",
			"state 42 maps to anim_roll_right")
	_check(skeletal.has_clip("anim_idle_prone"), "clip set carries anim_idle_prone")
	_check(skeletal.has_clip("anim_roll_left"), "clip set carries anim_roll_left")
	_check(skeletal.has_clip("anim_roll_right"), "clip set carries anim_roll_right")

	# Byte-15 seed-unit evidence: the wire phase counter is fixed-rate while our
	# seed conversion divides by (2 * clip fps) — print each clip's fps/length so
	# the capture's observed wrap (state 9 wraps at ~72) can be cross-checked.
	for probe_key in ["anim_roll_left", "anim_roll_right", "anim_run_2",
			"anim_walk_forward", "anim_idle", "anim_idle_prone"]:
		if skeletal.has_clip(probe_key):
			print("[roll-probe] clip %s: fps=%.2f length=%.4fs" % [
					probe_key, skeletal.get_clip_fps(probe_key),
					skeletal.get_clip_length(probe_key)])

	# The remote request sequence the wire produces for a prone roll.
	var f48 := int(NovaSimulation.infantry_anim_flags(48))
	var f41 := int(NovaSimulation.infantry_anim_flags(41))
	var f42 := int(NovaSimulation.infantry_anim_flags(42))
	model.apply_remote_body_state(48, "anim_idle_prone", f48, 0)
	_check(String(model.get_active_body_clip()) == "anim_idle_prone",
			"idle_prone accepted (active=%s)" % model.get_active_body_clip())
	model.apply_remote_body_state(41, "anim_roll_left", f41, 0)
	_check(String(model.get_active_body_clip()) == "anim_roll_left",
			"roll_left accepted from idle_prone (active=%s)" % model.get_active_body_clip())
	# 41 is transition-locked (flags bit 0x4): 42 queues, then promotes at the
	# clip's completion boundary.
	model.apply_remote_body_state(42, "anim_roll_right", f42, 0)
	_check(String(model.get_active_body_clip()) == "anim_roll_left",
			"roll_right queues behind the locked roll_left")
	var len_l: float = skeletal.get_clip_length("anim_roll_left")
	_check(len_l > 0.0, "anim_roll_left has a resolvable length (%f s)" % len_l)
	model.advance_body_animation(len_l + 0.01)
	_check(String(model.get_active_body_clip()) == "anim_roll_right",
			"queued roll_right promotes at completion (active=%s)" % model.get_active_body_clip())

	if _fails == 0:
		print("[roll-probe] ALL PASS")
	quit(1 if _fails > 0 else 0)
