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
const BROWSER_SPLIT_KEY := "browser_split_offset"
const BROWSER_VISIBLE_KEY := "browser_pane_visible"
# View options (3D preview guides), persisted in their own section.
const VIEW_STATE_SECTION := "view"
const GRID_VISIBLE_KEY := "grid_visible"
const AXES_VISIBLE_KEY := "axes_visible"
# Detachable panels: per-panel docked flag + floating rect ("<id>_docked",
# "<id>_rect"). Ids in use: "camera", "environment"; "assets" reserved.
const PANELS_STATE_SECTION := "panels"

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


# Loads the persisted root from the shared NovaResourceDirSettings.
# That helper drops a persisted root that no longer points at a real, sane resource
# directory (moved/deleted dirs, or stale temp/test paths that leaked into the
# shared state) so the browser shows a clean "no directory" state instead of a dead
# internal path. Returns {root_dir}.
func load_state() -> Dictionary:
	# Resource-dir persistence lives in NovaResourceDirSettings (shared with the
	# runtime); get_resource_dir() already drops stale/invalid paths.
	_root_dir = ResourceDirSettings.get_resource_dir()
	if _root_dir.is_empty():
		_resource_root.clear()
	else:
		_resource_root.set_root_dir(_root_dir)
	return {"root_dir": _root_dir}


func save_state() -> void:
	ResourceDirSettings.set_resource_dir(_root_dir)


# Recently used resource directories (shared with the runtime via
# NovaResourceDirSettings). The shell reaches this state only through the library,
# so these thin forwarders keep that boundary while the dropdown lives in the shell.
func get_recent_dirs() -> PackedStringArray:
	return ResourceDirSettings.get_recent_dirs()


func clear_recent_dirs() -> void:
	ResourceDirSettings.clear_recent_dirs()


func canonical_key(path: String) -> String:
	return ResourceDirSettings.canonical_key(path)


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


# Persisted Resource Browser pane state (visibility + its split offset),
# separate from save_layout_state so that call keeps its pinned two-argument
# signature. Offset presence is explicit (offsets can be negative).
func load_browser_state() -> Dictionary:
	var config := ConfigFile.new()
	if config.load(STATE_CONFIG_PATH) != OK:
		return {"visible": false, "has_split": false, "split": 0}
	return {
		"visible": bool(config.get_value(LAYOUT_STATE_SECTION, BROWSER_VISIBLE_KEY, false)),
		"has_split": config.has_section_key(LAYOUT_STATE_SECTION, BROWSER_SPLIT_KEY),
		"split": int(config.get_value(LAYOUT_STATE_SECTION, BROWSER_SPLIT_KEY, 0)),
	}


func save_browser_state(visible: bool, split_offset: int) -> void:
	var config := ConfigFile.new()
	config.load(STATE_CONFIG_PATH)
	config.set_value(LAYOUT_STATE_SECTION, BROWSER_VISIBLE_KEY, visible)
	config.set_value(LAYOUT_STATE_SECTION, BROWSER_SPLIT_KEY, split_offset)
	config.save(STATE_CONFIG_PATH)


# Persisted detachable-panel state: per panel, whether it should open docked
# and the floating window's last rect. Rect presence is explicit (positions can
# be negative on multi-monitor setups); rect validity (still on a screen) is
# the caller's concern at apply time.
func load_panel_state(panel_id: String) -> Dictionary:
	var config := ConfigFile.new()
	if config.load(STATE_CONFIG_PATH) != OK:
		return {"docked": true, "has_rect": false, "rect": Rect2i()}
	var rect_key := "%s_rect" % panel_id
	return {
		"docked": bool(config.get_value(PANELS_STATE_SECTION, "%s_docked" % panel_id, true)),
		"has_rect": config.has_section_key(PANELS_STATE_SECTION, rect_key),
		"rect": config.get_value(PANELS_STATE_SECTION, rect_key, Rect2i()) as Rect2i,
	}


func save_panel_state(panel_id: String, docked: bool, rect: Rect2i) -> void:
	var config := ConfigFile.new()
	config.load(STATE_CONFIG_PATH)
	config.set_value(PANELS_STATE_SECTION, "%s_docked" % panel_id, docked)
	config.set_value(PANELS_STATE_SECTION, "%s_rect" % panel_id, rect)
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
