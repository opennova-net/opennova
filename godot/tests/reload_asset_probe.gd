extends SceneTree

# ADVERSARIAL VERIFICATION probe for the claim:
#   "infantry_anim_key(65/66) resolves to anim_reload/anim_reload2, US01.adm ships both
#    with their .bad files present, every remote player renders as US01, and the loader
#    registers every .adm key -- so the asset half is already in place."
#
# This re-derives the whole chain through the RUNTIME resource stack (PFF mounts), not
# the loose JOX extraction:
#   * NovaSimulation.infantry_anim_key(65) / (66)
#   * the wire remote-player type id 0x14B9 -> visual item -> graphic + anim_def
#   * NovaSkeletalAnim built from that anim_def: does it carry the two reload clips,
#     with real frame counts, and do they pose DIFFERENTLY from the hold poses?
#
# Run:
#   NOVA_RESOURCE_DIR="C:/.../Joint Operations Combined Arms" \
#   "$GODOT_BIN" --headless --path godot -s res://tests/reload_asset_probe.gd
# Not collected by GUT (*_probe.gd).

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

const PLAYER_RUNTIME_TYPE_ID := 0x14B9

var _fail := 0


func _initialize() -> void:
	call_deferred("_run")


func _frames(n: int) -> void:
	for i in n:
		await process_frame


func _max_rot_delta(a: Array, b: Array) -> float:
	var best := 0.0
	var n: int = mini(a.size(), b.size())
	for i in n:
		var x = a[i]
		var y = b[i]
		if x is Transform3D and y is Transform3D:
			var qa := Quaternion((x as Transform3D).basis.orthonormalized())
			var qb := Quaternion((y as Transform3D).basis.orthonormalized())
			best = maxf(best, rad_to_deg(qa.angle_to(qb)))
	return best


func _check(ok: bool, what: String) -> void:
	if not ok:
		_fail += 1
	print("%s  %s" % ["PASS" if ok else "FAIL", what])


