extends GutTest

# DebugPickSession: the shell's pick state (list, toast flow, click latch) and
# the one forward that makes a landed pick the F3 Entities window's selection
# through the DevTools seam. Headless: DevTools attaches no ImGui context, but
# its selection state works (release flavour: compiled out, skipped).


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


func test_click_policy_needs_a_world_and_latches_on_the_edge() -> void:
	var session := DebugPickSession.new()
	session.sync_click_policy(null, true)
	assert_false(session.is_click_active(), "no world: nothing latched")
