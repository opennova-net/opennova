extends GutTest

# DebugPickSession: the shell's pick state (list, toast flow, click latch) and
# the one forward that makes a landed pick the F3 Entities window's selection
# through the DevTools seam. Headless: DevTools attaches no ImGui context, but
# its selection state works (release flavour: compiled out, skipped). The
# click policy is pinned against a typed GameWorld double that counts the
# catcher installs.


## Typed world double: IS a GameWorld whose debug views record the
## click-catcher policy (the session reaches them through debug_views()).
class PolicyViews:
	extends DebugViewSet
	var enabled_calls: Array[bool] = []
	var pick_lists: Array = []

	func set_pick_click_enabled(enabled: bool) -> void:
		enabled_calls.append(enabled)

	func set_pick_debug(pick_list: DebugPickList) -> void:
		pick_lists.append(pick_list)


class PolicyWorld:
	extends GameWorld
	var views := PolicyViews.new()

	var enabled_calls: Array[bool]:
		get:
			return views.enabled_calls

	var pick_lists: Array:
		get:
			return views.pick_lists

	func debug_views() -> DebugViewSet:
		return views


func _pick(handle: int) -> Dictionary:
	return {
		"hit": true,
		"entity_handle": handle,
		"name": "thing",
		"bms_id": 7,
		"position_godot": Vector3.ZERO,
		"bound_radius": 1.0,
		"tick": 1,
	}


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

	var world := PolicyWorld.new()
	autofree(world)
	session.sync_click_policy(world, true)
	session.sync_click_policy(world, true)
	assert_eq(world.enabled_calls, [true], "one install per transition, none on a repeat")
	assert_true(session.is_click_active())
	session.sync_click_policy(world, false)
	session.sync_click_policy(world, false)
	assert_eq(world.enabled_calls, [true, false], "one removal per transition")

	# A new world under an active latch gets the list AND the catcher.
	session.sync_click_policy(world, true)
	var next_world := PolicyWorld.new()
	autofree(next_world)
	session.begin_world(next_world)
	assert_eq(next_world.pick_lists, [session.list], "the new world renders the shell's list")
	assert_eq(next_world.enabled_calls, [true], "the catcher follows the latch onto the new world")
	session.sync_click_policy(next_world, false)
	var idle_world := PolicyWorld.new()
	autofree(idle_world)
	session.begin_world(idle_world)
	assert_eq(idle_world.enabled_calls, [], "an inactive latch installs no catcher")
