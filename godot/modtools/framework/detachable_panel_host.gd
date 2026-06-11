class_name DetachablePanelHost
extends RefCounted
## The two-state pop-out machine behind a detachable panel: DOCKED (the content
## sits where the scene put it) or FLOATING (the content lives in a Window —
## native where the OS supports it, embedded otherwise). One content Control,
## one Window, one gesture each way: detach() floats it, the window's close
## button re-docks it at its original child index. Deliberately NOT a docking
## framework — no tabs, no drag-tear, no multi-panel windows.
##
## Content moves with Node.reparent(), which preserves owners, so %-unique-name
## lookups into the content keep working in both states. The window factory is
## injectable so headless tests can pin the configuration without an OS window
## (headless lacks native subwindows anyway — the same code path IS the
## embedded fallback).

signal floating_changed(floating: bool)

const CONTENT_MARGIN := 12

var _panel_id: StringName
var _title: String
var _min_size: Vector2i
var _window_factory: Callable
var _content: Control
var _dock_parent: Node
var _dock_index := -1
var _window_parent: Node
# save_state: func(docked: bool, rect: Rect2i) -> void. Fired on detach,
# re-dock, and save_now() — signal-driven, never polled.
var _save_state := Callable()
var _window: Window


func _init(panel_id: StringName, window_title: String, window_min_size: Vector2i,
		window_factory: Callable = Callable()) -> void:
	_panel_id = panel_id
	_title = window_title
	_min_size = window_min_size
	_window_factory = window_factory


## Captures the content's dock slot (parent + child index) so re-docking puts
## it back exactly where the scene had it.
func setup(content: Control, window_parent: Node, save_state: Callable = Callable()) -> void:
	_content = content
	_dock_parent = content.get_parent() if content != null else null
	_dock_index = content.get_index() if content != null else -1
	_window_parent = window_parent
	_save_state = save_state


func is_floating() -> bool:
	return _window != null and is_instance_valid(_window)


func get_window() -> Window:
	return _window if is_floating() else null


## Float the content in a window at `screen_rect` (a zero rect derives one from
## where the content currently sits). The rect is clamped onto a visible screen.
func detach(screen_rect: Rect2i = Rect2i()) -> void:
	if is_floating() or _content == null or not is_instance_valid(_content) \
			or _window_parent == null:
		return
	var rect := screen_rect
	if rect.size.x <= 0 or rect.size.y <= 0:
		rect = screen_rect_for(_content)
	rect.size = rect.size.max(_min_size)
	var fallback := screen_rect_for(_window_parent as Control) \
			if _window_parent is Control else screen_rect_for(_content)
	rect = clamp_rect_to_screens(rect, fallback)

	_window = _make_window()
	_window_parent.add_child(_window)
	var margin := _window.get_node("PanelWrap/ContentMargin") as MarginContainer
	_content.reparent(margin)
	_content.visible = true
	if _window.force_native:
		_window.position = rect.position
	else:
		# Embedded windows position relative to the embedding viewport.
		var base := Vector2i()
		if _window.is_inside_tree():
			base = _window.get_tree().root.position
		_window.position = rect.position - base
	_window.size = rect.size
	_window.show()
	_save(false, rect)
	floating_changed.emit(true)


## The one way back: close the window, return the content to its dock slot.
## Wired to the window's close button; callers may also force it (e.g. the
## panel's subject disappeared).
func redock() -> void:
	if not is_floating():
		return
	var rect := Rect2i(_window.position, _window.size)
	if _content != null and is_instance_valid(_content) \
			and _dock_parent != null and is_instance_valid(_dock_parent):
		_content.reparent(_dock_parent)
		_dock_parent.move_child(_content, clampi(_dock_index, 0, _dock_parent.get_child_count() - 1))
	var window := _window
	_window = null
	window.queue_free()
	_save(true, rect)
	floating_changed.emit(false)


## Raise/focus the floating window (the rail toggle's behavior while floating).
func focus_window() -> void:
	if is_floating():
		_window.grab_focus()


func set_window_title(title: String) -> void:
	if is_floating():
		_window.title = title


## Persist the current state explicitly (shell teardown while floating).
func save_now() -> void:
	if is_floating():
		_save(false, Rect2i(_window.position, _window.size))


func _save(docked: bool, rect: Rect2i) -> void:
	if _save_state.is_valid():
		_save_state.call(docked, rect)


func _make_window() -> Window:
	var window: Window = null
	if _window_factory.is_valid():
		window = _window_factory.call(_title, _min_size) as Window
	if window == null:
		window = Window.new()
		window.title = _title
		# Must be decided before the window enters the tree; it cannot flip
		# while visible. Headless/web lack native subwindows, so the same code
		# degrades to an embedded window there — that IS the fallback.
		window.force_native = native_windows_supported()
		window.min_size = _min_size
		window.transient = true
		window.visible = false
	window.name = "%sPanelWindow" % String(_panel_id).capitalize()
	# Native windows don't inherit the embedded theme chain; assign explicitly.
	var theme := _nearest_theme()
	if theme != null:
		window.theme = theme

	var panel := PanelContainer.new()
	panel.name = "PanelWrap"
	panel.set_anchors_preset(Control.PRESET_FULL_RECT)
	window.add_child(panel)
	var margin := MarginContainer.new()
	margin.name = "ContentMargin"
	margin.add_theme_constant_override("margin_left", CONTENT_MARGIN)
	margin.add_theme_constant_override("margin_top", CONTENT_MARGIN)
	margin.add_theme_constant_override("margin_right", CONTENT_MARGIN)
	margin.add_theme_constant_override("margin_bottom", CONTENT_MARGIN)
	panel.add_child(margin)
	window.close_requested.connect(redock)
	return window


func _nearest_theme() -> Theme:
	var node: Node = _content
	while node != null:
		if node is Control and (node as Control).theme != null:
			return (node as Control).theme
		node = node.get_parent()
	return null


static func native_windows_supported() -> bool:
	return DisplayServer.has_feature(DisplayServer.FEATURE_SUBWINDOWS)


## Nudge `rect` fully onto whichever screen it touches; when it touches none
## (stale multi-monitor state), land it near the fallback area instead.
static func clamp_rect_to_screens(rect: Rect2i, fallback: Rect2i) -> Rect2i:
	for i in DisplayServer.get_screen_count():
		var usable := DisplayServer.screen_get_usable_rect(i)
		if usable.size.x <= 0 or usable.size.y <= 0 or not usable.intersects(rect):
			continue
		var out := rect
		out.position.x = clampi(out.position.x, usable.position.x,
				maxi(usable.position.x, usable.end.x - out.size.x))
		out.position.y = clampi(out.position.y, usable.position.y,
				maxi(usable.position.y, usable.end.y - out.size.y))
		return out
	var moved := rect
	moved.position = fallback.position + Vector2i(48, 48)
	return moved


static func screen_rect_for(control: Control) -> Rect2i:
	if control == null or not control.is_inside_tree():
		return Rect2i()
	return Rect2i(Vector2i(control.get_screen_position()), Vector2i(control.size))
