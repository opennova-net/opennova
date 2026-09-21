class_name StaticSourceFixture
extends RefCounted


## Place native static sources through the real item catalog and retained mesh
## registration seam. The caller owns the parent; the parsed catalog needs no
## scratch file after loading.
static func place(test: GutTest, parent: Node, models: Array[ObjectData],
		positions: Array[Vector3], sections: PackedInt32Array = PackedInt32Array([0]),
		scale: float = 1.0, offsets: Array[Vector3] = []) -> MissionObjectPlacer:
	var text := ""
	for i in models.size():
		text += "begin \"Source %d\"\nid %d\ntype building\ngraphic Source%d\nscale %f\nend\n" % [
			i, 105001 + i, i, scale]
	var path := OS.get_cache_dir().path_join("static_source_fixture_%d.def" % Time.get_ticks_usec())
	TestFs.write_text(test, path, text)
	var db := ItemDatabase.new()
	test.assert_eq(db.load(path), OK)
	DirAccess.remove_absolute(path)
	var resources := ResourceRoot.new()
	test.assert_eq(resources.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/threedi/synth")), OK)
	var placer := MissionObjectPlacer.create(resources, db)
	var mission := MissionData.new()
	test.assert_eq(mission.create_default(), OK)
	for i in models.size():
		var batches: Array[Dictionary] = []
		for row in sections.size():
			batches.append({"mesh": BoxMesh.new(), "material": null,
				"offset": Transform3D(Basis.IDENTITY, offsets[row] if row < offsets.size() else Vector3.ZERO),
				"submesh": row, "robj_index": sections[row]})
		test.assert_true(placer.register_resolved_static_graphic("Source%d" % i, models[i], batches))
		placer.register_occlusion_verdict(105001 + i, false)
		test.assert_not_null(mission.add_entity(MissionData.KIND_BUILDING, 105001 + i,
				MissionObjectPlacer.godot_to_bms_position(positions[i]), Vector3(0, 180, 0)))
	var branch := Node3D.new()
	parent.add_child(branch)
	test.assert_eq(placer.place(mission, branch).batched, models.size())
	return placer
