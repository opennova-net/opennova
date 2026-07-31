extends GutTest

# DetachablePanelMount: the two-state pop-out machine behind detachable panels.
# Headless runs lack native subwindows, so the default window takes the
# embedded path — which IS the production fallback, making the whole state
# machine pinnable without an OS window: reparent targets, dock-slot
# restoration, owner/%-name survival, the save-state contract, and rect
# clamping.

const MountScript := preload("res://modtools/framework/detachable_panel_mount.gd")


func _make_dock() -> Dictionary:
	# A scene-shaped dock: root -> box -> [before, content(with a deep child), after]
	var root: Control = add_child_autofree(Control.new())
	var box := VBoxContainer.new()
	box.name = "Box"
	root.add_child(box)
	var before := Label.new()
	before.name = "Before"
	box.add_child(before)
	var content := VBoxContainer.new()
	content.name = "PanelContent"
	box.add_child(content)
	var deep := Label.new()
	deep.name = "DeepLabel"
	content.add_child(deep)
	var after := Label.new()
	after.name = "After"
	box.add_child(after)
	# Owner + unique-name mirror how the workstation scene resolves popover
	# content: the production lookups ride %-names that must survive reparents.
	for node in [box, before, content, deep, after]:
		(node as Node).owner = root
	deep.unique_name_in_owner = true
	return {"root": root, "box": box, "content": content, "deep": deep}


func _make_mount(dock: Dictionary, saves: Array = []) -> DetachablePanelMount:
	var mount: DetachablePanelMount = MountScript.new(&"camera", "Camera", Vector2i(300, 200))
	mount.setup(dock["content"], dock["root"],
		func(docked: bool, rect: Rect2i) -> void: saves.append([docked, rect]))
	return mount


func test_detach_reparents_content_into_window() -> void:
	var dock := _make_dock()
	var mount := _make_mount(dock)
	var states: Array = []
	mount.floating_changed.connect(func(f: bool) -> void: states.append(f))

	mount.detach(Rect2i(100, 100, 400, 300))
	assert_true(mount.is_floating())
	assert_eq(states, [true])
	var content: Control = dock["content"]
	assert_eq(content.get_parent().name, "ContentMargin", "content lands in the window's margin")
	assert_eq(content.get_parent().get_parent().name, "PanelWrap")
	assert_true(content.get_parent().get_parent().get_parent() is Window,
		"the chain tops out at the Window")
	assert_false((dock["box"] as Node).get_children().has(content), "the dock slot empties")


func test_redock_restores_content_at_original_child_index() -> void:
	var dock := _make_dock()
	var mount := _make_mount(dock)
	var states: Array = []
	mount.floating_changed.connect(func(f: bool) -> void: states.append(f))
	mount.detach(Rect2i(100, 100, 400, 300))
	var window: Window = mount.get_window()

	mount.redock()
	assert_false(mount.is_floating())
	assert_eq(states, [true, false])
	var content: Control = dock["content"]
	assert_eq(content.get_parent(), dock["box"], "content returns to its dock parent")
	assert_eq(content.get_index(), 1, "...at its original slot (between Before and After)")
	assert_true(window.is_queued_for_deletion(), "the window is freed on re-dock")
	assert_null(mount.get_window())


func test_detach_uses_injected_window_factory() -> void:
	var dock := _make_dock()
	var made: Array = []
	var mount: DetachablePanelMount = MountScript.new(&"camera", "Camera", Vector2i(300, 200),
		func(title: String, min_size: Vector2i) -> Window:
			var w := Window.new()
			w.title = "FACTORY " + title
			w.min_size = min_size
			w.visible = false
			made.append(w)
			return w)
	mount.setup(dock["content"], dock["root"])
	mount.detach(Rect2i(0, 0, 320, 240))
	assert_eq(made.size(), 1, "the factory builds the window")
	assert_eq(mount.get_window().title, "FACTORY Camera")
	mount.redock()


func test_default_window_is_embedded_fallback_when_native_unsupported() -> void:
	# Headless display servers report no native-subwindow support; the default
	# factory must then produce an embedded window - the same degradation web
	# builds get. This is the executable record of the force_native spike.
	assert_false(MountScript.native_windows_supported(),
		"headless has no native subwindows (if this fails, the test runner has a real display)")
	var dock := _make_dock()
	var mount := _make_mount(dock)
	mount.detach(Rect2i(50, 50, 400, 300))
	var window: Window = mount.get_window()
	assert_false(window.force_native, "the fallback window stays embedded")
	assert_eq(window.title, "Camera")
	assert_eq(window.min_size, Vector2i(300, 200))
	assert_true(window.transient, "panel windows float above the shell")
	mount.redock()


func test_save_state_fires_on_detach_redock_and_save_now() -> void:
	var dock := _make_dock()
	var saves: Array = []
	var mount := _make_mount(dock, saves)

	mount.detach(Rect2i(100, 100, 400, 300))
	assert_eq(saves.size(), 1)
	assert_false(bool(saves[0][0]), "detach persists docked=false")

	mount.save_now()
	assert_eq(saves.size(), 2)
	assert_false(bool(saves[1][0]), "save_now while floating persists docked=false")

	mount.redock()
	assert_eq(saves.size(), 3)
	assert_true(bool(saves[2][0]), "re-dock persists docked=true")
	assert_true((saves[2][1] as Rect2i).size.x > 0, "...with the window's last rect")

	mount.save_now()
	assert_eq(saves.size(), 3, "save_now while docked has nothing to add (re-dock already saved)")


func test_forced_redock_never_overwrites_the_floating_preference() -> void:
	# Callers force a re-dock for transient reasons (the panel's subject
	# disappeared); that must not erase the user's deliberate pop-out.
	var dock := _make_dock()
	var saves: Array = []
	var mount := _make_mount(dock, saves)
	mount.detach(Rect2i(100, 100, 400, 300))
	assert_eq(saves.size(), 1)

	mount.redock(false)
	assert_false(mount.is_floating(), "the forced re-dock still re-docks")
	assert_eq(saves.size(), 1, "...but writes nothing (the floating preference survives)")


func test_offscreen_rect_moves_onto_usable_space() -> void:
	var fallback := Rect2i(0, 0, 1280, 720)
	var off := Rect2i(100000, 100000, 400, 300)
	var clamped: Rect2i = MountScript.clamp_rect_to_screens(off, fallback)
	assert_ne(clamped.position, off.position,
		"a rect on no visible screen is moved (stale multi-monitor state)")
	assert_eq(clamped.size, off.size, "only the position moves")


func test_unique_names_resolve_after_detach_redock_cycle() -> void:
	var dock := _make_dock()
	var mount := _make_mount(dock)
	var root: Control = dock["root"]
	assert_eq(root.get_node("%DeepLabel"), dock["deep"], "the %-lookup works docked")

	mount.detach(Rect2i(0, 0, 320, 240))
	assert_eq(root.get_node_or_null("%DeepLabel"), dock["deep"],
		"reparent() preserves owners, so %-lookups survive while floating")

	mount.redock()
	assert_eq(root.get_node_or_null("%DeepLabel"), dock["deep"], "...and after re-docking")
