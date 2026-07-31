extends SceneTree

# BUG B measurement probe: WHERE does a fire effect actually spawn, and how far is
# that from the real muzzle?
#
# Reports, all in the character model's OWN frame (the body node sits at identity):
#   * every userpoint authored on the player character model (0x14B9 -> its visual item),
#   * which userpoint the model's muzzle resolver picks, and on which bone,
#   * NovaObjectModel.get_muzzle_world_position() (what the mission presenter
#     feeds NovaSimulation.set_ai_muzzle_world),
#   * the HEAD bone (LocalPlayerPresenter.PLAYER_HEAD_BONE_INDEX = 14) origin — the local
#     player's EYE anchor, and the origin we put on the wire for our own shots,
#   * bone 16 "BN17 R Hand" — the held-weapon joint,
#   * the 0.9 u chest-lift fallback (pos.z + 0xE666) the AI fire path uses when no
#     binding-pushed muzzle is fresh,
#   * the M4_3RD gfx3's MFLASH01 userpoint carried through
#     PresentHeldWeapon.attach_transform (both attach frames) — the REAL muzzle of the
#     gun we actually draw in the hand.
#
# Measured in REST pose and again in a posed standing clip, because the rest pose of
# US01 has the arms out to the sides.
#
# Run:
#   NOVA_RESOURCE_DIR="C:/.../Joint Operations Combined Arms" \
#   "$GODOT_BIN" --headless --path godot -s res://tests/muzzle_origin_probe.gd
# Not collected by GUT (*_probe.gd).

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const PresentHeldWeapon := preload("res://engine/world/present_held_weapon.gd")

const PLAYER_RUNTIME_TYPE_ID := 0x14B9
const HEAD_BONE_INDEX := 14      # LocalPlayerPresenter.PLAYER_HEAD_BONE_INDEX
const HAND_BONE_INDEX := 16      # PresentHeldWeapon.BONE_INDEX
const FALLBACK_LIFT := 0.9       # 0xE666 in 16.16 — infantry.cpp:1849 / ai.cpp:1194

var _placer
var _presenter: Node3D
var _body: Node3D
var _skel: Skeleton3D
var _body_graphic := ""
var _pushed_muzzle := Vector3.INF
var _head := Vector3.INF
var _flash_local := Vector3.INF


func _initialize() -> void:
	call_deferred("_run")


func _frames(n: int) -> void:
	for i in n:
		await process_frame


