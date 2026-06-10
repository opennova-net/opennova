extends GutTest

# Binding-layer smoke over a directory of real shipped .bms missions. Opens every file through
# NovaMissionData -- the GDExtension surface the editor actually drives -- and validates that the
# editor-facing data is sane: pool counts agree with get_entities, waypoint summaries are clamped to
# the 32-slot region (CP19.bms ships a path with a raw marker_count of 39), full waypoint paths never
# exceed 32 markers, and the read accessors (area triggers, events, all-entities) don't crash. One
# mission also round-trips through save_as -> open_file to exercise the binding's write path on real
# data. This complements tests/mission/mission_corpus_test.cpp (the C++ parse/write proof) at the
# Godot layer.
#
# Gated on OPENNOVA_MISSION_CORPUS (point it at a dir of real .bms files, e.g. an extracted JO_ASSETS
# dir). The assets are copyrighted and not committed, so the test marks itself pending + passes when
# the env var is unset -- it never runs bare in CI.

const MAX_WAYPOINT_SLOTS := 32


func _corpus_dir() -> String:
	return OS.get_environment("OPENNOVA_MISSION_CORPUS").strip_edges()


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
		pending("set OPENNOVA_MISSION_CORPUS to a dir of real .bms files to run")
		return
	assert_true(DirAccess.dir_exists_absolute(dir), "corpus dir exists: %s" % dir)

	var files := _list_bms(dir)
	assert_gt(files.size(), 0, "corpus dir has .bms files")

	var kinds := [
		NovaMissionData.KIND_MARKER, NovaMissionData.KIND_ITEM,
		NovaMissionData.KIND_BUILDING, NovaMissionData.KIND_ORGANIC,
	]
	var problems := PackedStringArray()
	var checked := 0
	for path in files:
		var name := path.get_file()
		var m := NovaMissionData.new()
		if m.open_file(path) != OK or not m.is_loaded():
			problems.append("%s: failed to open through the binding" % name)
			continue

		for kind in kinds:
			if m.get_entity_count(kind) != m.get_entities(kind).size():
				problems.append("%s: kind %d count != get_entities size" % [name, kind])

		# Waypoint summaries are clamped to the editable 32-slot region (the CP19 over-count fix).
		for s in m.get_waypoint_summaries():
			var mc := int((s as Dictionary)["marker_count"])
			if mc < 0 or mc > MAX_WAYPOINT_SLOTS:
				problems.append("%s: waypoint marker_count %d out of [0,%d]" % [name, mc, MAX_WAYPOINT_SLOTS])
			# A full path fetch must never hand the editor more markers than there are slots.
			var p: Dictionary = m.get_waypoint_path(int((s as Dictionary)["index"]))
			var indices: PackedInt32Array = p.get("marker_indices", PackedInt32Array())
			if indices.size() > MAX_WAYPOINT_SLOTS:
				problems.append("%s: path %d has %d marker_indices" % [name, int(s["index"]), indices.size()])

		# Read accessors the inspector calls must not crash on any real mission.
		m.get_area_triggers()
		m.get_events()
		m.get_all_entities()
		checked += 1

	for p in problems:
		fail_test(p)
	assert_eq(problems.size(), 0, "every real mission is sane through the binding")
	gut.p("binding smoke: %d/%d real missions opened and validated" % [checked, files.size()])


func test_cp19_over_count_waypoint_loads_and_is_clamped() -> void:
	# CP19.bms ships a waypoint record whose raw marker_count (39) exceeds the 32-slot region. The
	# engine reads the fixed 136-byte record and never validates the count; our parser preserves it on
	# disk but the editor must see a clamped, in-range count. This is the regression guard for the
	# bug the corpus round-trip surfaced.
	var dir := _corpus_dir()
	if dir.is_empty():
		pending("set OPENNOVA_MISSION_CORPUS to the dir containing CP19.bms to run")
		return
	var path := dir.path_join("CP19.bms")
	if not FileAccess.file_exists(path):
		pending("CP19.bms not present in the corpus dir")
		return
	var m := NovaMissionData.new()
	assert_eq(m.open_file(path), OK, "CP19.bms opens through the binding (no rejection)")
	assert_true(m.is_loaded(), "CP19.bms is loaded")
	for s in m.get_waypoint_summaries():
		assert_lte(int((s as Dictionary)["marker_count"]), MAX_WAYPOINT_SLOTS,
			"every CP19 waypoint summary is clamped to the 32-slot region")


func test_real_mission_round_trips_through_the_binding_save_path() -> void:
	var dir := _corpus_dir()
	if dir.is_empty():
		pending("set OPENNOVA_MISSION_CORPUS to a dir of real .bms files to run")
		return
	var files := _list_bms(dir)
	if files.is_empty():
		pending("no .bms files in the corpus dir")
		return

	var src := files[0]
	var m := NovaMissionData.new()
	assert_eq(m.open_file(src), OK, "source mission opens")

	var out_path := "user://corpus_binding_roundtrip.bms"
	assert_eq(m.save_as(ProjectSettings.globalize_path(out_path)), OK, "save_as writes through the binding")

	var reopened := NovaMissionData.new()
	assert_eq(reopened.open_file(ProjectSettings.globalize_path(out_path)), OK, "the written mission reopens")
	assert_eq(reopened.get_terrain_ref(), m.get_terrain_ref(), "terrain ref survives the binding round-trip")
	for kind in [NovaMissionData.KIND_MARKER, NovaMissionData.KIND_ITEM, NovaMissionData.KIND_BUILDING, NovaMissionData.KIND_ORGANIC]:
		assert_eq(reopened.get_entity_count(kind), m.get_entity_count(kind),
			"kind %d count survives the binding round-trip" % kind)

	DirAccess.remove_absolute(ProjectSettings.globalize_path(out_path))
