extends GutTest

# AiDebugSession: the shell wiring between the F3 AI window's overlay toggles
# (the DevTools "ai_view_request" signal) and the world's debug-view set.
# Headless: no ImGui context attaches, but the DevTools node's signals and the
# session's apply path work and are pinned here.


func _make_world() -> GameWorld:
	var world := GameWorld.new()
	var terrain := Terrain.new()
	terrain.name = "Terrain"
	world.add_child(terrain)
	return world


func _make_pair() -> Array:
	var dev_tools := DevTools.new()
	add_child_autofree(dev_tools)
	var session := AiDebugSession.new()
	session.setup(dev_tools)
	return [dev_tools, session]


func test_view_requests_apply_to_the_live_world() -> void:
	var pair := _make_pair()
	var dev_tools: DevTools = pair[0]
	var session: AiDebugSession = pair[1]
	var world := _make_world()
	add_child_autofree(world)
	session.begin_world(world)

	assert_false(world.is_ai_debug(), "the overlay starts off")
	dev_tools.ai_view_request.emit(&"overlay", true)
	assert_true(world.is_ai_debug(), "the master toggle builds the view")
	assert_not_null(world.get_node_or_null("AiDebug"), "the view node exists")

	dev_tools.ai_view_request.emit(&"routes", false)
	assert_false(bool(world.get_ai_view_state().get("routes", true)),
			"an element toggle lands in the retained state")
	assert_true(bool(world.get_ai_view_state().get("labels", false)),
			"the other elements keep their defaults")

	dev_tools.ai_view_request.emit(&"overlay", false)
	assert_false(world.is_ai_debug(), "the master toggle frees the view")
	assert_null(world.get_node_or_null("AiDebug"))
	await get_tree().process_frame


func test_requests_without_a_world_are_dropped() -> void:
	var pair := _make_pair()
	var dev_tools: DevTools = pair[0]
	var session: AiDebugSession = pair[1]

	# No world yet: nothing to apply to, nothing crashes.
	dev_tools.ai_view_request.emit(&"overlay", true)

	# A freed world: the liveness guard drops the request.
	var world := _make_world()
	add_child(world)
	session.begin_world(world)
	world.free()
	dev_tools.ai_view_request.emit(&"overlay", true)
	# Clearing the world detaches the provider without erroring.
	session.begin_world(null)
	dev_tools.ai_view_request.emit(&"labels", false)
	pass_test("no crash applying toggles without a live world")


func test_pick_session_owns_and_forwards_the_ai_session() -> void:
	var dev_tools := DevTools.new()
	add_child_autofree(dev_tools)
	var picks := DebugPickSession.new()
	picks.setup(dev_tools)
	var world := _make_world()
	add_child_autofree(world)
	picks.begin_world(world)

	dev_tools.ai_view_request.emit(&"overlay", true)
	assert_true(world.is_ai_debug(),
			"the pick session's owned AI session wired the new world")
	dev_tools.ai_view_request.emit(&"overlay", false)
	await get_tree().process_frame
