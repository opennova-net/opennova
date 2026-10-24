extends GutTest

# A joiner's streamed statics are placed in its load (GameWorld's streamed placement, past the
# stream's materialize), and their static terrain shadow casters resolve there with them, as the
# authored placement's do: the first frame that draws the terrain finds every caster's geometry
# and texture alpha made and composes its pages loading nothing (D-NET-NEW-LF-18; engine
# mission/mission_load_plan.h carries the witness: retail's tile compose draws each caster through
# the model its row holds, which the mission start's reload loaded ahead of the 0x0B). Before the
# fix the joiner resolved them in its first visible terrain frame (LP-s25: 130..156 ms terrain
# legs with about fifty texture decodes). No retail files: the minimal pack, its LAN host's mission
# carrying one building over the synthetic house (fixtures/threedi/synth/house.3di).

const HOUSE_3DI := "res://../fixtures/threedi/synth/house.3di"
const HOUSE_ID := 102001
const HOUSE_ITEM_DEF := """begin "Synthetic House"
  id 102001
  type building
  graphic SynHouse
  sid synhouse
  hp 5000
end
"""

var _root := ""


func after_each() -> void:
	if not _root.is_empty():
		TestFs.remove_dir_recursive(_root)
		_root = ""


func _join_target(port: int) -> JoinTarget:
	var target := JoinTarget.new()
	target.host_ip = "127.0.0.1"
	target.port = port
	target.mission = "mnml"
	target.player_name = "Joiner"
	return target


func test_a_joiners_streamed_casters_resolve_in_its_load() -> void:
	_root = WorldFixture.stage_minimal_root("streamed_static_shadow")
	TestFs.copy(self, HOUSE_3DI, _root.path_join("SynHouse.3di"))
	var items := FileAccess.get_file_as_string(_root.path_join("items.def"))
	TestFs.write_text(self, _root.path_join("items.def"), items + "\n" + HOUSE_ITEM_DEF)
	var world := WorldFixture.make_world(self)
	await get_tree().process_frame
	world.set_playable(false)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_root), OK)
	world.set_resource_root(root)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	assert_not_null(mission.add_entity(MissionData.KIND_BUILDING, HOUSE_ID,
			Vector3(16.0, 16.0, 0.0), Vector3.ZERO), "the host's mission carries the building")
	var host := Simulation.new()
	var host_options := HostSessionOptions.new()
	host_options.server_name = "Streamed Shadow Host"
	host_options.mission_name = mission.get_mission_name()
	host_options.mission_file = "mnml.bms"
	host_options.expansion = ""
	host_options.game_type = 0x30020
	host_options.max_players = 4
	host.configure_host_session(host_options)
	assert_true(host.enable_host_listen(0))
	assert_true(host.load_from_mission_data(mission))
	# Read at the load's end, in world_loaded's own call: no frame has drawn the terrain yet.
	var observed := {"loaded": false, "shadow": {}}
	world.world_loaded.connect(func() -> void:
		observed["loaded"] = true
		var terrain: Terrain = world.get_terrain_node()
		observed["shadow"] = terrain.get_tile_cache_diagnostics() if terrain != null else {}
	, CONNECT_ONE_SHOT)
	assert_eq(world.load_mission_as_joiner(_join_target(host.get_host_listen_port())), OK)
	for _frame in range(600):
		host.step()
		await get_tree().process_frame
		if bool(observed["loaded"]):
			break
	assert_true(bool(observed["loaded"]), "the S2C 0x1A's mission start loaded the world")
	var timeline: LoadTimeline = world.last_load_timeline()
	assert_not_null(timeline)
	if timeline != null:
		var names := Array(timeline.span_names())
		var place := names.find("place")
		var shadows := names.find("static_shadows")
		assert_true(place >= 0 and place < shadows,
				"the streamed placement, then its casters: %s" % [names])
	var shadow: Dictionary = observed["shadow"]
	assert_eq(int(shadow.get("shadow_provider_admitted_count", -1)), 1,
			"the streamed building is the load's one caster: %s" % [shadow])
	assert_eq(int(shadow.get("shadow_provider_resolved_casters", -1)), 1,
			"its geometry resolved inside the load, ahead of the first terrain frame")
	world.unload()
