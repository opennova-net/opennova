extends SceneTree

# ADVERSARIAL probe for D-WPN-32.
#
# Tests exactly one claim: "the AABB probe measured +Z-long at BUILD time only, before
# NovaObjectModel self-ticks; the LIVE weapon node's long axis at DRAW time may differ."
#
# For each firearm gfx3 it builds the model exactly as the wire present pass's
# held-weapon update
# does, measures the combined mesh AABB in the model root's own frame, reports whether the
# model has LIVE PANM at all, lets 30 real frames elapse (so _process/_apply_runtime_state
# actually run), and re-measures. Then it drives the measured live attach triple through
# PresentHeldWeapon's basis and prints the elevation of the model's own longest axis after
# the basis is applied -- i.e. what the viewer actually sees.
#
# Run:
#   NOVA_RESOURCE_DIR=... "$GODOT_BIN" --headless --path godot -s res://tests/held_weapon_livetick_probe.gd

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

const ATTACH := Vector3(17.95, 13.57, 0.0)  # the measured LOCAL live attach triple


func _initialize() -> void:
	_run()


func _run() -> void:
	var root_dir := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root_dir.is_empty():
		root_dir = ResourceDirSettings.get_resource_dir()
	var exp := OS.get_environment("NOVA_EXPANSION").strip_edges()
	if exp == "-":
		exp = ""
	elif exp.is_empty():
		exp = ResourceDirSettings.get_expansion()
	print("resource root: %s   expansion: %s" % [root_dir, exp])

	var res := NovaResourceRoot.new()
	if res.mount_runtime(root_dir, exp) != OK:
		print("FAIL: cannot mount ", root_dir)
		quit(1)
		return

	var db := NovaWeaponDatabase.new()
	if db.load_from_resource_root(res, "weapon.def") != OK:
		print("FAIL: weapon.def -> ", db.get_last_error())
		quit(1)
		return

	var placer := MissionObjectPlacer.new()
	placer.resource_root = res
	var mount := Node3D.new()
	get_root().add_child(mount)

	var picked: Array = []
	var seen := {}
	for i in range(db.get_count()):
		var w: Dictionary = db.get_weapon(i)
		var gfx3: String = String(w.get("gfx3", ""))
		if gfx3.is_empty() or seen.has(gfx3.to_lower()):
			continue
		seen[gfx3.to_lower()] = true
		var nm: String = String(w.get("name", "?")).to_upper()
		if nm.contains("M4") or nm.contains("M16") or nm.contains("M60") \
				or nm.contains("COLT") or nm.contains("MP5") or nm.contains("AK47") \
				or nm.contains("KNIFE") or nm.contains("SR25"):
			picked.append([String(w.get("name", "?")), gfx3])

	var models: Array = []
	for row in picked:
		var m: Node3D = placer.build_model_from_graphic(String(row[1]), "", mount, "", null)
		if m == null:
			print("  %-18s gfx3=%-14s <no model built>" % [row[0], row[1]])
			continue
		var live := false
		var od = m.get_object_data() if m.has_method("get_object_data") else null
		if od != null and od.has_method("has_live_panm_for_lod"):
			live = bool(od.has_live_panm_for_lod(0))
		models.append([row[0], row[1], m, _axis_of(m), live])

	# Let real frames run so every _process()/_apply_runtime_state() actually fires.
	for _i in range(30):
		await process_frame

	print("name              gfx3            live_panm  BUILD size / axis          AFTER-30-FRAMES size / axis   drawn-long-axis elev")
	for e in models:
		var m: Node3D = e[2]
		var before: Variant = e[3]
		var after: Variant = _axis_of(m)
		var b_txt := _fmt(before)
		var a_txt := _fmt(after)
		var elev := "n/a"
		if after != null:
			var s: Vector3 = (after as AABB).size
			var axis := Vector3(1, 0, 0) if (s.x >= s.y and s.x >= s.z) else (Vector3(0, 1, 0) if s.y >= s.z else Vector3(0, 0, 1))
			var world: Vector3 = MissionObjectPlacer.bms_to_godot_basis(ATTACH) * axis
			elev = "%+.2f deg" % rad_to_deg(asin(clampf(world.y, -1.0, 1.0)))
		print("  %-16s %-14s %-9s %-27s %-29s %s" % [e[0], e[1], str(e[4]), b_txt, a_txt, elev])

	# Reference: what the basis does to each unit axis, for the record.
	var basis: Basis = MissionObjectPlacer.bms_to_godot_basis(ATTACH)
	print("bms_to_godot_basis(p=%.2f,y=%.2f,r=%.2f): +Z->%s elev %+.2f | +Y->%s elev %+.2f | +X->%s elev %+.2f" % [
			ATTACH.x, ATTACH.y, ATTACH.z,
			_v(basis * Vector3(0, 0, 1)), rad_to_deg(asin((basis * Vector3(0, 0, 1)).y)),
			_v(basis * Vector3(0, 1, 0)), rad_to_deg(asin((basis * Vector3(0, 1, 0)).y)),
			_v(basis * Vector3(1, 0, 0)), rad_to_deg(asin((basis * Vector3(1, 0, 0)).y))])
	quit(0)


func _axis_of(model: Node3D) -> Variant:
	return _combined_aabb(model, model.global_transform.affine_inverse())


func _fmt(box: Variant) -> String:
	if box == null:
		return "<no meshes>"
	var b: AABB = box
	var s: Vector3 = b.size
	var axis: String = "X" if (s.x >= s.y and s.x >= s.z) else ("Y" if s.y >= s.z else "Z")
	return "(%.3f,%.3f,%.3f) %s" % [s.x, s.y, s.z, axis]


func _combined_aabb(node: Node, to_local: Transform3D) -> Variant:
	var out: Variant = null
	if node is MeshInstance3D:
		var mi := node as MeshInstance3D
		if mi.mesh != null:
			out = ((to_local * mi.global_transform) * mi.mesh.get_aabb()) as AABB
	for child in node.get_children():
		var sub: Variant = _combined_aabb(child, to_local)
		if sub == null:
			continue
		out = sub if out == null else (out as AABB).merge(sub as AABB)
	return out


func _v(v: Vector3) -> String:
	return "(%+.3f,%+.3f,%+.3f)" % [v.x, v.y, v.z]
