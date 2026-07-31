class_name DetailDockMount
extends RefCounted

# The asset-dock detail-pane state machine, extracted verbatim from the Object
# workspace: lazy panel creation, conditional mount, and the load-bearing
# rebuild policy — a genuine mount change or a workflow switch rebuilds the
# pane's content (sync_mount), while a routine same-mount re-sync (the shell
# re-forwards the dock on EVERY editor-state sync, e.g. each time-of-day drag
# step) keeps the existing content unless it is empty or a different document
# is open (ensure_mounted; tearing down here was the TOD lag).
#
# Parameterized by three Callables so the workspace keeps its domain logic:
#   uses_detail()  -> bool   does the active workflow want the pane at all
#   build_content(box)       fill the freshly created margin+scroll+VBox column
#   content_key()  -> int    identity of the open document (instance id); a
#                            change forces a rebuild on routine re-mounts
#
# Mission and terrain keep their own dock shapes on purpose (persistent
# inspector delegation / scene instantiation) — this owns the conditional
# cached pattern only.

var _dock_name: StringName
var _box_name: StringName
var _uses_detail: Callable
var _build_content: Callable
var _content_key: Callable
var _separation: int

var _mount: Control
var _dock: Control
var _content_id: int = 0


func _init(dock_name: StringName, box_name: StringName, uses_detail: Callable,
		build_content: Callable, content_key: Callable, separation: int = 10) -> void:
	_dock_name = dock_name
	_box_name = box_name
	_uses_detail = uses_detail
	_build_content = build_content
	_content_key = content_key
	_separation = separation


## The workspace's set_asset_dock body: adopt the (possibly new) mount. A real
## mount change rebuilds; the same mount re-forwarded only re-ensures.
func set_mount(dock: Control) -> void:
	var mount_changed := dock != _mount
	_mount = dock
	if dock == null:
		free_dock()
		return
	if mount_changed:
		sync_mount()
	else:
		ensure_mounted()


## Workflow switch: (re)build for the new workflow, or drop the pane when the
## workflow has no detail.
func sync_mount() -> void:
	if not bool(_uses_detail.call()):
		free_dock()
		return
	_ensure_dock()
	rebuild()


## Routine re-sync: keep the pane parented without rebuilding, unless it is
## empty or the open document changed.
func ensure_mounted() -> void:
	if not bool(_uses_detail.call()):
		free_dock()
		return
	_ensure_dock()
	if _dock == null:
		return
	if _dock.get_child_count() == 0 or int(_content_key.call()) != _content_id:
		rebuild()


func rebuild() -> void:
	if not bool(_uses_detail.call()):
		free_dock()
		return
	if _dock == null or not is_instance_valid(_dock):
		return
	for child in _dock.get_children():
		_dock.remove_child(child)
		child.free()

	var margin := MarginContainer.new()
	for side in ["margin_left", "margin_top", "margin_right", "margin_bottom"]:
		margin.add_theme_constant_override(side, UiBox.PANEL_MARGIN)
	margin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_dock.add_child(margin)

	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	margin.add_child(scroll)

	var box := VBoxContainer.new()
	box.name = _box_name
	box.add_theme_constant_override("separation", _separation)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(box)

	_build_content.call(box)
	_content_id = int(_content_key.call())


func free_dock() -> void:
	_content_id = 0
	if _dock == null:
		return
	if is_instance_valid(_dock):
		var parent := _dock.get_parent()
		if parent != null:
			parent.remove_child(_dock)
		_dock.free()
	_dock = null


func _ensure_dock() -> void:
	if _mount == null or not bool(_uses_detail.call()):
		return
	if _dock == null or not is_instance_valid(_dock):
		_dock = PanelContainer.new()
		_dock.name = _dock_name
		_dock.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_dock.size_flags_vertical = Control.SIZE_EXPAND_FILL
	if _dock.get_parent() != _mount:
		var old_parent := _dock.get_parent()
		if old_parent != null:
			old_parent.remove_child(_dock)
		_mount.add_child(_dock)
