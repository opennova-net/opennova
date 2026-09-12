extends GutTest

# DebugPickSession: the shell's pick state (list, toast flow, click latch) and
# the one forward that makes a landed pick the F3 Entities window's selection
# through the DevTools seam. Headless: DevTools attaches no ImGui context, but
# its selection state works (release flavour: compiled out, skipped). The
# click policy is pinned on a real GameWorld: the catcher node the session
# installs under it is the observable.


func _catcher(world: GameWorld) -> PickClickCatcher:
	return world.get_node_or_null(DebugPickSession.PICK_CATCHER_NAME) as PickClickCatcher


func _pick(handle: int) -> DebugPickCard:
	var card := DebugPickCard.new()
	card.hit = true
	card.entity_handle = handle
	card.name = "thing"
	card.bms_id = 7
	card.position_godot = Vector3.ZERO
	card.bound_radius = 1.0
	card.tick = 1
	return card


func test_a_landed_pick_selects_its_entities_row() -> void:
	var dev_tools: DevTools = add_child_autofree(DevTools.new())
	var session := DebugPickSession.new()
	session.setup(dev_tools)
	session.setup(dev_tools)  # idempotent: one forward, never two
	if dev_tools.stats_row_ids().is_empty():
		pending("release flavour: the dev tools are compiled out")
		return
	assert_eq(dev_tools.selected_entity_handle(), -1, "nothing selected before a pick")
	assert_eq(session.list.add(_pick(0x3001)), 0)
	assert_eq(dev_tools.selected_entity_handle(), 0x3001,
			"the pick's handle became the Entities selection")
	assert_eq(session.list.add(_pick(0x1002)), 1)
	assert_eq(dev_tools.selected_entity_handle(), 0x1002, "the newest pick wins")
	session.list.add(_pick(0x3001))
	assert_eq(dev_tools.selected_entity_handle(), 0x3001, "a refreshed pick re-selects")


func test_click_policy_latches_on_the_edge_and_follows_a_new_world() -> void:
	var session := DebugPickSession.new()
	session.sync_click_policy(null, true)
	assert_false(session.is_click_active(), "no world: nothing latched")

	# Out of the tree: a bare GameWorld's _ready wants its scene siblings, and
	# the catcher install needs only the world node as a parent.
	var world: GameWorld = autofree(GameWorld.new())
	var presenter: LocalPlayerPresenter = autofree(LocalPlayerPresenter.new())
	world.set_local_view_presenter(presenter)
	session.sync_click_policy(world, true)
	var first := _catcher(world)
	assert_not_null(first, "the edge installs the catcher under the world")
	assert_eq(first.presenter(), presenter,
			"the catcher picks through the world's local view presenter")
	session.sync_click_policy(world, true)
	assert_eq(_catcher(world), first, "a repeat is a no-op")
	assert_true(session.is_click_active())
	session.sync_click_policy(world, false)
	assert_null(_catcher(world), "the falling edge removes it")
	assert_null(first.get_parent(), "...detaching it before its deferred destruction")
	session.sync_click_policy(world, false)
	assert_null(_catcher(world))

	# A new world under an active latch gets the catcher.
	session.sync_click_policy(world, true)
	var next_world: GameWorld = autofree(GameWorld.new())
	session.begin_world(next_world)
	assert_not_null(_catcher(next_world), "the catcher follows the latch onto the new world")
	session.sync_click_policy(next_world, false)
	var idle_world: GameWorld = autofree(GameWorld.new())
	session.begin_world(idle_world)
	assert_null(_catcher(idle_world), "an inactive latch installs no catcher")
	await get_tree().process_frame  # the detached catchers' queue_free lands
