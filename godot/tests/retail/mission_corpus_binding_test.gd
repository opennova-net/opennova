extends GutTest

# Exercise real file I/O through MissionData over the shipped BMS corpus.
# Native mission_corpus covers parsing, writer fidelity and waypoint clamping;
# this suite checks document loading and saving through Godot paths.


func _corpus_dir() -> String:
	return RetailData.assets()


func _list_bms(dir_path: String) -> PackedStringArray:
	var out := PackedStringArray()
	var d := DirAccess.open(dir_path)
	if d == null:
		return out
	for f in d.get_files():
		if f.to_lower().ends_with(".bms"):
			out.append(dir_path.path_join(f))
	out.sort()
	return out


func test_real_missions_open_and_are_sane_through_the_binding() -> void:
	var dir := _corpus_dir()
	if dir.is_empty():
		pending("set OPENNOVA_JO_ASSETS to the extracted tree of real .bms files to run")
		return
	assert_true(DirAccess.dir_exists_absolute(dir), "corpus dir exists: %s" % dir)

	var files := _list_bms(dir)
	assert_gt(files.size(), 0, "corpus dir has .bms files")

	var kinds := [
		MissionData.KIND_MARKER, MissionData.KIND_ITEM,
		MissionData.KIND_BUILDING, MissionData.KIND_ORGANIC,
	]
	var problems := PackedStringArray()
	var checked := 0
	for path in files:
		var name := path.get_file()
		var m := MissionData.new()
		if m.open_file(path) != OK or not m.is_loaded():
			problems.append("%s: failed to open through the binding" % name)
			continue

		for kind in kinds:
			if m.get_entity_count(kind) != m.get_entity_refs(kind).size():
				problems.append("%s: kind %d count != get_entity_refs size" % [name, kind])

		m.get_all_entity_refs()
		checked += 1

	for p in problems:
		fail_test(p)
	assert_eq(problems.size(), 0, "every real mission is sane through the binding")
	gut.p("binding smoke: %d/%d real missions opened and validated" % [checked, files.size()])


func test_real_mission_round_trips_through_the_binding_save_path() -> void:
	var dir := _corpus_dir()
	if dir.is_empty():
		pending("set OPENNOVA_JO_ASSETS to the extracted tree of real .bms files to run")
		return
	var files := _list_bms(dir)
	if files.is_empty():
		pending("OPENNOVA_JO_ASSETS: no .bms files in the tree")
		return

	var src := files[0]
	var m := MissionData.new()
	assert_eq(m.open_file(src), OK, "source mission opens")

	var out_path := "user://corpus_binding_roundtrip.bms"
	assert_eq(m.save_as(ProjectSettings.globalize_path(out_path)), OK, "save_as writes through the binding")

	var reopened := MissionData.new()
	assert_eq(reopened.open_file(ProjectSettings.globalize_path(out_path)), OK, "the written mission reopens")
	assert_eq(reopened.get_terrain_ref(), m.get_terrain_ref(), "terrain ref survives the binding round-trip")
	for kind in [MissionData.KIND_MARKER, MissionData.KIND_ITEM, MissionData.KIND_BUILDING, MissionData.KIND_ORGANIC]:
		assert_eq(reopened.get_entity_count(kind), m.get_entity_count(kind),
			"kind %d count survives the binding round-trip" % kind)

	DirAccess.remove_absolute(ProjectSettings.globalize_path(out_path))
