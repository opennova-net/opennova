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

# Resource-dir config path/keys are shared with the runtime (game/main_game.gd)
# via engine/resource_index/resource_dir_settings.gd so a directory picked in
# either app is the same persisted value.
const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
# Layout state (split offsets) shares the same config file as the resource dir,
# but lives in its own section; the resource-dir section is owned by
# NovaResourceDirSettings (load_state/save_state delegate to it).
const STATE_CONFIG_PATH := ResourceDirSettings.CONFIG_PATH
const LAYOUT_STATE_SECTION := "layout"
const LEFT_SPLIT_KEY := "left_split_offset"
const RIGHT_SPLIT_KEY := "right_split_offset"
# View options (3D preview guides), persisted in their own section.
const VIEW_STATE_SECTION := "view"
const GRID_VISIBLE_KEY := "grid_visible"
const AXES_VISIBLE_KEY := "axes_visible"

var _index: RefCounted
var _resource_root: NovaResourceRoot = NovaResourceRoot.new()
var _root_dir: String = ""
# Active expansion name (e.g. "jox01"); "" mounts the base game. Threaded into both the
# resource root and the browser index so the whole editor sees the same override stack.
var _expansion: String = ""


func ensure_index() -> void:
	if _index == null:
		_index = NovaResourceIndex.new()


func get_index() -> RefCounted:
	ensure_index()
	return _index


func get_root_dir() -> String:
	return _root_dir


func get_expansion() -> String:
	return _expansion


func get_resource_root() -> NovaResourceRoot:
	return _resource_root


# Expansion names available under `path` (defaults to the configured root). Thin
# passthrough to the C++ root so the shell never touches NovaResourceRoot directly.
func list_expansions(path: String = "") -> PackedStringArray:
	var dir := path if not path.is_empty() else _root_dir
	if dir.is_empty():
		return PackedStringArray()
	return _resource_root.list_expansions(dir)


func clear_index() -> void:
	ensure_index()
	_index.clear()


# Updates the configured flat root and (optionally) persists + scans it. Returns
# a result the shell applies; `status` is a status-bar message to show (empty = none).
func set_root_dir(path: String, persist: bool, scan: bool, expansion: String = "") -> Dictionary:
	ensure_index()
	var previous := _root_dir
	var previous_expansion := _expansion
	_root_dir = path.strip_edges()
	_expansion = expansion.strip_edges()
	if _root_dir.is_empty():
		_expansion = ""
		_index.clear()
		_resource_root.clear()
		if persist:
			save_state()
		return {"err": OK, "status": ""}
	var root_err := _resource_root.mount_game(_root_dir, _expansion)
	if root_err != OK:
		var root_error_message := _resource_root.get_last_error()
		_root_dir = previous
		_expansion = previous_expansion
		if _root_dir.is_empty():
			_resource_root.clear()
		else:
			_resource_root.mount_game(_root_dir, _expansion)
		_index.clear()
		return {"err": root_err, "status": root_error_message}
	if persist:
		save_state()
	if scan:
		return scan_root()
	if previous != _root_dir or previous_expansion != _expansion:
		_index.clear()
	return {"err": OK, "status": ""}


# Scans the configured root into the index. Returns {err, status}; `status` is
# the message the caller should surface (empty when there is nothing to say).
func scan_root() -> Dictionary:
	ensure_index()
	if _root_dir.is_empty():
		_index.clear()
		return {"err": OK, "status": ""}
	var root_err := _resource_root.mount_game(_root_dir, _expansion)
	if root_err != OK:
		_index.clear()
		return {"err": root_err, "status": _resource_root.get_last_error()}
	var err: Error = _index.scan(_root_dir, _expansion)
	if err == OK:
		return {"err": OK, "status": "Resource directory indexed."}
	var detail := ""
	if _index.has_method("get_last_error"):
		detail = String(_index.get_last_error())
	return {"err": err, "status": "Resource scan failed." if detail.is_empty() else detail}


