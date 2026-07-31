class_name DetachablePanelMount
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
	# keep_global_transform=false: the old viewport's coordinates must not
	# carry into the window's small viewport (the container re-lays out either
	# way, but the carried rect would survive until the deferred sort).
	_content.reparent(margin, false)
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
## Wired to the window's close button. Callers forcing a re-dock for reasons
## of their own (the panel's subject disappeared) pass persist=false so a
## TRANSIENT condition never overwrites the user's floating preference.
func redock(persist := true) -> void:
	if not is_floating():
		return
	var rect := _window_screen_rect()
	if _content != null and is_instance_valid(_content) \
			and _dock_parent != null and is_instance_valid(_dock_parent):
		_content.reparent(_dock_parent, false)
		_dock_parent.move_child(_content, clampi(_dock_index, 0, _dock_parent.get_child_count() - 1))
	var window := _window
	_window = null
	window.queue_free()
	if persist:
		_save(true, rect)
	floating_changed.emit(false)


## Raise/focus the floating window (the rail toggle's behavior while floating).
func focus_window() -> void:
	if is_floating():
		_window.grab_focus()


func set_window_title(title: String) -> void:
	# The change guard is load-bearing: callers push from per-frame state
	# refreshes, and Window.set_title has no same-value early-out (a native
	# window would take an OS call per frame).
	if is_floating() and _window.title != title:
		_window.title = title


## Persist the current state explicitly (shell teardown while floating).
func save_now() -> void:
	if is_floating():
		_save(false, _window_screen_rect())


# The window's rect in SCREEN coordinates regardless of mode: an embedded
# window's position is viewport-relative, the inverse of the offset detach()
# applied, so persisted rects stay in one coordinate space.
func _window_screen_rect() -> Rect2i:
	var pos := _window.position
	if not _window.force_native and _window.is_inside_tree():
		pos += _window.get_tree().root.position
	return Rect2i(pos, _window.size)


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


## Nudge `rect` fully onto the screen holding MOST of it (a window straddling
## two monitors lands on the bigger share, not whichever has the lower index);
## when it touches none (stale multi-monitor state), land it near the fallback.
static func clamp_rect_to_screens(rect: Rect2i, fallback: Rect2i) -> Rect2i:
	var best := Rect2i()
	var best_area := 0
	for i in DisplayServer.get_screen_count():
		var usable := DisplayServer.screen_get_usable_rect(i)
		if usable.size.x <= 0 or usable.size.y <= 0:
			continue
		var overlap := usable.intersection(rect)
		var area := overlap.size.x * overlap.size.y
		if area > best_area:
			best_area = area
			best = usable
	if best_area > 0:
		var out := rect
		out.position.x = clampi(out.position.x, best.position.x,
				maxi(best.position.x, best.end.x - out.size.x))
		out.position.y = clampi(out.position.y, best.position.y,
				maxi(best.position.y, best.end.y - out.size.y))
		return out
	var moved := rect
	moved.position = fallback.position + Vector2i(48, 48)
	return moved


static func screen_rect_for(control: Control) -> Rect2i:
	if control == null or not control.is_inside_tree():
		return Rect2i()
	return Rect2i(Vector2i(control.get_screen_position()), Vector2i(control.size))
