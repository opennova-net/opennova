extends GutTest

# The DevTools workspace-by-name seams the dev_tools_tour probe and MCP
# automation drive: the windows by title and the overlay layers by
# "Group/Label", opened and toggled as the menus do, headless (no ImGui
# context is needed to flip the flags). The release flavour lists nothing.


func test_windows_open_and_close_by_title() -> void:
	var dev_tools: DevTools = add_child_autofree(DevTools.new())
	var titles := dev_tools.window_titles()
	if titles.is_empty():
		pending("release flavour: the dev tools are compiled out")
		return
	for title in ["Game", "Stats", "Entities", "AI", "Script", "Player", "Render", "Particles",
			"Audio", "Net", "Log"]:
		assert_true(titles.has(title), "the '%s' window is registered" % title)
	assert_true(dev_tools.set_window_open("Net", true), "a window opens by title")
	assert_true(dev_tools.set_window_open("Net", false), "and closes")
	assert_false(dev_tools.set_window_open("Game", false), "the Game window cannot close")
	assert_false(dev_tools.set_window_open("No such window", true), "an unknown title is refused")


func test_overlays_toggle_by_name() -> void:
	var dev_tools: DevTools = add_child_autofree(DevTools.new())
	var names := dev_tools.overlay_names()
	if names.is_empty():
		pending("release flavour: the dev tools are compiled out")
		return
	for name in ["Entities/Selection", "Entities/Labels", "AI/Labels", "AI/Routes",
			"Collision/Rays", "Collision/Contacts", "Collision/Hit meshes"]:
		assert_true(names.has(name), "the '%s' overlay is registered" % name)
	assert_true(dev_tools.set_overlay_enabled("AI/Routes", true), "an overlay toggles by name")
	assert_eq(dev_tools.overlay_last_draw("AI/Routes"), Vector3i.ZERO,
			"an overlay that has not drawn reports an empty draw")
	assert_true(dev_tools.set_overlay_enabled("AI/Routes", false))
	assert_false(dev_tools.set_overlay_enabled("AI/Nothing", true), "an unknown name is refused")
	assert_eq(dev_tools.overlay_last_draw("AI/Nothing"), Vector3i(-1, -1, -1))
	assert_eq(dev_tools.status_text(), "", "no command has reported yet")
