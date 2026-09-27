extends GutTest

## Guarded end-to-end probe against real loose JO data (Desktop/JOX):
## mount the dir, load 00TRa.bms + items.def, run MissionAudio, and report how
## many sound markers resolved to a sound set. Skips when the data is absent.


# The loose JOX dump root is the documented OPENNOVA_JO_ASSETS gate
# (docs/asset-gated-tests.md, RetailData.assets()); machine paths go in
# .claude/settings.local.json env, never tracked.
var JO_DIR := RetailData.assets()


func test_jo_00tra_sound_marker_resolution() -> void:
	if JO_DIR.is_empty():
		pending("OPENNOVA_JO_ASSETS (the JOX extract) is required for the real-data probe")
		return

	var root := ResourceRoot.new()
	root.set_root_dir(JO_DIR)
	if not root.has_file("00TRa.bms"):
		pending("00TRa.bms is not resolvable from OPENNOVA_JO_ASSETS")
		return
	assert_true(root.has_file("00TRa.LWF"), "mission has a co-named sound profile")

	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, "00TRa.bms"), OK, "mission parses")

	var item_db := ItemDatabase.new()
	item_db.load_from_resource_root(root, "items.def")

	var container := Node3D.new()
	add_child_autofree(container)

	var audio = MissionAudio.create(root, item_db)
	var stats := audio.setup(mission, "00TRa.bms", container)
	gut.p("JO 00TRa mission audio: %s" % str(stats.to_json_value()))

	gut.p("bank exposes %d sound sets" % audio.get_bank().get_set_names().size())
	assert_gt(int(stats.banks_loaded), 0, "the mission .LWF / game.lwf / gamelocl.LWF load")
	assert_true(audio.get_bank().has_set("LPNV_LIGHT"), "game.lwf ambient-loop sets are loaded")
	# The payoff: a real JO mission describes ambient layers at its sound markers
	# without materializing one player per layer. 00TRa resolves ~210 of its 390
	# markers (the rest are waypoints /
	# spawns / time-of-day one-shot markers, which use nightshot/dawnshot/duskshot).
	assert_gt(int(stats.markers_resolved), 0, "sound markers resolve to real sound sets")
	assert_gte(int(stats.ambient_candidates),
		int(stats.markers_resolved),
		"each resolved marker describes at least one ambient layer candidate")
	assert_between(int(stats.markers_resolved), 0, int(stats.markers_total),
		"resolved markers are a subset of total markers")
	audio.teardown()