func _run() -> void:
	var root_dir := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root_dir.is_empty():
		root_dir = OS.get_environment("OPENNOVA_JO_DIR").strip_edges()
	if root_dir.is_empty():
		root_dir = ResourceDirSettings.get_resource_dir()
	var res := NovaResourceRoot.new()
	if res.mount_runtime(root_dir, ResourceDirSettings.get_expansion()) != OK:
		print("FAIL: cannot mount ", root_dir)
		quit(1)
		return
	print("resource root: %s" % root_dir)

	# ---- 1. the key mapper -------------------------------------------------------
	print("")
	print("=== NovaSimulation.infantry_anim_key ===")
	for s in [43, 50, 51, 62, 63, 64, 65, 66]:
		print("  %3d -> '%s'  flags=0x%X" % [
				s, NovaSimulation.infantry_anim_key(s),
				NovaSimulation.infantry_anim_flags(s)])
	_check(String(NovaSimulation.infantry_anim_key(65)) == "anim_reload",
			"infantry_anim_key(65) == anim_reload")
	_check(String(NovaSimulation.infantry_anim_key(66)) == "anim_reload2",
			"infantry_anim_key(66) == anim_reload2")

	# ---- 2. the remote-player asset chain ----------------------------------------
	print("")
	print("=== remote player (wire type 0x14B9) asset chain ===")
	var placer = MissionObjectPlacer.new()
	placer.resource_root = res
	var mount := Node3D.new()
	root.add_child(mount)
	await _frames(2)

	var visual_item := int(placer.resolve_player_visual_item_id(PLAYER_RUNTIME_TYPE_ID))
	var graphic := String(placer.graphic_for(visual_item))
	var item_db = placer.item_db
	var anim_def := ""
	if item_db != null:
		anim_def = String(item_db.get_anim_def(visual_item))
	print("  0x%X -> visual item %d -> graphic '%s'  anim_def '%s'"
			% [PLAYER_RUNTIME_TYPE_ID, visual_item, graphic, anim_def])
	_check(graphic.to_lower() == "us01", "wire remote player graphic is US01")
	_check(anim_def.to_lower().begins_with("us01"), "wire remote player anim_def is US01")

	# ---- 3. the loaded clip set ---------------------------------------------------
	print("")
	print("=== NovaSkeletalAnim built from that anim_def ===")
	var od: Variant = placer.object_data_for(graphic)
	if od == null:
		print("FAIL: object data for '%s' did not load" % graphic)
		quit(1)
		return
	var adm_name := anim_def if anim_def.to_lower().ends_with(".adm") else anim_def + ".adm"
	var skel := NovaSkeletalAnim.new()
	var loaded: bool = skel.load_from_resource_root(
			res, adm_name, od.get_bone_origins(), od.get_bone_parents())
	print("  load_from_resource_root('%s') = %s   err='%s'"
			% [adm_name, str(loaded), String(skel.get_last_error())])
	if not loaded:
		quit(1)
		return
	var keys: PackedStringArray = skel.get_clip_keys()
	print("  registered clip keys = %d   bones = %d" % [keys.size(), skel.get_bone_count()])

	for key in ["anim_reload", "anim_reload2", "anim_knife", "anim_pistol",
			"anim_binoculars", "anim_idle"]:
		var has: bool = skel.has_clip(key)
		var frames := int(skel.get_clip_frame_count(key, 0)) if has else -1
		var fps: float = skel.get_clip_fps(key, 0) if has else -1.0
		var variants := int(skel.get_clip_variant_count(key)) if has else 0
		print("  %-16s has=%s frames=%d fps=%.2f variants=%d"
				% [key, str(has), frames, fps, variants])

	_check(skel.has_clip("anim_reload"), "US01 clip set carries anim_reload")
	_check(skel.has_clip("anim_reload2"), "US01 clip set carries anim_reload2")
	_check(int(skel.get_clip_frame_count("anim_reload", 0)) > 1,
			"anim_reload has more than one frame")
	_check(int(skel.get_clip_frame_count("anim_reload2", 0)) > 1,
			"anim_reload2 has more than one frame")

	# ---- 4. do the reload clips actually MOVE relative to the hold pose? -----------
	print("")
	print("=== pose deltas (max per-bone rotation delta, degrees) ===")
	var idle_pose: Array = skel.eval_pose("anim_pistol", 0.0, 0)
	for key in ["anim_reload", "anim_reload2"]:
		if not skel.has_clip(key):
			continue
		var len_s: float = skel.get_clip_length(key, 0)
		var start: Array = skel.eval_pose(key, 0.0, 0)
		var mid: Array = skel.eval_pose(key, len_s * 0.5, 0)
		print("  %-16s length=%.3fs  vs anim_pistol@0 = %.2f deg   own t0 vs t_mid = %.2f deg"
				% [key, len_s, _max_rot_delta(idle_pose, mid), _max_rot_delta(start, mid)])
		_check(_max_rot_delta(idle_pose, mid) > 0.01,
				"%s poses differently from the hold pose" % key)
		_check(_max_rot_delta(start, mid) > 0.01, "%s actually animates over its length" % key)

	# ---- 5. does the weapon-channel splice actually consume the reload clip? -------
	print("")
	print("=== splice_weapon_channel with the reload key (identity aim overlay) ===")
	var classes: PackedInt32Array = skel.get_overlay_classes()
	var deltas: Array = []
	for i in 8:
		deltas.append(Basis())
	var no_wpn: Array = skel.eval_pose_overlay(
			"anim_pistol", 0.0, classes, deltas, "", 0.0, false)
	for key in ["anim_reload", "anim_reload2"]:
		var with_wpn: Array = skel.eval_pose_overlay(
				"anim_pistol", 0.0, classes, deltas,
				key, float(skel.get_clip_length(key, 0)) * 0.5, false)
		var d := _max_rot_delta(no_wpn, with_wpn)
		print("  wpn_key='%s' -> max masked-bone rotation change = %.2f deg" % [key, d])
		_check(d > 0.01, "the splice consumes %s" % key)

	print("")
	print("RESULT: %s (%d failing checks)" % ["ALL PASS" if _fail == 0 else "FAILURES", _fail])
	quit(0 if _fail == 0 else 1)
