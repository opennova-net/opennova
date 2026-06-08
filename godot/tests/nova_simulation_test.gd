extends GutTest

# NovaSimulation (the GDExtension binding): promote a synthetic BMS mission into a live
# world + AI system, tick it, and confirm the AI walks entities along their authored route.
# This is the in-Godot end of step 1 (promotion) + step 2 (locomotion).

func test_demo_mission_promotes() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	assert_true(sim.is_loaded(), "demo mission promoted")
	assert_eq(sim.get_entity_count(), 2, "two organics got AI brains")
	assert_eq(sim.get_brain_count(), 2, "two AI brains attached")
	assert_eq(sim.get_spawned_count(), 3, "two organics + one building spawned into pools")
	assert_eq(sim.get_entity_state(0), 16, "a routed organic starts in GROUND_FOLLOWWP (16)")
	sim.free()

func test_entities_walk_their_route() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	var start: Vector3 = sim.get_entity_position(0)
	for _i in range(20):
		sim.step()
	var moved: Vector3 = sim.get_entity_position(0)
	# The AI mover aims at the first route marker (mission (100,0,0) -> Godot +x) and the
	# locomotion advances the entity there over the ticks.
	assert_gt(start.distance_to(moved), 1.0, "entity walked away from its spawn")
	assert_gt(moved.x, start.x + 1.0, "walked toward the first route marker (+x)")
	sim.free()

func test_load_from_editor_mission_data() -> void:
	# The editor-integration path: promote a live NovaMissionData (what the mission editor holds),
	# not a file or the synthetic demo. KIND_ORGANIC = 3.
	var md := NovaMissionData.new()
	assert_eq(md.create_default(), OK, "empty in-memory mission created")
	md.add_entity(3, 0, Vector3(0, 0, 0), Vector3.ZERO)
	md.add_entity(3, 0, Vector3(10, 0, 0), Vector3.ZERO)
	var sim := NovaSimulation.new()
	assert_true(sim.load_from_mission_data(md), "promoted the editor's live mission")
	assert_eq(sim.get_brain_count(), 2, "both organics got AI brains")
	assert_eq(sim.get_entity_kind(0), 3, "entity 0 maps back to KIND_ORGANIC")
	sim.free()

func test_transport_play_flag() -> void:
	var sim := NovaSimulation.new()
	sim.build_demo_mission()
	assert_false(sim.is_playing(), "starts paused")
	sim.set_playing(true)
	assert_true(sim.is_playing(), "play flag toggles")
	sim.free()