# Loads the persisted root from the shared NovaResourceDirSettings.
# That helper drops a persisted root that no longer points at a real, sane resource
# directory (moved/deleted dirs, or stale temp/test paths that leaked into the
# shared state) so the browser shows a clean "no directory" state instead of a dead
# internal path. Returns {root_dir}.
func load_state() -> Dictionary:
	# Resource-dir persistence lives in NovaResourceDirSettings (shared with the
	# runtime); get_resource_dir() already drops stale/invalid paths.
	_root_dir = ResourceDirSettings.get_resource_dir()
	_expansion = ResourceDirSettings.get_expansion()
	if _root_dir.is_empty():
		_expansion = ""
		_resource_root.clear()
	else:
		# Drop a persisted expansion that no longer exists under this root (game dir moved
		# or the expansion was removed) so we mount a clean base game instead of failing.
		if not _expansion.is_empty() and not _resource_root.list_expansions(_root_dir).has(_expansion):
			_expansion = ""
		_resource_root.mount_game(_root_dir, _expansion)
	return {"root_dir": _root_dir, "expansion": _expansion}


func save_state() -> void:
	ResourceDirSettings.set_resource_dir(_root_dir)
	ResourceDirSettings.set_expansion(_expansion)


# Persisted shell layout. Split offsets are stored alongside the resource state
# in the same config. Presence is reported explicitly (has_left/has_right)
# because a valid split offset can be negative, so no numeric value can stand in
# for "unset"; when a side is absent the shell keeps the scene default.
func load_layout_state() -> Dictionary:
	var config := ConfigFile.new()
	if config.load(STATE_CONFIG_PATH) != OK:
		return {"has_left": false, "left": 0, "has_right": false, "right": 0}
	return {
		"has_left": config.has_section_key(LAYOUT_STATE_SECTION, LEFT_SPLIT_KEY),
		"left": int(config.get_value(LAYOUT_STATE_SECTION, LEFT_SPLIT_KEY, 0)),
		"has_right": config.has_section_key(LAYOUT_STATE_SECTION, RIGHT_SPLIT_KEY),
		"right": int(config.get_value(LAYOUT_STATE_SECTION, RIGHT_SPLIT_KEY, 0)),
	}


# Merge the split offsets into the existing config (load-then-set-then-save) so
# the resource section is preserved, mirroring save_state().
func save_layout_state(left_offset: int, right_offset: int) -> void:
	var config := ConfigFile.new()
	config.load(STATE_CONFIG_PATH)
	config.set_value(LAYOUT_STATE_SECTION, LEFT_SPLIT_KEY, left_offset)
	config.set_value(LAYOUT_STATE_SECTION, RIGHT_SPLIT_KEY, right_offset)
	config.save(STATE_CONFIG_PATH)


# Persisted 3D-preview guide visibility (grid / axes). Defaults to visible when
# unset. Stored in the shared editor-state config alongside layout/resource state.
func load_view_state() -> Dictionary:
	var config := ConfigFile.new()
	if config.load(STATE_CONFIG_PATH) != OK:
		return {"grid": true, "axes": true}
	return {
		"grid": bool(config.get_value(VIEW_STATE_SECTION, GRID_VISIBLE_KEY, true)),
		"axes": bool(config.get_value(VIEW_STATE_SECTION, AXES_VISIBLE_KEY, true)),
	}


# Merge the view options into the existing config (load-then-set-then-save) so the
# resource/layout sections are preserved, mirroring save_layout_state().
func save_view_state(grid_visible: bool, axes_visible: bool) -> void:
	var config := ConfigFile.new()
	config.load(STATE_CONFIG_PATH)
	config.set_value(VIEW_STATE_SECTION, GRID_VISIBLE_KEY, grid_visible)
	config.set_value(VIEW_STATE_SECTION, AXES_VISIBLE_KEY, axes_visible)
	config.save(STATE_CONFIG_PATH)


func is_valid_root(path: String) -> bool:
	return ResourceDirSettings.is_valid_root(path)
