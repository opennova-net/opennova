class_name MenuFrameSurface
extends RefCounted

## The compiled-menu surface plumbing the in-world presenters share
## (ArmoryPresenter, DeployScreenPresenter, EndRoundPresenter): fitting the
## MenuFrame to its layout source, forwarding the frame's gui input to the
## MenuDriver, wiring the resize source, and loading the .mns style. Pure
## static helpers over the presenter's own typed members; each presenter keeps
## its 1-line _recompute_fit so the resize signals have a bound Callable.


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
