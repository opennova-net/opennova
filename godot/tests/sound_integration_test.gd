extends GutTest

## End-to-end mission audio over a minted install root: the global banks
## (game.lwf, gamelocl.lwf) and the mission's co-named sound profile are
## written through LwfData, items.def authors `envs` sound-marker classes, and
## MissionAudio resolves the placed markers to the loaded banks' sets. The
## shape follows the retail 00TRa witness this test replaced: a mission whose
## markers resolve only partly (00TRa resolved ~210 of its 390 markers; the
## rest name no loaded set), with game.lwf's ambient-loop set LPNV_LIGHT
## among the loaded sets.

const MISSION := "synthsnd.bms"

# Three envs marker classes: two name sets the global banks carry (one in
# game.lwf, one in gamelocl.lwf), one names a set no loaded bank has.
const ITEMS := """begin "Light hum marker"
  id 100301
  type decoration
  move_function envs
  soundloop_1 LPNV_LIGHT
  soundloop_2 LPNV_LIGHT
  soundloop_3 LPNV_LIGHT
  soundloop_4 LPNV_LIGHT
end

begin "Local ambience marker"
  id 100302
  type decoration
  move_function envs
  soundloop_1 SYN_LOCAL_AMB
  soundloop_2 SYN_LOCAL_AMB
  soundloop_3 SYN_LOCAL_AMB
  soundloop_4 SYN_LOCAL_AMB
end

begin "Unbanked marker"
  id 100303
  type decoration
  move_function envs
  soundloop_1 SYN_NO_SUCH_SET
  soundloop_2 SYN_NO_SUCH_SET
  soundloop_3 SYN_NO_SUCH_SET
  soundloop_4 SYN_NO_SUCH_SET
end
"""

var _dir := ""


func after_each() -> void:
	if not _dir.is_empty():
		TestFs.remove_dir_recursive(_dir)
	_dir = ""


func _bank(path: String, set_names: PackedStringArray) -> void:
	var lwf := LwfData.new()
	lwf.create_empty()
	for set_name in set_names:
		var set_i := lwf.add_set()
		lwf.set_set_field(set_i, "name", set_name)
		var layer_i := lwf.add_layer(set_i)
		lwf.set_layer_field(set_i, layer_i, "internal", true)
		lwf.set_layer_field(set_i, layer_i, "external", true)
		lwf.set_layer_field(set_i, layer_i, "falloff_radius", 200)
		var member_i := lwf.add_member(set_i, layer_i)
		lwf.set_member_field(set_i, layer_i, member_i, "wav_path", "tone.wav")
	assert_eq(lwf.save_file(path), OK, "the bank saves: %s" % path.get_file())


func test_minted_mission_sound_marker_resolution() -> void:
	_dir = TestFs.cache_dir(self, "sound_integration")
	TestFs.write_text(self, _dir.path_join("items.def"), ITEMS)
	TestFs.copy(self, ProjectSettings.globalize_path("res://../fixtures/lwf/tone.wav"),
			_dir.path_join("tone.wav"))
	_bank(_dir.path_join("game.lwf"), PackedStringArray(["LPNV_LIGHT", "SYN_GAME_AMB"]))
	_bank(_dir.path_join("gamelocl.lwf"), PackedStringArray(["SYN_LOCAL_AMB"]))
	# The mission's co-named profile: its dialog bank's sounds, never searched for
	# a marker's set.
	_bank(_dir.path_join("synthsnd.LWF"), PackedStringArray(["SYN_LOCAL_AMB_UNUSED"]))

	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_dir), OK)
	assert_true(root.has_file("synthsnd.LWF"), "mission has a co-named sound profile")

	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	mission.add_entity(MissionData.KIND_BUILDING, 100301, Vector3(10, 0, 0), Vector3.ZERO)
	mission.add_entity(MissionData.KIND_BUILDING, 100302, Vector3(20, 0, 0), Vector3.ZERO)
	mission.add_entity(MissionData.KIND_BUILDING, 100303, Vector3(30, 0, 0), Vector3.ZERO)

	var item_db := ItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK)

	var container := Node3D.new()
	add_child_autofree(container)

	var audio = MissionAudio.create(root, item_db)
	var stats := audio.setup(mission, MISSION, container)
	gut.p("minted mission audio: %s" % str(stats.to_json_value()))

	assert_gt(int(stats.banks_loaded), 0, "the global banks (game.lwf, gamelocl.lwf) load")
	assert_true(audio.get_bank().has_set("LPNV_LIGHT"), "game.lwf ambient-loop sets are loaded")
	assert_true(audio.get_bank().has_set("SYN_LOCAL_AMB"), "gamelocl.lwf sets are loaded")
	# A real mission describes ambient layers at its sound markers without
	# materializing one player per layer; a marker naming no loaded set stays
	# unresolved.
	assert_eq(int(stats.markers_total), 3, "every envs marker participates")
	assert_eq(int(stats.markers_resolved), 2, "the two banked markers resolve")
	assert_gt(int(stats.markers_resolved), 0, "sound markers resolve to real sound sets")
	assert_gte(int(stats.ambient_candidates),
		int(stats.markers_resolved),
		"each resolved marker describes at least one ambient layer candidate")
	assert_between(int(stats.markers_resolved), 0, int(stats.markers_total),
		"resolved markers are a subset of total markers")
	audio.teardown()
