class_name OnedSettings
extends RefCounted

## Persisted settings for ONED. The running game has
## its own config; neither side reads or migrates the other's state.

const CONFIG_PATH := "user://oned.cfg"
const SECTION := ResourceDirSettings.SECTION
const DIR_KEY := ResourceDirSettings.DIR_KEY
const EXPANSION_KEY := ResourceDirSettings.EXPANSION_KEY
const GAME_KEY := ResourceDirSettings.GAME_KEY
const RECENT_KEY := "recent_dirs"
const RETAIL_DIR_KEY := "retail_dir"
const RECENT_LIMIT := 8


static func get_resource_dir() -> String:
	return String(ConfigStore.read(CONFIG_PATH, SECTION, DIR_KEY, "")).strip_edges()


static func set_resource_dir(path: String) -> void:
	var clean := path.strip_edges()
	ConfigStore.update(CONFIG_PATH, func(config: ConfigFile) -> void:
		config.set_value(SECTION, DIR_KEY, clean)
		if not clean.is_empty() and ResourceDirSettings.is_valid_root(clean):
			_merge_recent(config, clean))


static func get_expansion() -> String:
	return String(ConfigStore.read(CONFIG_PATH, SECTION, EXPANSION_KEY, ""))


static func set_expansion(name: String) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, EXPANSION_KEY, name.strip_edges())


static func get_game() -> String:
	var code := String(ConfigStore.read(CONFIG_PATH, SECTION, GAME_KEY, "jo"))
	code = code.strip_edges().to_lower()
	return code if not code.is_empty() else "jo"


static func set_game(code: String) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, GAME_KEY, code.strip_edges().to_lower())


static func get_retail_dir() -> String:
	return String(ConfigStore.read(CONFIG_PATH, SECTION, RETAIL_DIR_KEY, "")).strip_edges()


static func set_retail_dir(path: String) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, RETAIL_DIR_KEY, path.strip_edges())


static func get_recent_dirs() -> PackedStringArray:
	return _sanitize(ConfigStore.read(
			CONFIG_PATH, SECTION, RECENT_KEY, PackedStringArray()))


static func add_recent_dir(path: String) -> void:
	var clean := path.strip_edges()
	if clean.is_empty() or not ResourceDirSettings.is_valid_root(clean):
		return
	ConfigStore.update(CONFIG_PATH, func(config: ConfigFile) -> void:
		_merge_recent(config, clean))


static func clear_recent_dirs() -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, RECENT_KEY, PackedStringArray())


static func _merge_recent(config: ConfigFile, clean: String) -> void:
	var key := ResourceDirSettings.canonical_key(clean)
	var merged := PackedStringArray([clean])
	for entry in _sanitize(config.get_value(SECTION, RECENT_KEY, PackedStringArray())):
		if ResourceDirSettings.canonical_key(entry) == key:
			continue
		merged.append(entry)
		if merged.size() >= RECENT_LIMIT:
			break
	config.set_value(SECTION, RECENT_KEY, merged)


static func _sanitize(value: Variant) -> PackedStringArray:
	var out := PackedStringArray()
	var seen := {}
	var entries := PackedStringArray(
			value if value is PackedStringArray or value is Array else [])
	for entry in entries:
		var clean := String(entry).strip_edges()
		if clean.is_empty() or not ResourceDirSettings.is_valid_root(clean):
			continue
		var key := ResourceDirSettings.canonical_key(clean)
		if seen.has(key):
			continue
		seen[key] = true
		out.append(clean)
		if out.size() >= RECENT_LIMIT:
			break
	return out
