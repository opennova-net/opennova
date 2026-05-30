class_name EditorResourceLibrary
extends RefCounted

## Single home for the OpenNova Editor's resource index + configured root
## directory, plus the persistence / scan / validation logic behind the
## resource browser and the settings popup. Extracted from
## editor_workstation.gd (B5-3a) so the shell is the thin UI layer over it.
##
## The shell keeps what stays a shell concern and forwards here:
##   - the recursive flag (a plain shell member the tests write directly),
##   - the settings-popup sync, status-bar messages, and VegAssets
##     search-root registration.
## Those side effects are surfaced through the return values below
## ({err, search_roots, status}) so this helper never reaches back into the UI.

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

var _index: RefCounted
var _root_dir: String = ""


func ensure_index() -> void:
	if _index == null:
		_index = NovaResourceIndex.new()


func get_index() -> RefCounted:
	ensure_index()
	return _index


func get_root_dir() -> String:
	return _root_dir


func clear_index() -> void:
	ensure_index()
	_index.clear()


# Updates the configured root and (optionally) persists + scans it. Returns a
# result the shell applies: `search_roots` is the VegAssets search-root list
# (empty when the root is cleared) and `status` is a status-bar message to
# show (empty = none).
func set_root_dir(path: String, recursive: bool, persist: bool, scan: bool) -> Dictionary:
	ensure_index()
	var previous := _root_dir
	_root_dir = path.strip_edges()
	if persist:
		save_state(recursive)
	if _root_dir.is_empty():
		_index.clear()
		return {"err": OK, "search_roots": [], "status": ""}
	if scan:
		var result := scan_root(recursive)
		return {"err": result["err"], "search_roots": [_root_dir], "status": result["status"]}
	if previous != _root_dir:
		_index.clear()
	return {"err": OK, "search_roots": [_root_dir], "status": ""}


# Scans the configured root into the index. Returns {err, status}; `status` is
# the message the caller should surface (empty when there is nothing to say).
func scan_root(recursive: bool) -> Dictionary:
	ensure_index()
	if _root_dir.is_empty():
		_index.clear()
		return {"err": OK, "status": ""}
	var err: Error = _index.scan(_root_dir, recursive)
	if err == OK:
		return {"err": OK, "status": "Resource directory indexed."}
	var detail := ""
	if _index.has_method("get_last_error"):
		detail = String(_index.get_last_error())
	return {"err": err, "status": "Resource scan failed." if detail.is_empty() else detail}


# Loads the persisted root + recursive flag from the shared NovaResourceDirSettings.
# That helper drops a persisted root that no longer points at a real, sane resource
# directory (moved/deleted dirs, or stale temp/test paths that leaked into the
# shared state) so the browser shows a clean "no directory" state instead of a dead
# internal path. Returns {root_dir, recursive}; the caller stores `recursive`.
func load_state() -> Dictionary:
	# Resource-dir persistence lives in NovaResourceDirSettings (shared with the
	# runtime); get_resource_dir() already drops stale/invalid paths.
	_root_dir = ResourceDirSettings.get_resource_dir()
	return {"root_dir": _root_dir, "recursive": ResourceDirSettings.get_recursive()}


func save_state(recursive: bool) -> void:
	ResourceDirSettings.set_resource_dir(_root_dir, recursive)


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


func is_valid_root(path: String) -> bool:
	return ResourceDirSettings.is_valid_root(path)
