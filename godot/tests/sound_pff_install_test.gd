extends GutTest

## Mission audio through the GAME SHELL's actual mount: mount_runtime over a
## minted packed install (the boot table's PFF archives, PFF3 through
## WorldFixture.write_pff, plus one expansion's own archive; loose files gated
## on /d [orig boot table: docs/required-resources.md]), probed on the base
## mount and on every expansion the install carries, so marker silence at a
## packed install shows up here as a red with a per-file visibility report.
## Placement follows retail's: the banks, their waves, items.def and the
## mission in localres.pff, the mission's co-named profile in language.pff
## (engine/base/resource_index/file_kind.cpp), an expansion's own bank in
## expansion/<name>/<name>.pff.

const MISSION := "synthsnd.bms"
const EXPANSION := "synx01"

# Two envs marker classes: one names game.lwf's ambient loop, one a set only
# the expansion's own bank carries (the global bank chain searches
# <exp>L.lwf, <exp>.lwf before gamelocl.lwf, game.lwf: runtime/audio/bank_chain).
const ITEMS := """begin "Light hum marker"
  id 100311
  type decoration
  move_function envs
  soundloop_1 LPNV_LIGHT
  soundloop_2 LPNV_LIGHT
  soundloop_3 LPNV_LIGHT
  soundloop_4 LPNV_LIGHT
end

begin "Expansion ambience marker"
  id 100312
  type decoration
  move_function envs
  soundloop_1 SYN_EXP_AMB
  soundloop_2 SYN_EXP_AMB
  soundloop_3 SYN_EXP_AMB
  soundloop_4 SYN_EXP_AMB
end
"""

var _dir := ""
var _banks := 0


func after_each() -> void:
	if not _dir.is_empty():
		TestFs.remove_dir_recursive(_dir)
	_dir = ""


func _bank_bytes(scratch: String, set_names: PackedStringArray) -> PackedByteArray:
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
	_banks += 1
	var path := scratch.path_join("bank_%d.lwf" % _banks)
	assert_eq(lwf.save_file(path), OK)
	var bytes := FileAccess.get_file_as_bytes(path)
	DirAccess.remove_absolute(path)
	return bytes


func _mission_bytes(scratch: String) -> PackedByteArray:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	mission.add_entity(MissionData.KIND_BUILDING, 100311, Vector3(10, 0, 0), Vector3.ZERO)
	mission.add_entity(MissionData.KIND_BUILDING, 100312, Vector3(20, 0, 0), Vector3.ZERO)
	var path := scratch.path_join(MISSION)
	assert_eq(mission.save_as(path), OK)
	var bytes := FileAccess.get_file_as_bytes(path)
	DirAccess.remove_absolute(path)
	return bytes


func _stage_install() -> String:
	var install := TestFs.cache_dir(self, "sound_pff_install")
	var scratch := install.path_join("scratch")
	assert_eq(DirAccess.make_dir_recursive_absolute(scratch), OK)
	var tone := FileAccess.get_file_as_bytes(
			ProjectSettings.globalize_path("res://../fixtures/lwf/tone.wav"))
	assert_false(tone.is_empty())
	WorldFixture.write_pff(self, install.path_join("localres.pff"), [
		{"name": "game.lwf", "bytes": _bank_bytes(scratch, PackedStringArray(["LPNV_LIGHT"]))},
		{"name": "gamelocl.LWF", "bytes": _bank_bytes(scratch,
				PackedStringArray(["SYN_LOCAL_AMB"]))},
		{"name": "tone.wav", "bytes": tone},
		{"name": "items.def", "bytes": TestFs.crlf(ITEMS)},
		{"name": MISSION, "bytes": _mission_bytes(scratch)},
	])
	WorldFixture.write_pff(self, install.path_join("language.pff"), [
		{"name": "synthsnd.LWF", "bytes": _bank_bytes(scratch,
				PackedStringArray(["SYN_PROFILE"]))},
	])
	WorldFixture.write_pff(self, install.path_join("resource.pff"), [
		{"name": "synthsnd.env", "bytes": "unused"},
	])
	var expansion_dir := install.path_join("expansion").path_join(EXPANSION)
	assert_eq(DirAccess.make_dir_recursive_absolute(expansion_dir), OK)
	WorldFixture.write_pff(self, expansion_dir.path_join(EXPANSION + ".pff"), [
		{"name": EXPANSION + ".lwf", "bytes": _bank_bytes(scratch,
				PackedStringArray(["SYN_EXP_AMB"]))},
	])
	TestFs.remove_dir_recursive(scratch)
	return install


func test_pff_install_mission_audio() -> void:
	_dir = _stage_install()
	var mounts := PackedStringArray([""])
	var expansions := ResourceRoot.new().list_expansions(_dir)
	assert_eq(expansions, PackedStringArray([EXPANSION]),
			"the install enumerates its one expansion")
	mounts.append_array(expansions)
	for expansion in mounts:
		_probe_mission_audio(_dir, expansion)


func _probe_mission_audio(install_dir: String, expansion: String) -> void:
	var root := ResourceRoot.new()
	var err: int = root.mount_runtime(install_dir, expansion, false, "jo")
	assert_eq(err, OK, "mount_runtime(%s, exp=%s) mounts" % [install_dir, expansion])
	if err != OK:
		return

	# --- File-visibility report: every name the mission-audio chain loads, so
	# one run discriminates bank-load vs item-db causes.
	var probe_names: PackedStringArray = [
		"game.lwf", "gamelocl.LWF", "game2.lwf", "game3.lwf", "items.def",
	]
	if not expansion.is_empty():
		probe_names.append(expansion + "L.lwf")
		probe_names.append(expansion + ".lwf")
	for n in probe_names:
		var have: bool = root.has_file(n)
		var size: int = root.read_file(n).size() if have else 0
		gut.p("PFF install: %-16s has_file=%-5s size=%d" % [n, str(have), size])

	assert_true(root.has_file("game.lwf"),
		"game.lwf (the global ambient-loop bank) is served from the PFF mount")
	assert_true(root.has_file("items.def"),
		"items.def is served from the PFF mount")
	assert_eq(root.has_file(EXPANSION + ".lwf"), not expansion.is_empty(),
		"the expansion's own bank is served only on its mount")
	assert_true(root.has_file(MISSION), "the mission is served from the PFF mount")
	assert_true(root.has_file(MISSION.get_basename() + ".LWF"),
		"the mission's co-named profile is served from the PFF mount")

	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, MISSION), OK,
		"%s parses from the mount" % MISSION)

	var item_db := ItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK,
		"items.def loads from the mount")

	var container := Node3D.new()
	add_child_autofree(container)

	var audio = MissionAudio.create(root, item_db)
	var stats := audio.setup(mission, MISSION, container)
	gut.p("PFF install mission audio (exp=%s): %s" % [expansion, str(stats.to_json_value())])
	assert_gt(int(stats.banks_loaded), 0,
		"sound banks load through the PFF mount")
	assert_eq(int(stats.markers_total), 2, "both envs markers participate")
	assert_gt(int(stats.markers_resolved), 0,
		"sound markers resolve against the install's items.def")
	assert_eq(int(stats.markers_resolved), 1 if expansion.is_empty() else 2,
		"the expansion's marker resolves only through the expansion's own bank")
	assert_gte(int(stats.ambient_candidates),
		int(stats.markers_resolved),
		"each resolved marker describes at least one ambient layer candidate")
	audio.teardown()
