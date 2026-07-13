class_name EditorResourceLibrary
extends RefCounted

## Single home for the OpenNova Editor's resource index + configured root
## directory, plus the persistence / scan / validation logic behind the
## resource browser and the settings popup. Extracted from
## editor_workstation.gd (B5-3a) so the shell is the thin UI layer over it.
##
## The shell keeps what stays a shell concern and forwards here:
##   - the settings-popup sync and status-bar messages.
## Those side effects are surfaced through the return values below
## ({err, status}) so this helper never reaches back into the UI.

const OnedSettings := preload("res://modtools/editor/oned_settings.gd")

var _index: RefCounted
var _resource_root: NovaResourceRoot = NovaResourceRoot.new()
var _root_dir: String = ""
var _reference_index: NovaReferenceIndex


func ensure_index() -> void:
	if _index == null:
		_index = NovaResourceIndex.new()


func get_index() -> RefCounted:
	ensure_index()
	return _index


# The whole-editor reference index (libs/refs over the mounted root): one
# instance per library so every link widget / referrers panel shares the same
# lazily-built graph. It self-invalidates against the root's cache epoch, so a
# rescan or directory change never serves stale edges.
func get_reference_index() -> NovaReferenceIndex:
	if _reference_index == null:
		_reference_index = NovaReferenceIndex.new()
		_reference_index.set_resource_root(_resource_root)
	return _reference_index


func get_root_dir() -> String:
	return _root_dir


func get_resource_root() -> NovaResourceRoot:
	return _resource_root


func get_loose_expansions() -> PackedStringArray:
	if _root_dir.is_empty():
		return PackedStringArray()
	return _resource_root.list_loose_expansions(_root_dir)


func clear_index() -> void:
	ensure_index()
	_index.clear()


# Updates the configured flat root and (optionally) persists + scans it. Returns
# a result the shell applies; `status` is a status-bar message to show (empty = none).
# The editor mounts loose files only (set_root_dir); the PFF archives are a runtime concern.
func set_root_dir(path: String, persist: bool, scan: bool) -> Dictionary:
	ensure_index()
	var previous := _root_dir
	_root_dir = path.strip_edges()
	if _root_dir.is_empty():
		_index.clear()
		_resource_root.clear()
		if persist:
			save_state()
		return {"err": OK, "status": ""}
	var root_err := _resource_root.set_root_dir(_root_dir)
	if root_err != OK:
		var root_error_message := _resource_root.get_last_error()
		_root_dir = previous
		if _root_dir.is_empty():
			_resource_root.clear()
		else:
			_resource_root.set_root_dir(_root_dir)
		_index.clear()
		return {"err": root_err, "status": root_error_message}
	if persist:
		save_state()
	if scan:
		return scan_root()
	if previous != _root_dir:
		_index.clear()
	return {"err": OK, "status": ""}


# Scans the configured root into the index. Returns {err, status}; `status` is
# the message the caller should surface (empty when there is nothing to say).
func scan_root() -> Dictionary:
	ensure_index()
	if _root_dir.is_empty():
		_index.clear()
		return {"err": OK, "status": ""}
	var root_err := _resource_root.set_root_dir(_root_dir)
	if root_err != OK:
		_index.clear()
		return {"err": root_err, "status": _resource_root.get_last_error()}
	var err: Error = _index.scan(_root_dir)
	if err == OK:
		return {"err": OK, "status": "Resource directory indexed."}
	var detail := ""
	if _index.has_method("get_last_error"):
		detail = String(_index.get_last_error())
	return {"err": err, "status": "Resource scan failed." if detail.is_empty() else detail}


# Loads ONED's persisted authoring root. NovaOnedSettings drops stale or invalid
# paths so the browser starts clean rather than pointing at a dead directory.
func load_state() -> Dictionary:
	_root_dir = OnedSettings.get_resource_dir()
	if _root_dir.is_empty():
		_resource_root.clear()
	else:
		_resource_root.set_root_dir(_root_dir)
	return {"root_dir": _root_dir}


func save_state() -> void:
	OnedSettings.set_resource_dir(_root_dir)


# The shell reaches recent directories through the library while their
# persistence remains owned by NovaOnedSettings.
func get_recent_dirs() -> PackedStringArray:
	return OnedSettings.get_recent_dirs()


func clear_recent_dirs() -> void:
	OnedSettings.clear_recent_dirs()


func canonical_key(path: String) -> String:
	return OnedSettings.canonical_key(path)


# Persisted shell layout. Split offsets are stored alongside the resource state
# in the same config. Presence is reported explicitly (has_left/has_right)
# because a valid split offset can be negative, so no numeric value can stand in
# for "unset"; when a side is absent the shell keeps the scene default.
func load_layout_state() -> Dictionary:
	return OnedSettings.load_layout_state()


# Merge the split offsets into the existing config (load-then-set-then-save) so
# the resource section is preserved, mirroring save_state().
func save_layout_state(left_offset: int, right_offset: int) -> void:
	OnedSettings.save_layout_state(left_offset, right_offset)


# Persisted Resource Browser pane state (visibility + its split offset),
# separate from save_layout_state so that call keeps its pinned two-argument
# signature. Offset presence is explicit (offsets can be negative).
func load_browser_state() -> Dictionary:
	return OnedSettings.load_browser_state()


func save_browser_state(visible: bool, split_offset: int) -> void:
	OnedSettings.save_browser_state(visible, split_offset)


# Persisted detachable-panel state: per panel, whether it should open docked
# and the floating window's last rect. Rect presence is explicit (positions can
# be negative on multi-monitor setups); rect validity (still on a screen) is
# the caller's concern at apply time.
func load_panel_state(panel_id: String) -> Dictionary:
	return OnedSettings.load_panel_state(panel_id)


func save_panel_state(panel_id: String, docked: bool, rect: Rect2i) -> void:
	OnedSettings.save_panel_state(panel_id, docked, rect)


# Persisted 3D-preview guide visibility (grid / axes). Defaults to visible when
# unset. Stored in the shared editor-state config alongside layout/resource state.
func load_view_state() -> Dictionary:
	return OnedSettings.load_view_state()


# Merge the view options into the existing config (load-then-set-then-save) so the
# resource/layout sections are preserved, mirroring save_layout_state().
func save_view_state(grid_visible: bool, axes_visible: bool) -> void:
	OnedSettings.save_view_state(grid_visible, axes_visible)


func is_valid_root(path: String) -> bool:
	return OnedSettings.is_valid_root(path)
