extends GutTest

## Guarded end-to-end probe against real loose JO data (Desktop/JOX):
## mount the dir, load 00TRa.bms + items.def, run NovaMissionAudio, and report how
## many sound markers resolved to a sound set. Skips when the data is absent.

const NovaMissionAudioScript = preload("res://engine/world/nova_mission_audio.gd")

const JO_DIR := "C:/Users/taylor/Desktop/JOX"


func test_jo_00tra_sound_marker_resolution() -> void:
	if not DirAccess.dir_exists_absolute(JO_DIR):
		pass_test("JOX not present; skipping real-data probe")
		return

	var root := NovaResourceRoot.new()
	root.set_root_dir(JO_DIR)
	if not root.has_file("00TRa.bms"):
		pass_test("00TRa.bms not resolvable from JOX; skipping")
		return
	assert_true(root.has_file("00TRa.LWF"), "mission has a co-named sound profile")

	var mission := NovaMissionData.new()
	assert_eq(mission.open_from_resource_root(root, "00TRa.bms"), OK, "mission parses")

	var item_db := NovaItemDatabase.new()
	item_db.load_from_resource_root(root, "items.def")

	var container := Node3D.new()
	add_child_autofree(container)

	var audio = NovaMissionAudioScript.new(root, item_db)
	var stats := audio.setup(mission, "00TRa.bms", container)
	gut.p("JO 00TRa mission audio: %s" % str(stats))

	gut.p("bank exposes %d sound sets" % audio.get_bank().get_set_names().size())
	assert_gt(int(stats.get("banks_loaded", 0)), 0, "the mission .LWF / game.lwf / gamelocl.LWF load")
	assert_true(audio.get_bank().has_set("LPNV_LIGHT"), "game.lwf ambient-loop sets are loaded")
	# The payoff: a real JO mission describes ambient layers at its sound markers
	# without materializing one player per layer. 00TRa resolves ~210 of its 390
	# markers (the rest are waypoints /
	# spawns / time-of-day one-shot markers, which use nightshot/dawnshot/duskshot).
	assert_gt(int(stats.get("markers_resolved", 0)), 0, "sound markers resolve to real sound sets")
	assert_gte(int(stats.get("ambient_candidates", 0)),
		int(stats.get("markers_resolved", 0)),
		"each resolved marker describes at least one ambient layer candidate")
	assert_between(int(stats.get("markers_resolved", 0)), 0, int(stats.get("markers_total", 0)),
		"resolved markers are a subset of total markers")
	audio.teardown()
