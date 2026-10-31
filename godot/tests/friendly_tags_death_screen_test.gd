extends GutTest

# The friendly-tags cycle's death-screen step over the REAL bindings: a poll handed the latch
# and a title rule that steps past mode 1 (dfx2.exe's) lands on mode 2; without the rule, or
# without the latch, the cycle reaches mode 1 (Jointops.exe's). The no-router poll reads both
# from its sim through HudToggles.sim_death_screen / sim_friendly_tags_skip_mode_1, which give
# false with no sim. The witness rides the engine (base/title/title.h section 5.3).


func _cycle_to_off(toggles: HudToggles) -> void:
	while toggles.get_friendly_tag_mode() != 0:
		toggles.cycle_friendly_tags()


func _press(toggles: HudToggles, death_screen: bool, skip: bool) -> int:
	var row := 1 << HudToggles.ROW_FRIENDLY_TAGS
	# poll(rows, active, in_session, objective, local_alive, authority, peer, mouse_negative,
	#      head_drops, death_screen, friendly_tags_skip_mode_1)
	toggles.poll(0, true, false, false, true, true, true, false, 0, death_screen, skip)
	return toggles.poll(row, true, false, false, true, true, true, false, 0, death_screen, skip)


func test_the_death_screen_step_takes_the_latch_and_the_rule() -> void:
	var toggles := HudToggles.new()
	_cycle_to_off(toggles)
	assert_ne(_press(toggles, true, true) & HudToggles.EVENT_FRIENDLY_TAGS_CYCLED, 0)
	assert_eq(toggles.get_friendly_tag_mode(), 2, "latch and rule: mode 1 stepped past")
	_cycle_to_off(toggles)
	_press(toggles, true, false)
	assert_eq(toggles.get_friendly_tag_mode(), 1, "no rule: mode 1")
	_cycle_to_off(toggles)
	_press(toggles, false, true)
	assert_eq(toggles.get_friendly_tag_mode(), 1, "no latch: mode 1")


func test_the_sim_reads_are_false_without_a_sim() -> void:
	assert_false(HudToggles.sim_death_screen(null))
	assert_false(HudToggles.sim_friendly_tags_skip_mode_1(null))
