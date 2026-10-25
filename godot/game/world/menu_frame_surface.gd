class_name MenuFrameSurface
extends RefCounted

## The compiled-menu surface plumbing the in-world presenters share
## (ArmoryPresenter, DeployScreenPresenter, EndRoundPresenter): the one
## open_surface prologue (parse the .mnu, build the MenuFrame + MenuAudio +
## MenuDriver stack over a foreign HUD parent, open the document), hiding the
## frame on close, fitting the MenuFrame to its layout source, forwarding the
## frame's gui input to the MenuDriver, wiring the resize source, and loading
## the shell's stylesheets. Pure static helpers over the presenter's own typed
## members; the armory and deploy presenters keep a 1-line _recompute_fit so
## their resize signals have a bound Callable.


## One built stack: the two nodes parented under the ui parent and the
## RefCounted driver over them. The presenter adopts the three members.
class Surface extends RefCounted:
	var frame: MenuFrame = null
	var audio: MenuAudio = null
	var driver: MenuDriver = null


## The presenters' shared menu prologue: parse `menu_file` from the root, build
## the stack (`node_name` names the frame), hand the built driver to
## `before_open` (the presenter's text-table registration and signal wiring,
## both due before the screen shows), then open `screen`. Null when there is no
## root, and null with a warning under `owner_name` when the file is missing,
## does not parse or has no screens (the stack built for it is freed again);
## the presenter adopts the returned members.
static func open_surface(root: ResourceRoot, ui_parent: Node, layout_control: Control,
		menu_file: String, screen: String, node_name: String, owner_name: String,
		on_gui_input: Callable, before_open: Callable) -> Surface:
	if root == null:
		return null
	var doc := load_document(root, menu_file, owner_name)
	if doc == null:
		return null
	var surface := build(root, ui_parent, layout_control, node_name, on_gui_input)
	if before_open.is_valid():
		before_open.call(surface.driver)
	if open_document(surface.driver, doc, root, menu_file, screen, owner_name):
		return surface
	surface.frame.queue_free()
	surface.audio.queue_free()
	return null


## Hide the frame if it is showing; true when it was, so the presenter reports
## its close. Untyped on purpose: a frame freed with its HUD parent arrives as a
## freed instance, which a typed parameter would reject before the validity test.
static func hide_frame(frame: Variant) -> bool:
	if not is_instance_valid(frame):
		return false
	var menu_frame := frame as MenuFrame
	if menu_frame == null or not menu_frame.visible:
		return false
	menu_frame.visible = false
	return true


## Read and parse one .mnu from the resource root, else null with a warning
## under the owning presenter's name.
static func load_document(root: ResourceRoot, menu_file: String,
		owner_name: String) -> MnuDocument:
	var bytes := root.read_file(menu_file)
	if bytes.is_empty():
		push_warning("%s: %s not found in the resource root" % [owner_name, menu_file])
		return null
	var doc := MnuDocument.new()
	if doc.load_from_bytes(bytes) != OK:
		push_warning("%s: %s did not parse" % [owner_name, menu_file])
		return null
	return doc


## Build the stack under the ui parent: `node_name` names the frame,
## `node_name + "Audio"` the audio node. Every in-world .mnu shares the retail
## menu's fixed 800x600 design space; the frame scales it to its OWN size
## internally, so the fit just sizes the Control.
## [orig: CUIScene_SetScreenScale @0x639480]
## The driver pushes each screen's authored MUSICVAR into the same menumus
## discriminator slot MenuShell uses (the witness [orig: UI_DispatchScreenEvent
## @ 0x54e6a0, store @ 0x54eff4 -> AudioVM_SetVariable(2, v)] lives at engine
## audio/music_policy.h kMenuMusicVarSlot).
static func build(root: ResourceRoot, ui_parent: Node, layout_control: Control,
		node_name: String, on_gui_input: Callable) -> Surface:
	var surface := Surface.new()
	surface.frame = MenuFrame.new()
	surface.frame.name = node_name
	surface.frame.set_anchors_preset(Control.PRESET_TOP_LEFT)
	# Unlike MenuShell (a Control parent sampling for a full-rect child frame),
	# the presenter overlays a foreign HUD parent, so the frame itself is the
	# input surface: its gui_input forwards into the driver's pump.
	surface.frame.mouse_filter = Control.MOUSE_FILTER_STOP
	surface.frame.gui_input.connect(on_gui_input)
	ui_parent.add_child(surface.frame)
	fit_frame(surface.frame, layout_control, ui_parent)
	# Widget <SOUND> triggers play through the MenuAudio device leg.
	surface.audio = MenuAudio.new()
	surface.audio.name = node_name + "Audio"
	surface.audio.set_resource_root(root)
	ui_parent.add_child(surface.audio)
	surface.driver = MenuDriver.new()
	surface.driver.attach(surface.frame, surface.audio)
	surface.driver.set_music_director(MusicService.director())
	surface.driver.set_music_var_index(MusicDirector.MENU_MUSIC_VAR_SLOT)
	return surface


## Open the parsed document on a built driver with the shell's stylesheets
## (menu_style.mns then brand.mns, as the game loads them) and the expansion's
## string table (the screen's own tables are its windows' TEXT_RSRC, which the
## frame loads). False (with a warning) when the document has no screens.
static func open_document(driver: MenuDriver, doc: MnuDocument, root: ResourceRoot,
		menu_file: String, screen: String, owner_name: String) -> bool:
	var style: MnsStyleSheet = MnsStyleSheet.load_shell(root)
	if driver.open_document(doc, root, style, Strings.get_override_table(), menu_file, screen):
		return true
	push_warning("%s: %s has no screens" % [owner_name, menu_file])
	return false


