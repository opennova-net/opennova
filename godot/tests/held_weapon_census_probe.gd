extends SceneTree

# Census of EVERY third-person weapon model's authored long axis (D-WPN-32).
#
# The held weapon is drawn rigid at the entity attach basis, which sends the model's +Z down
# the soldier's line of fire and the model's +Y straight up. So a gfx3 authored +Z-long reads
# as a held rifle, and a gfx3 authored +Y-long reads as a near-VERTICAL object no matter how
# correct the basis is. This walks all 94 weapon.def rows and tallies which is which, so
# "the gun looks vertical" can be attributed to the orientation or to the weapon IDENTITY
# rather than guessed at.
#
# Run:
#   "$GODOT_BIN" --headless --path godot -s res://tests/held_weapon_census_probe.gd
# Not collected by GUT (*_probe.gd).

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")


func _init() -> void:
	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
	var res := NovaResourceRoot.new()
	if res.mount_runtime(root, ResourceDirSettings.get_expansion()) != OK:
		print("FAIL: cannot mount ", root)
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

	print("%-5s %-18s %-16s %-24s %-24s %s" % [
			"row", "weapon", "gfx3", "size", "centre", "LONG"])
	var seen := {}
	var tally := {"X": 0, "Y": 0, "Z": 0}
	var vertical: Array[String] = []
	for i in range(db.get_count()):
		var w: Dictionary = db.get_weapon(i)
		var gfx3: String = String(w.get("gfx3", ""))
		var nm: String = String(w.get("name", "?"))
		if gfx3.is_empty():
			print("%-5d %-18s %-16s  <no gfx3 authored>" % [i + 1, nm, "-"])
			continue
		var key := gfx3.to_lower()
		if seen.has(key):
			print("%-5d %-18s %-16s  (same model as row %d)" % [i + 1, nm, gfx3, seen[key]])
			continue
		seen[key] = i + 1
		var model: Node3D = placer.build_model_from_graphic(gfx3, "", mount, "", null)
		if model == null:
			print("%-5d %-18s %-16s  <model did not build>" % [i + 1, nm, gfx3])
			continue
		var box: Variant = _combined_aabb(model, model.global_transform.affine_inverse())
		if box == null:
			print("%-5d %-18s %-16s  <no meshes>" % [i + 1, nm, gfx3])
			model.queue_free()
			continue
		var b: AABB = box
		var s: Vector3 = b.size
		var axis := "X" if (s.x >= s.y and s.x >= s.z) else ("Y" if s.y >= s.z else "Z")
		tally[axis] = int(tally[axis]) + 1
		if axis == "Y":
			vertical.append("%s (%s)" % [nm, gfx3])
		print("%-5d %-18s %-16s %-24s %-24s %s" % [
				i + 1, nm, gfx3, _v(s), _v(b.get_center()), axis])
		model.queue_free()

	print("")
	print("distinct models: %d   long axis X=%d  Y=%d  Z=%d" % [
			seen.size(), tally["X"], tally["Y"], tally["Z"]])
	print("Y-LONG (these draw NEAR-VERTICAL under a correct attach basis): %s" % [
			", ".join(vertical) if not vertical.is_empty() else "<none>"])
	quit(0)


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
	return "(%+.3f, %+.3f, %+.3f)" % [v.x, v.y, v.z]