func _run() -> void:
	var root_dir := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root_dir.is_empty():
		root_dir = ResourceDirSettings.get_resource_dir()
	var res := NovaResourceRoot.new()
	if res.mount_runtime(root_dir, ResourceDirSettings.get_expansion()) != OK:
		print("FAIL: cannot mount ", root_dir)
		quit(1)
		return
	print("resource root: %s" % root_dir)

	_placer = MissionObjectPlacer.new()
	_placer.resource_root = res
	_presenter = Node3D.new()
	root.add_child(_presenter)
	await _frames(2)

	var visual_item := int(_placer.resolve_player_visual_item_id(PLAYER_RUNTIME_TYPE_ID))
	_body_graphic = String(_placer.graphic_for(visual_item))
	print("player runtime type 0x%X -> visual item %d -> graphic '%s'"
			% [PLAYER_RUNTIME_TYPE_ID, visual_item, _body_graphic])

	_body = _placer.build_player_animated_model(PLAYER_RUNTIME_TYPE_ID, _presenter, null)
	if _body == null:
		print("FAIL: the player character model did not build")
		quit(1)
		return
	await _frames(2)
	_skel = PresentHeldWeapon.find_skeleton(_body)
	if _skel == null:
		print("FAIL: no Skeleton3D under the character model")
		quit(1)
		return
	print("body node global origin = %s   skeleton global origin = %s   bones = %d"
			% [_v(_body.global_transform.origin), _v(_skel.global_transform.origin),
					_skel.get_bone_count()])

	# ---- 1. every userpoint on the CHARACTER model -------------------------------
	print("")
	print("=== character model userpoints (%s) ===" % _body_graphic)
	var od: Variant = _placer.object_data_for(_body_graphic)
	var up_count := 0
	if od != null and od.has_method("get_user_point_count"):
		up_count = int(od.get_user_point_count())
	print("%-4s %-18s %-26s %-10s %s" % ["idx", "name", "model position", "subobject", "bone name"])
	for i in up_count:
		var info: Dictionary = od.get_user_point_info(i)
		var sub := int(info.get("subobject", -1))
		print("%-4d %-18s %-26s %-10d %s" % [
				i, String(info.get("name", "?")),
				_v(info.get("position", Vector3.ZERO)), sub, _bone_name(sub)])
	if up_count == 0:
		print("(none authored)")

	# ---- 2. what _resolve_muzzle_userpoint picked --------------------------------
	print("")
	print("=== _resolve_muzzle_userpoint result ===")
	var muzzle_bone := int(_body.get("_muzzle_bone"))
	print("has_muzzle()          = %s" % str(_body.has_muzzle()))
	print("_muzzle_bone          = %d  (%s)" % [muzzle_bone, _bone_name(muzzle_bone)])
	print("_muzzle_model_pos     = %s" % _v(_body.get("_muzzle_model_pos")))

	# ---- 3. the held weapon's userpoints -----------------------------------------
	print("")
	print("=== held weapon M4_3RD userpoints ===")
	var weapon: Node3D = _placer.build_model_from_graphic("M4_3RD", "", _presenter, "", null)
	if weapon == null:
		print("FAIL: M4_3RD did not build")
		quit(1)
		return
	var wod: Variant = _placer.object_data_for("M4_3RD")
	var w_count := 0
	if wod != null and wod.has_method("get_user_point_count"):
		w_count = int(wod.get_user_point_count())
	print("%-4s %-18s %-26s %s" % ["idx", "name", "model position", "subobject"])
	for i in w_count:
		var info: Dictionary = wod.get_user_point_info(i)
		var nm := String(info.get("name", "?"))
		print("%-4d %-18s %-26s %d" % [i, nm, _v(info.get("position", Vector3.ZERO)),
				int(info.get("subobject", -1))])
		if _flash_local == Vector3.INF and nm.to_lower().contains("flash"):
			_flash_local = info.get("position", Vector3.ZERO)
	if _flash_local == Vector3.INF:
		print("NOTE: no *flash* userpoint on M4_3RD; using the authored constant")
		_flash_local = Vector3(0.0027, 0.0891, 0.7810)

	# ---- 4. available body clips --------------------------------------------------
	var skeletal = _body.get_skeletal_anim() if _body.has_method("get_skeletal_anim") else null
	var keys := PackedStringArray()
	if skeletal != null and skeletal.has_method("get_clip_keys"):
		keys = skeletal.get_clip_keys()
	print("")
	print("=== body clips (%d) ===" % keys.size())
	print(", ".join(keys))

	# ---- 5. measure in rest pose, then in each interesting pose --------------------
	await _measure("REST POSE (no clip), body yaw 0", "", "", 0.0)
	for probe_case in [
			["anim_idle", "", 0.0],
			["anim_idle", "", 90.0],
			["anim_idle", "anim_reload", 0.0],
			["anim_idle_crouch", "", 0.0],
			["anim_walk_forward", "", 0.0]]:
		var body_key := String(probe_case[0])
		var wpn_key := String(probe_case[1])
		var yaw := float(probe_case[2])
		if not keys.has(body_key):
			continue
		if not wpn_key.is_empty() and not keys.has(wpn_key):
			continue
		await _measure("body='%s' weapon='%s' entity yaw=%.0f" % [body_key, wpn_key, yaw],
				body_key, wpn_key, yaw)

	print("")
	print("=== fire-origin candidates vs the real muzzle: see per-pose blocks above ===")
	quit(0)


