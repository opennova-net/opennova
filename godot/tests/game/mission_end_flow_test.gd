extends GutTest
## MissionEndFlow and MissionEndScreen: the shell half of the SP end-of-round
## cine. The engine runs the cine (world/epilog_cine.h; ctest
## sp_mission_lifecycle pins its stage machines, timings and values); here
## the flow's arm and mount gates, and the screen reading a real world's cine
## after a WAC Win or Lose ends the round. The shell-level flow (the round end
## through the screen, ESC through the real input path to the menu) is
## main_game_lifecycle_test's.


# A standalone SP world with its human (the world-run gate) and a script that
# ends the round on its first execution.
func _ending_sim(script: String) -> Simulation:
	var sim := Simulation.new()
	sim.build_demo_mission()
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1),
			"the local player is the human that keeps the world running")
	assert_true(sim.compile_and_set_wac(PackedStringArray([script])))
	return sim


# Step the world until its end screen is built (the cine's stage machine).
func _run_to_screen(sim: Simulation, max_steps := 400) -> EpilogCineState:
	for _i in range(max_steps):
		sim.step()
		var cine := sim.get_epilog_cine()
		if cine != null and cine.screen_active:
			return cine
	return sim.get_epilog_cine()


func _events_of(cine: EpilogCineState, kind: int) -> Array[CineEventRecord]:
	var out: Array[CineEventRecord] = []
	for e: CineEventRecord in cine.events:
		if e.kind == kind:
			out.append(e)
	return out


func test_flow_arms_once_out_of_a_session_and_mounts_one_screen() -> void:
	var flow := MissionEndFlow.new()
	flow.begin(null)
	assert_false(flow.is_round_ended(), "no world, no flow")
	var sim := Simulation.new()
	sim.build_demo_mission()
	flow.begin(sim)
	assert_true(flow.is_round_ended(), "a single-player round end arms the flow")
	var mount := Node.new()
	add_child_autofree(mount)
	flow.show_screen(sim, func() -> String: return "", null, mount)
	assert_true(flow.has_screen())
	assert_not_null(mount.get_node_or_null("MissionEndScreen"))
	flow.show_screen(sim, func() -> String: return "", null, mount)
	assert_eq(mount.get_child_count(), 1, "one screen per round")
	flow.reset()
	assert_false(flow.is_round_ended())
	assert_false(flow.has_screen())
	await get_tree().process_frame
	assert_null(mount.get_node_or_null("MissionEndScreen"), "reset frees the screen")


func test_the_lose_cine_builds_the_mission_failed_screen() -> void:
	var sim := _ending_sim("Lose(0)")
	var cine := _run_to_screen(sim)
	assert_not_null(cine)
	if cine == null:
		return
	assert_eq(cine.mode, EpilogCineState.MODE_LOSE, "WAC Lose ends the round lost")
	assert_true(cine.screen_active, "the MISSION FAILED screen is built")
	var images := _events_of(cine, CineEventRecord.KIND_IMAGE_FADE)
	assert_eq(images.size(), 3, "the fade pair and the backdrop")
	if images.size() == 3:
		assert_eq(images[2].image, "jo_Epil2.tga")
	var lines := _events_of(cine, CineEventRecord.KIND_TEXT_FADE)
	assert_eq(lines.size(), 3, "MISSION FAILED, the banner line and the key help")
	if lines.size() == 3:
		assert_eq(lines[0].text_key, "STROVER_MISSION_FAILED")
		assert_eq(lines[1].text_source, CineEventRecord.TEXT_SOURCE_BANNER)
		assert_eq(lines[2].text_key, "STREPILOG_KEYINFO")
	# The screen draws from the same cine.
	var screen := MissionEndScreen.new()
	add_child_autofree(screen)
	screen.setup(sim, func() -> String: return "You have killed a teammate.", null)
	await get_tree().process_frame
	await get_tree().process_frame
	assert_not_null(screen.get_cine(), "the screen reads the world's cine")
	if screen.get_cine() != null:
		assert_eq(screen.get_cine().mode, EpilogCineState.MODE_LOSE)


func test_the_win_cine_builds_the_score_screen_with_its_counters() -> void:
	var sim := _ending_sim("Win(1)")
	var cine := _run_to_screen(sim)
	assert_not_null(cine)
	if cine == null:
		return
	assert_eq(cine.mode, EpilogCineState.MODE_WIN)
	assert_true(cine.screen_active, "the score screen is built")
	var counters := _events_of(cine, CineEventRecord.KIND_EPILOG_COUNTER)
	assert_eq(counters.size(), 4, "the four counter lines")
	if counters.size() == 4:
		assert_eq(counters[0].label_key, "STREPILOG_OBJECTIVEBONUS")
		assert_true(counters[0].value_text.begins_with("0/"), "nothing won in the demo mission")
		assert_eq(counters[2].label_key, "STREPILOG_TEAMUNITS")
		assert_eq(counters[2].value_text, "0", "the team line has no max")
	var images := _events_of(cine, CineEventRecord.KIND_IMAGE_FADE)
	if images.size() == 3:
		assert_eq(images[2].image, "jo_Epil.tga")
