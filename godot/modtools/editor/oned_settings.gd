class_name NovaOnedSettings
extends RefCounted

## ONED-product persistence. Authoring state lives only in this file and is
## never read by the exported game.

const CONFIG_PATH := "user://oned_settings.cfg"
const RESOURCES_SECTION := "resources"
const RESOURCE_DIR_KEY := "resource_dir"
const EXPANSION_KEY := "expansion"
const RECENT_DIRS_KEY := "recent_dirs"
const RECENT_LIMIT := 8
const LAYOUT_SECTION := "layout"
const LEFT_SPLIT_KEY := "left_split_offset"
const RIGHT_SPLIT_KEY := "right_split_offset"
const BROWSER_SPLIT_KEY := "browser_split_offset"
const BROWSER_VISIBLE_KEY := "browser_pane_visible"
const VIEW_SECTION := "view"
const GRID_VISIBLE_KEY := "grid_visible"
const AXES_VISIBLE_KEY := "axes_visible"
const PANELS_SECTION := "panels"
const PATHS_SECTION := "paths"
const LAST_OPEN_DIR_KEY := "last_open_dir"
const LAST_SAVE_DIR_KEY := "last_save_dir"
const LAST_EXPORT_DIR_KEY := "last_export_dir"
const MCP_SECTION := "mcp"
const MCP_ENABLED_KEY := "enabled"
const MCP_PORT_KEY := "port"
const DEFAULT_MCP_PORT := 8975


static func get_resource_dir() -> String:
	var config := ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		return ""
	var path := String(config.get_value(RESOURCES_SECTION, RESOURCE_DIR_KEY, ""))
	return path if is_valid_root(path) else ""


static func set_resource_dir(path: String) -> void:
	var config := ConfigFile.new()
	config.load(CONFIG_PATH)
	var clean := path.strip_edges()
	config.set_value(RESOURCES_SECTION, RESOURCE_DIR_KEY, clean)
	if not clean.is_empty() and is_valid_root(clean):
		_merge_recent(config, clean)
	var selected := String(config.get_value(RESOURCES_SECTION, EXPANSION_KEY, ""))
	config.set_value(RESOURCES_SECTION, EXPANSION_KEY, _valid_expansion(clean, selected))
	config.save(CONFIG_PATH)


static func get_expansion() -> String:
	var config := ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		return ""
	return String(config.get_value(RESOURCES_SECTION, EXPANSION_KEY, "")).strip_edges()


static func set_expansion(name: String) -> void:
	var config := ConfigFile.new()
	config.load(CONFIG_PATH)
	config.set_value(RESOURCES_SECTION, EXPANSION_KEY, name.strip_edges())
	config.save(CONFIG_PATH)


static func is_valid_root(path: String) -> bool:
	return NovaResourceRoot.is_valid_root(path)


static func get_recent_dirs() -> PackedStringArray:
	var config := ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		return PackedStringArray()
	return _sanitize_recent(config.get_value(RESOURCES_SECTION, RECENT_DIRS_KEY, PackedStringArray()))


static func add_recent_dir(path: String) -> void:
	var clean := path.strip_edges()
	if clean.is_empty() or not is_valid_root(clean):
		return
	var config := ConfigFile.new()
	config.load(CONFIG_PATH)
	_merge_recent(config, clean)
	config.save(CONFIG_PATH)


static func clear_recent_dirs() -> void:
	var config := ConfigFile.new()
	config.load(CONFIG_PATH)
	config.set_value(RESOURCES_SECTION, RECENT_DIRS_KEY, PackedStringArray())
	config.save(CONFIG_PATH)


static func load_layout_state() -> Dictionary:
	var config := ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		return {"has_left": false, "left": 0, "has_right": false, "right": 0}
	return {
		"has_left": config.has_section_key(LAYOUT_SECTION, LEFT_SPLIT_KEY),
		"left": int(config.get_value(LAYOUT_SECTION, LEFT_SPLIT_KEY, 0)),
		"has_right": config.has_section_key(LAYOUT_SECTION, RIGHT_SPLIT_KEY),
		"right": int(config.get_value(LAYOUT_SECTION, RIGHT_SPLIT_KEY, 0)),
	}


static func save_layout_state(left_offset: int, right_offset: int) -> void:
	var config := ConfigFile.new()
	config.load(CONFIG_PATH)
	config.set_value(LAYOUT_SECTION, LEFT_SPLIT_KEY, left_offset)
	config.set_value(LAYOUT_SECTION, RIGHT_SPLIT_KEY, right_offset)
	config.save(CONFIG_PATH)


static func load_browser_state() -> Dictionary:
	var config := ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		return {"visible": false, "has_split": false, "split": 0}
	return {
		"visible": bool(config.get_value(LAYOUT_SECTION, BROWSER_VISIBLE_KEY, false)),
		"has_split": config.has_section_key(LAYOUT_SECTION, BROWSER_SPLIT_KEY),
		"split": int(config.get_value(LAYOUT_SECTION, BROWSER_SPLIT_KEY, 0)),
	}


static func save_browser_state(visible: bool, split_offset: int) -> void:
	var config := ConfigFile.new()
	config.load(CONFIG_PATH)
	config.set_value(LAYOUT_SECTION, BROWSER_VISIBLE_KEY, visible)
	config.set_value(LAYOUT_SECTION, BROWSER_SPLIT_KEY, split_offset)
	config.save(CONFIG_PATH)


