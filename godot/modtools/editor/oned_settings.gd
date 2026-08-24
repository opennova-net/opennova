class_name OnedSettings
extends RefCounted

## ONED's own persisted settings — deliberately a DIFFERENT file from the runtime's
## ([ResourceDirSettings], `user://opennova.cfg`).
##
## The two used to share one config so a directory picked in either app applied to both. That
## stops being helpful the moment the editor and the game are pointed at different things, which
## is now the normal case: ONED authors the loose asset tree while the game (or retail) runs a
## PACKED dir built from it. Sharing meant packing silently repointed the editor at its own
## output. Separate files, clean cut — nothing reads the other's config and nothing is migrated.
##
## The pure helpers ([method ResourceDirSettings.is_valid_root], [method
## ResourceDirSettings.canonical_key]) are still reused: they are stateless rules about paths,
## not settings, and duplicating them would be the actual mistake.

const CONFIG_PATH := "user://oned.cfg"
const SECTION := "resources"
const DIR_KEY := "resource_dir"
const EXPANSION_KEY := "expansion"
const GAME_KEY := "game"
const RECENT_KEY := "recent_dirs"
const RETAIL_DIR_KEY := "retail_dir"
const RECENT_LIMIT := 8


## The editor's mounted asset directory, or "" when unset / no longer a valid dir.
static func get_resource_dir() -> String:
	var dir := String(ConfigStore.read(CONFIG_PATH, SECTION, DIR_KEY, ""))
	return dir if ResourceDirSettings.is_valid_root(dir) else ""


## Persist the editor's asset directory. A valid, non-empty directory is also recorded at the
## front of the recently used list; clearing ("") or an invalid path leaves that list untouched.
static func set_resource_dir(path: String) -> void:
	var clean := path.strip_edges()
	ConfigStore.update(CONFIG_PATH, func(config: ConfigFile) -> void:
		config.set_value(SECTION, DIR_KEY, clean)
		if not clean.is_empty() and ResourceDirSettings.is_valid_root(clean):
			_merge_recent(config, clean))


## The editor's expansion name (e.g. "jox01"), or "" for the base game.
static func get_expansion() -> String:
	return String(ConfigStore.read(CONFIG_PATH, SECTION, EXPANSION_KEY, ""))


static func set_expansion(name: String) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, EXPANSION_KEY, name.strip_edges())


## The editor's game code (e.g. "jodemo"), or "jo" when unset. Selects the SCR decode key.
static func get_game() -> String:
	var code := String(ConfigStore.read(CONFIG_PATH, SECTION, GAME_KEY, "jo")).strip_edges().to_lower()
	return code if not code.is_empty() else "jo"


static func set_game(code: String) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, GAME_KEY, code.strip_edges().to_lower())


## The retail install used to PROVE authored assets, or "" when unset.
##
## Retail is the ORACLE, never a build dependency: it is the original consumer of these formats,
## so an asset it loads and renders is a correct asset. Play-in-Retail stages Jointops.exe /
## binkw32.dll / game.cfg from here into a packed dir and launches that; with nothing configured
## the action stays disabled. Editor-only by nature — the runtime has no use for it.
static func get_retail_dir() -> String:
	var dir := String(ConfigStore.read(CONFIG_PATH, SECTION, RETAIL_DIR_KEY, ""))
	return dir if not dir.is_empty() and DirAccess.dir_exists_absolute(dir) else ""


static func set_retail_dir(path: String) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, RETAIL_DIR_KEY, path.strip_edges())


## The recently used asset directories, most-recent first. Stale, deleted, or otherwise invalid
## entries are dropped on read (never written back), so callers only ever see real directories.
## Editor-only: the runtime mounts one directory and has no picker.
static func get_recent_dirs() -> PackedStringArray:
	return _sanitize(ConfigStore.read(CONFIG_PATH, SECTION, RECENT_KEY, PackedStringArray()))


## Record a directory at the front of the recently used list. Empty or invalid paths are ignored.
static func add_recent_dir(path: String) -> void:
	var clean := path.strip_edges()
	if clean.is_empty() or not ResourceDirSettings.is_valid_root(clean):
		return
	ConfigStore.update(CONFIG_PATH, func(config: ConfigFile) -> void:
		_merge_recent(config, clean))


## Forget every recently used directory, preserving any other sections.
static func clear_recent_dirs() -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, RECENT_KEY, PackedStringArray())


# Prepend `clean` to the recents array already loaded in `config`, dropping any existing entry
# that shares its canonical key and capping at RECENT_LIMIT. The existing tail is sanitized too,
# so each rewrite also trims stale entries from disk. The caller saves.
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


# Drop empty, duplicate, and no-longer-valid directories, preserving order.
static func _sanitize(value: Variant) -> PackedStringArray:
	var out := PackedStringArray()
	var seen := {}
	for entry in PackedStringArray(value if value is PackedStringArray or value is Array else []):
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