## Fit the frame to the layout control, else the ui parent's viewport. The
## frame maps the 800x600 design space to its own rect internally
## [orig: CUIScene_SetScreenScale @0x639480] — no Control scale math here.
static func fit_frame(frame: MenuFrame, layout_control: Control, ui_parent: Node) -> void:
	if frame == null or not is_instance_valid(frame):
		return
	var target_size := Vector2.ZERO
	if layout_control != null:
		target_size = layout_control.size
	elif ui_parent != null and ui_parent.get_viewport() != null:
		target_size = ui_parent.get_viewport().get_visible_rect().size
	if target_size.x <= 1.0 or target_size.y <= 1.0:
		return
	frame.position = Vector2.ZERO
	frame.size = target_size


## The compiled frame is a passive surface — it draws and hit-tests but never
## pumps input itself; forward its gui input to the driver the way MenuShell
## does: each mouse event as its message, as it arrives (event positions are
## frame-local, the space the driver expects). The pump is the presenter's,
## once a frame in its _process (MenuDriver.pump_mouse), as a match pumps its
## menu after the frame's messages (engine/runtime/menu/menu_runtime.h). The
## caller gates on its own open state and driver before calling.
static func forward_gui_input(event: InputEvent, driver: MenuDriver, frame: MenuFrame) -> void:
	if driver.take_mouse_event(event):
		frame.accept_event()


## Connect the resize source (the layout control's resized, else the ui
## parent viewport's size_changed) to the presenter's refit Callable, once.
static func connect_layout_source(layout_control: Control, ui_parent: Node,
		on_resize: Callable) -> void:
	if layout_control != null:
		if not layout_control.resized.is_connected(on_resize):
			layout_control.resized.connect(on_resize)
		return
	var viewport := ui_parent.get_viewport() if ui_parent != null else null
	if viewport != null and not viewport.size_changed.is_connected(on_resize):
		viewport.size_changed.connect(on_resize)


# --- MCP menu-driving seams (the game_menu tool) ------------------------------
# Over any built frame and its driver: the shell's menu and the presenters' surfaces
# answer game_menu through these alike. Positions are design space (800x600), scaled
# to the frame as its own input is.


static func design_to_local(frame: MenuFrame, design_pos: Vector2) -> Vector2:
	var size := frame.get_size()
	return Vector2(design_pos.x * size.x / MenuFrame.DESIGN_WIDTH,
			design_pos.y * size.y / MenuFrame.DESIGN_HEIGHT)


## Every widget the frame places, in its pre-order: the design rect, kind, text and
## item count, and a list's rows and selected row (-1 none), read by its NAME (the
## driver's doc-id space is not the frame's index space).
static func widget_rows(driver: MenuDriver, frame: MenuFrame) -> Array[Dictionary]:
	var rows: Array[Dictionary] = []
	for i in frame.widget_count():
		var rect := frame.widget_rect(i)
		var widget_name := frame.widget_name(i)
		var item_count := frame.item_count(i)
		var row := {
			"index": i,
			"name": widget_name,
			"kind": frame.widget_kind(i),
			"disabled": frame.is_widget_disabled(i),
			"rect": [rect.position.x, rect.position.y, rect.size.x, rect.size.y],
			"text": frame.get_widget_text(i),
			"items": item_count,
		}
		var id := driver.widget_id(widget_name)
		if item_count > 0 and id >= 0:
			var texts := PackedStringArray()
			for item in driver.item_count(id):
				texts.append(driver.item_text(id, item))
			row.merge({"rows": texts, "selected": driver.selected_row(id)})
		rows.append(row)
	return rows


## Press+release through the real pump at the named, enabled widget's design-rect
## center, so click activation follows the pump's claim rules; false for none.
static func press_widget(driver: MenuDriver, frame: MenuFrame, widget_name: String) -> bool:
	for i in frame.widget_count():
		if frame.widget_name(i) != widget_name or frame.is_widget_disabled(i):
			continue
		var local := design_to_local(frame, frame.widget_rect(i).get_center())
		driver.process_mouse(local, true)
		driver.process_mouse(local, false)
		return true
	return false


## One raw pump sample at design coords with the button held or not, as a motion is
## (the claim's hover and the window sounds it plays); the hit widget index, -1 for none.
static func pump_at(driver: MenuDriver, frame: MenuFrame, design_pos: Vector2, down: bool) -> int:
	var local := design_to_local(frame, design_pos)
	var hit := frame.hit_test(local)
	driver.process_mouse(local, down)
	return hit


## A raw pump press+release at design coords (list rows, combo popups, spin arrows);
## the hit widget index, -1 for none.
static func press_at(driver: MenuDriver, frame: MenuFrame, design_pos: Vector2) -> int:
	var hit := pump_at(driver, frame, design_pos, true)
	pump_at(driver, frame, design_pos, false)
	return hit


## A REAL left click at design coords: press and release fed through Godot's input
## dispatch at the frame's place on the canvas (mouse filters, companion mounts and GUI
## focus all apply), unlike the pump-direct ops. The events land on the next frame.
static func click_at(frame: MenuFrame, design_pos: Vector2) -> void:
	var window_pos := frame.get_global_transform_with_canvas() * design_to_local(frame, design_pos)
	for pressed in [true, false]:
		var click := InputEventMouseButton.new()
		click.button_index = MOUSE_BUTTON_LEFT
		click.pressed = pressed
		click.position = window_pos
		click.global_position = window_pos
		Input.parse_input_event(click)