static func load_panel_state(panel_id: String) -> Dictionary:
	var config := ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		return {"docked": true, "has_rect": false, "rect": Rect2i()}
	var rect_key := "%s_rect" % panel_id
	return {
		"docked": bool(config.get_value(PANELS_SECTION, "%s_docked" % panel_id, true)),
		"has_rect": config.has_section_key(PANELS_SECTION, rect_key),
		"rect": config.get_value(PANELS_SECTION, rect_key, Rect2i()) as Rect2i,
	}


static func save_panel_state(panel_id: String, docked: bool, rect: Rect2i) -> void:
	var config := ConfigFile.new()
	config.load(CONFIG_PATH)
	config.set_value(PANELS_SECTION, "%s_docked" % panel_id, docked)
	config.set_value(PANELS_SECTION, "%s_rect" % panel_id, rect)
	config.save(CONFIG_PATH)


static func load_view_state() -> Dictionary:
	var config := ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		return {"grid": true, "axes": true}
	return {
		"grid": bool(config.get_value(VIEW_SECTION, GRID_VISIBLE_KEY, true)),
		"axes": bool(config.get_value(VIEW_SECTION, AXES_VISIBLE_KEY, true)),
	}


static func save_view_state(grid_visible: bool, axes_visible: bool) -> void:
	var config := ConfigFile.new()
	config.load(CONFIG_PATH)
	config.set_value(VIEW_SECTION, GRID_VISIBLE_KEY, grid_visible)
	config.set_value(VIEW_SECTION, AXES_VISIBLE_KEY, axes_visible)
	config.save(CONFIG_PATH)


static func load_terrain_paths() -> Dictionary:
	var config := ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		return {"open": "", "save": "", "export": ""}
	return {
		"open": String(config.get_value(PATHS_SECTION, LAST_OPEN_DIR_KEY, "")),
		"save": String(config.get_value(PATHS_SECTION, LAST_SAVE_DIR_KEY, "")),
		"export": String(config.get_value(PATHS_SECTION, LAST_EXPORT_DIR_KEY, "")),
	}


static func save_terrain_paths(open_dir: String, save_dir: String, export_dir: String) -> void:
	var config := ConfigFile.new()
	config.load(CONFIG_PATH)
	config.set_value(PATHS_SECTION, LAST_OPEN_DIR_KEY, open_dir)
	config.set_value(PATHS_SECTION, LAST_SAVE_DIR_KEY, save_dir)
	config.set_value(PATHS_SECTION, LAST_EXPORT_DIR_KEY, export_dir)
	config.save(CONFIG_PATH)


static func get_mcp_enabled() -> bool:
	var config := ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		return true
	return bool(config.get_value(MCP_SECTION, MCP_ENABLED_KEY, true))


static func set_mcp_enabled(value: bool) -> void:
	var config := ConfigFile.new()
	config.load(CONFIG_PATH)
	config.set_value(MCP_SECTION, MCP_ENABLED_KEY, value)
	config.save(CONFIG_PATH)


static func get_mcp_port() -> int:
	var config := ConfigFile.new()
	if config.load(CONFIG_PATH) != OK:
		return DEFAULT_MCP_PORT
	return clampi(int(config.get_value(MCP_SECTION, MCP_PORT_KEY, DEFAULT_MCP_PORT)), 1024, 65535)


static func set_mcp_port(value: int) -> void:
	var config := ConfigFile.new()
	config.load(CONFIG_PATH)
	config.set_value(MCP_SECTION, MCP_PORT_KEY, clampi(value, 1024, 65535))
	config.save(CONFIG_PATH)


static func canonical_key(path: String) -> String:
	return path.strip_edges().replace(String.chr(92), "/").rstrip("/").to_lower()


static func _valid_expansion(root_dir: String, selected: String) -> String:
	var wanted := selected.strip_edges()
	if wanted.is_empty() or not is_valid_root(root_dir):
		return ""
	for expansion in NovaResourceRoot.new().list_loose_expansions(root_dir):
		if expansion.to_lower() == wanted.to_lower():
			return expansion
	return ""


static func _merge_recent(config: ConfigFile, clean: String) -> void:
	var key := canonical_key(clean)
	var merged := PackedStringArray([clean])
	for entry in _sanitize_recent(
		config.get_value(RESOURCES_SECTION, RECENT_DIRS_KEY, PackedStringArray())
	):
		if canonical_key(entry) == key:
			continue
		merged.append(entry)
		if merged.size() >= RECENT_LIMIT:
			break
	config.set_value(RESOURCES_SECTION, RECENT_DIRS_KEY, merged)


static func _sanitize_recent(raw: Variant) -> PackedStringArray:
	var result := PackedStringArray()
	if not (raw is PackedStringArray or raw is Array):
		return result
	var seen := {}
	for value in raw:
		var path := String(value).strip_edges()
		if path.is_empty() or not is_valid_root(path):
			continue
		var key := canonical_key(path)
		if seen.has(key):
			continue
		seen[key] = true
		result.append(path)
		if result.size() >= RECENT_LIMIT:
			break
	return result
