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
## does (event positions are frame-local, the space process_mouse expects).
## The caller gates on its own open state and driver before calling.
static func forward_gui_input(event: InputEvent, driver: MenuDriver, frame: MenuFrame) -> void:
	if event is InputEventMouseMotion:
		var motion := event as InputEventMouseMotion
		driver.process_mouse(motion.position,
				(motion.button_mask & MOUSE_BUTTON_MASK_LEFT) != 0)
	elif event is InputEventMouseButton:
		var button := event as InputEventMouseButton
		if button.button_index == MOUSE_BUTTON_LEFT:
			driver.process_mouse(button.position, button.pressed)
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