func _measure(label: String, body_key: String, weapon_key: String, yaw_deg: float) -> void:
	# Body and weapon must share ONE frame convention: the present pass gives the body
	# node bms_to_godot_basis(entity rot) and the entity-frame weapon
	# bms_to_godot_basis(attach angles). Drive both from the same triple here.
	var angles := Vector3(0.0, yaw_deg, 0.0)
	_body.transform = Transform3D(MissionObjectPlacer.bms_to_godot_basis(angles), Vector3.ZERO)
	if not body_key.is_empty():
		_body.play_body_clip(body_key)
	if not weapon_key.is_empty() and _body.has_method("set_weapon_channel"):
		_body.set_weapon_channel(weapon_key, 0)
	if _body.has_method("set_animation_time"):
		_body.set_animation_time(0.0)
	await _frames(3)

	var to_local := _body.global_transform.affine_inverse()
	print("")
	print("############ %s ############" % label)
	_head = _bone_world(HEAD_BONE_INDEX)
	var hand := _bone_world(HAND_BONE_INDEX)
	print("HEAD bone %d '%s'  = %s  body-local %s   <-- eye / wire fire origin"
			% [HEAD_BONE_INDEX, _bone_name(HEAD_BONE_INDEX), _v(_head), _v(to_local * _head)])
	print("HAND bone %d '%s'  = %s  body-local %s   <-- held-weapon joint"
			% [HAND_BONE_INDEX, _bone_name(HAND_BONE_INDEX), _v(hand), _v(to_local * hand)])
	print("0.9 u chest-lift fallback  = %s   <-- infantry.cpp:1849 / ai.cpp:1194"
			% _v(Vector3(0.0, FALLBACK_LIFT, 0.0)))
	if _body.has_muzzle():
		_pushed_muzzle = _body.get_muzzle_world_position()
		print("body userpoint muzzle      = %s  body-local %s   <-- _push_muzzle seam"
				% [_v(_pushed_muzzle), _v(to_local * _pushed_muzzle)])

	for frame_case in [
			{"name": "ENTITY frame (attach triple = entity angles)", "hand": false},
			{"name": "HAND frame (0x80 hold states)", "hand": true}]:
		var attach: Variant = PresentHeldWeapon.attach_transform(
				_body, angles, bool(frame_case["hand"]))
		if attach == null:
			print("  %-44s <attach_transform null>" % frame_case["name"])
			continue
		var xf: Transform3D = attach
		var muzzle_world: Vector3 = xf * _flash_local
		print("  --- %s ---" % frame_case["name"])
		print("  grip (attach origin)     = %s  body-local %s"
				% [_v(xf.origin), _v(to_local * xf.origin)])
		print("  REAL MUZZLE (MFLASH01)   = %s  body-local %s"
				% [_v(muzzle_world), _v(to_local * muzzle_world)])
		_gap("  effect at HEAD/eye", _head, muzzle_world, to_local)
		_gap("  effect at 0.9u fallback", Vector3(0.0, FALLBACK_LIFT, 0.0),
				muzzle_world, to_local)
		if _pushed_muzzle != Vector3.INF:
			_gap("  effect at body-userpoint muzzle", _pushed_muzzle, muzzle_world, to_local)


func _gap(label: String, spawn: Vector3, muzzle: Vector3, to_local: Transform3D) -> void:
	var d := spawn - muzzle
	print("%-36s |gap|=%.3f u   gap(body-local)=%s"
			% [label, d.length(), _v(to_local.basis * d)])


func _bone_world(index: int) -> Vector3:
	if _skel == null or index < 0 or index >= _skel.get_bone_count():
		return Vector3.INF
	return (_skel.global_transform * _skel.get_bone_global_pose(index)).origin


func _bone_name(index: int) -> String:
	if _skel == null or index < 0 or index >= _skel.get_bone_count():
		return "<absent>"
	return _skel.get_bone_name(index)


func _v(v: Vector3) -> String:
	if v == Vector3.INF:
		return "<none>"
	return "(%+.3f, %+.3f, %+.3f)" % [v.x, v.y, v.z]
