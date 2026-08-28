extends GutTest
## MissionEndFlow: the SP round_end lead-in, the mounted score screen, the ESC
## routing and the teardown reset. The shell-level flow (the sim's round_end
## effect through the mounted MissionEndScreen to the menu) is covered by
## main_game_lifecycle_test.


func test_begin_arms_the_lead_in_and_tick_expires_it() -> void:
	var flow := MissionEndFlow.new()
	assert_false(flow.is_round_ended())
	assert_false(flow.tick(1.0), "an idle flow never fires")
	flow.begin(2, null)
	assert_true(flow.is_round_ended())
	assert_false(flow.tick(MissionEndFlow.LEAD_IN_SECONDS * 0.5), "the beat is still running")
	assert_true(flow.tick(MissionEndFlow.LEAD_IN_SECONDS * 0.5),
			"the beat expires exactly at the lead-in")
	assert_false(flow.request_screen_exit(),
			"ESC before the screen mounts falls to the caller's direct exit")


func test_show_screen_mounts_once_and_routes_exit() -> void:
	var flow := MissionEndFlow.new()
	var mount := Node.new()
	add_child_autofree(mount)
	flow.begin(1, null)
	var exits: Array[int] = []
	flow.show_screen(null, "", null, mount, func() -> void: exits.append(1))
	var screen := mount.get_node_or_null("MissionEndScreen") as MissionEndScreen
	assert_not_null(screen, "the score screen mounts under the given parent")
	assert_true(flow.has_screen())
	assert_false(flow.tick(10.0), "a mounted screen stops the beat")
	flow.show_screen(null, "", null, mount, func() -> void: exits.append(2))
	assert_eq(mount.get_child_count(), 1, "one screen per round")
	assert_true(flow.request_screen_exit(), "ESC routes through the mounted screen")
	assert_eq(exits, [1], "the first mount's exit callable fires once")
	flow.reset()
	assert_false(flow.is_round_ended())
	assert_false(flow.has_screen())
	await get_tree().process_frame
	assert_null(mount.get_node_or_null("MissionEndScreen"), "reset frees the screen")
	assert_false(flow.tick(10.0), "a reset flow is idle again")
