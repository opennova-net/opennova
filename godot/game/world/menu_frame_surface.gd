class_name MenuFrameSurface
extends RefCounted

## The compiled-menu surface plumbing the in-world presenters share
## (ArmoryPresenter, DeployScreenPresenter, EndRoundPresenter): parsing the
## .mnu, building the MenuFrame + MenuAudio + MenuDriver stack over a foreign
## HUD parent, opening the document, fitting the MenuFrame to its layout
## source, forwarding the frame's gui input to the MenuDriver, wiring the
## resize source, and loading the .mns style. Pure static helpers over the
## presenter's own typed members; each presenter keeps its 1-line
## _recompute_fit so the resize signals have a bound Callable.

## The canonical menu stylesheet name the original engine looks for
## (MenuShell's default).
const STYLESHEET_FILE := "menu_style.mns"


## One built stack: the two nodes parented under the ui parent and the
## RefCounted driver over them. The presenter adopts the three members.
class Surface extends RefCounted:
	var frame: MenuFrame = null
	var audio: MenuAudio = null
	var driver: MenuDriver = null


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


## Open the parsed document on a built driver with the canonical stylesheet
## and the registered menutxt table. False (with a warning) when the document
## has no screens; the caller tears its surface down.
static func open_document(driver: MenuDriver, doc: MnuDocument, root: ResourceRoot,
		menu_file: String, screen: String, owner_name: String) -> bool:
	var style := load_style(root, STYLESHEET_FILE)
	var menu_text: RtxtStringFile = Strings.get_table(Strings.TABLE_MENUTXT)
	if driver.open_document(doc, root, style, menu_text, menu_file, screen):
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


## The runtime-valid .mns style sheet from the resource root, else null.
static func load_style(root: ResourceRoot, stylesheet_file: String) -> MnsStyleSheet:
	var bytes := root.read_file(stylesheet_file)
	if bytes.is_empty():
		return null
	var s := MnsStyleSheet.new()
	return s if s.load_from_bytes(bytes) == OK and s.is_runtime_valid() else null
