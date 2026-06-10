extends GutTest

# MissionPlayController: the play-in-editor host — the real game world
# (nova_world.tscn) in an editor-owned SubViewport. Headless coverage is the
# state machine and rejection paths (a successful boot needs real game assets:
# terrain + env + models; see screenshot_capture's mission_play target and the
# manual A/B against the standalone game).

const PlayController := preload("res://modtools/mission/mission_play_controller.gd")
const BMS_PATH := "res://../fixtures/bms/ash_i5b.reference.bms"


func _make_play():
	var play = PlayController.new()
	add_child_autofree(play)
	return play


func _fixture_root() -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/godot/dvxi5"))
	return root


func test_builds_the_play_stack() -> void:
	var play = _make_play()
	await get_tree().process_frame
	assert_not_null(play.get_node_or_null("PlayViewportContainer/PlayViewport"), "embedded SubViewport exists")
	assert_not_null(play.get_node_or_null("PlayViewportContainer/PlayViewport/World"), "the packaged game world is instanced")
	assert_not_null(play.get_play_camera(), "a fly camera drives the play view")
	assert_true(play.get_node("PlayViewportContainer/PlayViewport").own_world_3d,
		"the play world is isolated from the editor's 3D world")
	assert_false(play.is_playing())


func test_start_rejects_missing_or_unloaded_mission() -> void:
	var play = _make_play()
	await get_tree().process_frame
	var statuses: Array = []
	play.status_reported.connect(func(msg, is_err): statuses.append([msg, is_err]))

	assert_eq(play.start(null, "x.bms", _fixture_root()), ERR_UNAVAILABLE)
	assert_eq(play.start(NovaMissionData.new(), "x.bms", _fixture_root()), ERR_UNAVAILABLE,
		"an unloaded document cannot play")
	assert_false(play.is_playing())
	assert_eq(statuses.size(), 2, "both rejections explain themselves")


func test_start_rejects_missing_root() -> void:
	var play = _make_play()
	await get_tree().process_frame
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_eq(play.start(mission, "x.bms", null), ERR_UNCONFIGURED)
	assert_false(play.is_playing())


func test_failed_world_boot_leaves_a_clean_stopped_state() -> void:
	# The dvxi5 fixture has the terrain but no .env, so the real mission load
	# fails inside NovaWorld; the controller must come back not-playing with the
	# failure surfaced, ready for the workspace to swap the edit viewport back.
	var play = _make_play()
	await get_tree().process_frame
	var mission := NovaMissionData.new()
	assert_eq(mission.open_file(ProjectSettings.globalize_path(BMS_PATH)), OK)

	var statuses: Array = []
	play.status_reported.connect(func(msg, is_err): statuses.append([String(msg), bool(is_err)]))
	var err: int = play.start(mission, "ash_i5b.bms", _fixture_root())
	assert_ne(err, OK, "the fixture root cannot boot a full world (no .env)")
	assert_false(play.is_playing(), "a failed boot is fully stopped")
	assert_gt(statuses.size(), 0, "the failure reason reached the host")
	assert_true(statuses[0][1], "and it is an error status")

	# Stop on a never-started controller is inert, repeatedly.
	play.stop()
	play.stop()
	assert_false(play.is_playing())
