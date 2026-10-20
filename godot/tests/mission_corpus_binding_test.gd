extends GutTest

# Exercise real file I/O through MissionData over committed missions: every
# minted mission under fixtures/bms (minimal_bms_gen) and our base game's
# assets/onjo_m1.bms. The native mission tests cover parsing, writer fidelity,
# waypoint clamping and the shipped variants; this suite checks document
# loading and saving through Godot paths.

const FIXTURE_DIR := "res://../fixtures/bms"
const BASE_GAME_MISSION := "res://../assets/onjo_m1.bms"
const KINDS := [
	MissionData.KIND_MARKER, MissionData.KIND_ITEM,
	MissionData.KIND_BUILDING, MissionData.KIND_ORGANIC,
]


func _missions() -> PackedStringArray:
	var out := PackedStringArray()
	var dir := ProjectSettings.globalize_path(FIXTURE_DIR)
	for f in DirAccess.get_files_at(dir):
		if f.to_lower().ends_with(".bms"):
			out.append(dir.path_join(f))
	out.sort()
	out.append(ProjectSettings.globalize_path(BASE_GAME_MISSION))
	return out


func test_committed_missions_open_and_are_sane_through_the_binding() -> void:
	var files := _missions()
	assert_gt(files.size(), 1, "the fixture set and the base game carry missions")
	var problems := PackedStringArray()
	var checked := 0
	for path in files:
		var name := path.get_file()
		var m := MissionData.new()
		if m.open_file(path) != OK or not m.is_loaded():
			problems.append("%s: failed to open through the binding" % name)
			continue
		var total := 0
		for kind in KINDS:
			if m.get_entity_count(kind) != m.get_entity_refs(kind).size():
				problems.append("%s: kind %d count != get_entity_refs size" % [name, kind])
			total += m.get_entity_count(kind)
		if m.get_all_entity_refs().size() != total:
			problems.append("%s: get_all_entity_refs size != the kinds' sum" % name)
		checked += 1

	for p in problems:
		fail_test(p)
	assert_eq(problems.size(), 0, "every committed mission is sane through the binding")
	assert_eq(checked, files.size(), "every committed mission opened")
	gut.p("binding smoke: %d/%d committed missions opened and validated" % [checked, files.size()])


func test_mission_round_trips_through_the_binding_save_path() -> void:
	var src := ProjectSettings.globalize_path(FIXTURE_DIR.path_join("synth_dense.bms"))
	var m := MissionData.new()
	assert_eq(m.open_file(src), OK, "source mission opens")
	var populated := 0
	for kind in KINDS:
		if m.get_entity_count(kind) > 0:
			populated += 1
	assert_gt(populated, 1, "the dense mission places entities of several kinds")

	var out_path := "user://corpus_binding_roundtrip.bms"
	assert_eq(m.save_as(ProjectSettings.globalize_path(out_path)), OK, "save_as writes through the binding")

	var reopened := MissionData.new()
	assert_eq(reopened.open_file(ProjectSettings.globalize_path(out_path)), OK, "the written mission reopens")
	assert_eq(reopened.get_terrain_ref(), m.get_terrain_ref(), "terrain ref survives the binding round-trip")
	for kind in KINDS:
		assert_eq(reopened.get_entity_count(kind), m.get_entity_count(kind),
			"kind %d count survives the binding round-trip" % kind)

	DirAccess.remove_absolute(ProjectSettings.globalize_path(out_path))
