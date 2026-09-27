extends GutTest

## Guarded diagnosis + regression probe against a real PFF install. The install
## comes from OPENNOVA_JO_DIR (RetailData.install()); the base mount and every
## expansion the install carries are each probed. The ambient-marker path is
## GUT-green against the flat JOX extract (sound_integration_test.gd); this
## probe exercises the GAME SHELL's actual mount instead — mount_runtime: PFF
## archives + expansion, loose files gated on /d [orig boot table:
## docs/required-resources.md] — so marker silence at a packed install shows up
## here as a red with a per-file visibility report, not just as an in-game
## mystery. Skips when the install is absent (CI never has it).


const MISSION_CANDIDATES: PackedStringArray = ["00TRa.bms", "00TRg.bms"]


func test_pff_install_mission_audio() -> void:
	var install_dir := RetailData.install()
	if install_dir.is_empty():
		pending("OPENNOVA_JO_DIR / a retail PFF install is required for the PFF-install probe")
		return
	var mounts := PackedStringArray([""])
	mounts.append_array(RetailData.expansions())
	for expansion in mounts:
		_probe_mission_audio(install_dir, expansion)


func _probe_mission_audio(install_dir: String, expansion: String) -> void:
	var root := ResourceRoot.new()
	var err: int = root.mount_runtime(install_dir, expansion, false, "jo")
	assert_eq(err, OK, "mount_runtime(%s, exp=%s) mounts" % [install_dir, expansion])
	if err != OK:
		return

	# --- File-visibility report: every name the mission-audio chain loads, plus
	# the interactive-music script names (menu_shell resolve_music_pair probes
	# these), so one run discriminates bank-load vs item-db vs music-pair causes.
	var probe_names: PackedStringArray = [
		"game.lwf", "gamelocl.LWF", "game2.lwf", "game3.lwf",
		"items.def",
		"menumus.bin", "gamemus.bin",
	]
	if not expansion.is_empty():
		probe_names.append(expansion + "L.lwf")
		probe_names.append(expansion + ".lwf")
		probe_names.append("M" + expansion + ".bin")
		probe_names.append("G" + expansion + ".bin")
	for n in probe_names:
		var have: bool = root.has_file(n)
		var size: int = root.read_file(n).size() if have else 0
		gut.p("PFF install: %-16s has_file=%-5s size=%d" % [n, str(have), size])

	assert_true(root.has_file("game.lwf"),
		"game.lwf (the global ambient-loop bank) is served from the PFF mount")
	assert_true(root.has_file("items.def"),
		"items.def is served from the PFF mount")

	# --- Mission pick: a stock training mission when packed, else the first
	# .bms the index lists (a modded install may replace the stock set).
	var mission_name := ""
	for c in MISSION_CANDIDATES:
		if root.has_file(c):
			mission_name = c
			break
	if mission_name.is_empty():
		var listed := root.list_files(".bms")
		if not listed.is_empty():
			mission_name = String(listed[0]).get_file()
	if mission_name.is_empty():
		fail_test("no .bms resolvable from the PFF mount (tried %s and list_files)"
			% str(MISSION_CANDIDATES))
		return
	gut.p("PFF install: probing mission %s (co-named .LWF has_file=%s)"
		% [mission_name, str(root.has_file(mission_name.get_basename() + ".LWF"))])

	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, mission_name), OK,
		"%s parses from the mount" % mission_name)

	var item_db := ItemDatabase.new()
	assert_eq(item_db.load_from_resource_root(root, "items.def"), OK,
		"items.def loads from the mount")

	var container := Node3D.new()
	add_child_autofree(container)

	var audio = MissionAudio.create(root, item_db)
	var stats := audio.setup(mission, mission_name, container)
	gut.p("PFF install mission audio: %s" % str(stats.to_json_value()))
	assert_gt(int(stats.banks_loaded), 0,
		"sound banks load through the PFF mount")
	if int(stats.markers_total) > 0:
		assert_gt(int(stats.markers_resolved), 0,
			"sound markers resolve against the install's items.def")
		assert_gte(int(stats.ambient_candidates),
			int(stats.markers_resolved),
			"each resolved marker describes at least one ambient layer candidate")
	audio.teardown()
