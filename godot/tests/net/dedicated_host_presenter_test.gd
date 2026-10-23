extends GutTest

# A dedicated host presents no entity (engine inmatch::Session::presents_entities): its render
# callback never takes the scene arm its entities are drawn in, and its mission start inits no
# render subsystem. MissionRoot's session step builds and applies nothing there (no row walk,
# no pass, no pass device) while the session row runs unchanged, and a ticked frame drops what
# its updates raised for the passes undrawn (engine world::discard_entity_presentation). A
# listen host over the same mission is the control: its wire walk takes an admitted joiner's
# body and its fire pass presents a round. The engine half is ctest's
# entity_presentation_discard.

const PLAYER_ITEM := 0x14B9


func _hosted_world(serve_and_play: bool) -> GameWorld:
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path(RuntimeFixture.directory())), OK)
	world.set_resource_root(root)
	var config := HostSessionConfig.new()
	config.mission = "mnml.bms"
	config.bind_port = 0
	config.game_type = HostSessionConfig.GAME_TYPE_COOP
	var options := config.to_session_options()
	options.serve_and_play = serve_and_play
	assert_eq(world.load_mission_as_host(options), OK)
	return world


# A raw joiner over real loopback UDP, driven with the hosted world's frames until it is in
# the match.
func _join(world: GameWorld, host: Simulation) -> Simulation:
	var mission := MissionData.new()
	assert_eq(mission.open_file(RuntimeFixture.file("mnml.bms")), OK)
	var joiner := Simulation.new()
	assert_true(joiner.enable_join(
			"127.0.0.1", host.get_host_listen_port(), "PresenterJoiner"))
	assert_true(joiner.load_from_mission_data(mission))
	for _i in range(800):
		world.tick(Vector3.ZERO)
		joiner.step()
		if joiner.is_joined_in_match():
			break
		OS.delay_msec(2)
	return joiner


# The joiner's body in the host's pool: a player body that is not the host's own player (a
# listen host's local player). -1 when the pool holds none.
func _joiner_body_handle(sim: Simulation) -> int:
	var own: int = sim.get_local_player_wire_handle() if sim.has_local_player() else -1
	for index in range(sim.get_entity_count()):
		var card: EntityCard = sim.entity_card_by_ai_index(index)
		if card != null and card.get_item_id() == PLAYER_ITEM and card.get_wire_handle() != own:
			return card.get_wire_handle()
	return -1


# Frames until one runs a logic tick (the session's banked update); false when none did.
func _tick_once(world: GameWorld, host: Simulation) -> bool:
	for _i in range(30):
		var before := host.get_logic_tick()
		world.tick(Vector3.ZERO)
		if host.get_logic_tick() != before:
			return true
	return false


func test_a_serve_only_hosts_session_presents_no_entity() -> void:
	for serve_and_play in [true, false]:
		var world: GameWorld = await _hosted_world(serve_and_play)
		var host: Simulation = world.get_sim()
		assert_not_null(host)
		if host == null:
			world.unload()
			continue
		assert_eq(host.session_role(), Simulation.ROLE_LISTEN_HOST if serve_and_play
				else Simulation.ROLE_DEDICATED_HOST)
		var runtime: MissionRoot = world.get_runtime()
		var presenter: EntityPresenter = runtime.get_entity_presenter()
		assert_not_null(presenter)
		if presenter == null:
			world.unload()
			continue
		# The passes bind their devices on a presenting host alone: the tracer ribbon's mesh
		# under the mission container is one.
		assert_eq(presenter.fire_ribbon_mesh() != null, serve_and_play,
				"the passes bind their devices on a presenting host alone")

		var joiner := _join(world, host)
		assert_true(joiner.is_joined_in_match(), "the joiner reached the match")
		# Frames past the admission: a listen host's wire walk takes the body (its cold-spawn
		# budget builds, or counts unresolved, every row it owes).
		for _i in range(30):
			world.tick(Vector3.ZERO)
			joiner.step()
			OS.delay_msec(2)
		# The sim's words: the host's pool holds the joiner's body whichever role it plays.
		var joiner_body := _joiner_body_handle(host)
		assert_gte(joiner_body, 0, "the host's session admitted the joiner's body")
		var wire: WirePresentStats = runtime.get_wire_present_stats()
		if serve_and_play:
			assert_gt(wire.spawned + wire.unresolved, 0,
					"a listen host's wire walk takes the admitted body")
		else:
			assert_eq(wire.spawned + wire.unresolved + wire.live, 0,
					"a dedicated host's session builds no wire body")
			assert_eq(presenter.wire_entity_count(), 0, "no wire node exists")
			assert_eq(runtime.get_perf_counters().present_us, 0, "no present frame ran")

		# A round the joiner's body fires (the fire pass counts no round of the host's own
		# player, whose fire presents through its action slot): the fire pass presents it on a
		# listen host; a dedicated host's ticked frame drops it undrawn. Either way the frame
		# empties the fire ring.
		var fires_before: int = presenter.get_fire_present_stats().fires
		assert_gte(host.debug_spawn_round(Vector3(0, 40, 0), Vector3(1, 0, 0), "AM_556MM",
				joiner_body), 0, "the host's ammo table carries the fixture round")
		assert_true(_tick_once(world, host), "a frame ran a logic tick")
		assert_eq(host.drain_fire_presentation_events().size(), 0,
				"the ticked frame emptied the fire ring")
		assert_eq(presenter.get_fire_present_stats().fires - fires_before > 0, serve_and_play,
				"the fire pass runs on a presenting host alone")
		world.unload()
