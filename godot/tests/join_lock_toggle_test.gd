extends GutTest

# The lockgame action's Godot leg over the REAL bindings (D-NET-659): the HUD
# toggles' lockgame row fires on an authority in a session alone, its event
# the engine word's top bit, which the binding's poll hands over as a positive
# 64-bit word; Simulation.toggle_join_lock flips a listen host's join lock
# (1 locked, 0 open) and toggles nothing without a host context (-1).
# [orig: Input_HandleActionBinding case 119 @0x49BC1C..0x49BC36;
#  Server_ToggleJoinLock @0x50D750..0x50D767]


func test_lockgame_row_fires_the_top_bit_on_an_authority() -> void:
	var toggles := HudToggles.new()
	var row := 1 << HudToggles.ROW_LOCK_GAME
	# poll(rows, chorded, active, in_session, objective, local_alive, authority, peer)
	assert_eq(toggles.poll(0, false, true, true, false, true, true, true), 0, "the key up")
	var events := toggles.poll(row, false, true, true, false, true, true, true)
	assert_ne(events & HudToggles.EVENT_JOIN_LOCK_TOGGLED, 0,
			"the press edge asks for the toggle")
	assert_gt(events, 0, "the 32-bit word's top bit arrives as a positive 64-bit word")
	assert_eq(events, HudToggles.EVENT_JOIN_LOCK_TOGGLED)
	# A joiner (no authority) never toggles; out of a session neither does the host.
	toggles.poll(0, false, true, true, false, true, false, true)
	assert_eq(toggles.poll(row, false, true, true, false, true, false, true), 0, "a joiner")
	toggles.poll(0, false, true, false, false, true, true, true)
	assert_eq(toggles.poll(row, false, true, false, false, true, true, true), 0,
			"single player")


func test_toggle_without_a_host_context() -> void:
	var sim := Simulation.new()
	assert_eq(sim.toggle_join_lock(), -1, "no host context: nothing toggles")


func test_toggle_flips_a_listen_hosts_lock() -> void:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var host := Simulation.new()
	autofree(host)
	var host_options := HostSessionOptions.new()
	host_options.game_type = 0x30020
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0), "a real listen host")
	assert_true(host.load_from_mission_data(mission))
	assert_eq(host.toggle_join_lock(), 1, "the first toggle locks the game")
	assert_eq(host.toggle_join_lock(), 0, "the second opens it again")
