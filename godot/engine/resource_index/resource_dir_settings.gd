class_name NovaResourceDirSettings
extends RefCounted

## Shared persistence for the OpenNova resource/asset directory — the on-disk
## folder of original .3di models and their textures. The editor's resource
## browser (modtools/editor/resource_library.gd) and the runtime
## (game/main_game.gd) read/write the SAME user:// config, so a directory picked
## in either app is shared. This lives under engine/ (not modtools/) because the
## runtime export excludes modtools/* and still needs the path + keys.

const CONFIG_PATH := "user://terrain_editor_state.cfg"
const SECTION := "resources"
const DIR_KEY := "resource_dir"
const EXPANSION_KEY := "expansion"
const GAME_KEY := "game"
const RECENT_KEY := "recent_dirs"
const RECENT_LIMIT := 8
const PLAYER_SECTION := "player"
const CROSSHAIR_STYLE_KEY := "crosshair_style"
const CROSSHAIR_STYLE_MIN := 0
const CROSSHAIR_STYLE_MAX := 24


## The persisted resource directory, or "" when unset / no longer a valid dir.
static func get_resource_dir() -> String:
	var dir := String(NovaConfigStore.read(CONFIG_PATH, SECTION, DIR_KEY, ""))
	return dir if is_valid_root(dir) else ""


## Persist the resource directory, preserving any other sections in the config. A
## valid, non-empty directory is also recorded at the front of the recently used
## list (surfaced by the editor's "Recent directories…" dropdown); clearing ("")
## or an invalid path leaves that list untouched. This is the single seam both the
## editor (via resource_library.save_state) and the runtime (main_game) go
## through, so recording is automatic for both with one disk write.
static func set_resource_dir(path: String) -> void:
	var clean := path.strip_edges()
	NovaConfigStore.update(CONFIG_PATH, func(config: ConfigFile) -> void:
		config.set_value(SECTION, DIR_KEY, clean)
		if not clean.is_empty() and is_valid_root(clean):
			_merge_recent(config, clean))


## The persisted expansion name (e.g. "jox01"), or "" for the base game. Not validated
## here (there is no dir context); resource_library.gd drops a name that no longer matches
## an expansion under the live root.
static func get_expansion() -> String:
	return String(NovaConfigStore.read(CONFIG_PATH, SECTION, EXPANSION_KEY, ""))


## Persist the expansion name, preserving any other sections in the config.
static func set_expansion(name: String) -> void:
	NovaConfigStore.write(CONFIG_PATH, SECTION, EXPANSION_KEY, name.strip_edges())


## The persisted game code (e.g. "jodemo"), or "jo" when unset. Selects the SCR decode
## key. A `/game` launch flag overrides this (see NovaLaunchFlags.game).
static func get_game() -> String:
	var code := String(NovaConfigStore.read(CONFIG_PATH, SECTION, GAME_KEY, "jo")) 			.strip_edges().to_lower()
	return code if not code.is_empty() else "jo"


## Persist the game code, preserving any other sections in the config.
static func set_game(code: String) -> void:
	NovaConfigStore.write(CONFIG_PATH, SECTION, GAME_KEY, code.strip_edges().to_lower())


## The player's retail crosshair index (0 = cross01.tga, 24 = cross25.tga).
## Clamp corrupt or out-of-range values so HUD asset lookup always stays inside
## the authored XHAIR_APPEARANCE table.
static func get_crosshair_style() -> int:
	return clampi(int(NovaConfigStore.read(
			CONFIG_PATH, PLAYER_SECTION, CROSSHAIR_STYLE_KEY, CROSSHAIR_STYLE_MIN)),
		CROSSHAIR_STYLE_MIN, CROSSHAIR_STYLE_MAX)


## Persist the selected retail crosshair index, preserving every other setting.
static func set_crosshair_style(style: int) -> void:
	NovaConfigStore.write(CONFIG_PATH, PLAYER_SECTION, CROSSHAIR_STYLE_KEY,
		clampi(style, CROSSHAIR_STYLE_MIN, CROSSHAIR_STYLE_MAX))


## A resource library is a real asset directory on disk, never inside the app's
## own user-data dir (rejects leaked temp/test paths).
static func is_valid_root(path: String) -> bool:
	return NovaResourceRoot.is_valid_root(path)


## The recently used resource directories, most-recent first. Stale, deleted, or
## otherwise invalid entries are dropped on read (never written back), so the list
## a caller sees always points at real directories.
static func get_recent_dirs() -> PackedStringArray:
	return _sanitize(NovaConfigStore.read(CONFIG_PATH, SECTION, RECENT_KEY, PackedStringArray()))


## Record a directory at the front of the recently used list. Empty or invalid
## paths are ignored. Preserves any other sections in the config. (set_resource_dir
## already records on every successful apply; this is the explicit seam for callers
## that want to record without changing the active directory, and for tests.)
static func add_recent_dir(path: String) -> void:
	var clean := path.strip_edges()
	if clean.is_empty() or not is_valid_root(clean):
		return
	NovaConfigStore.update(CONFIG_PATH, func(config: ConfigFile) -> void:
		_merge_recent(config, clean))


## Forget every recently used directory, preserving any other sections.
static func clear_recent_dirs() -> void:
	NovaConfigStore.write(CONFIG_PATH, SECTION, RECENT_KEY, PackedStringArray())


## A case/slash-insensitive comparison key for a directory path. Mirrors the C++
## NovaResourceRoot::normalize_dir rule (which is not bound to GDScript) so
## "D:\Game", "D:/Game/" and "d:/game" collapse to one entry. Comparison only:
## the original stripped path is what gets stored and displayed.
static func canonical_key(path: String) -> String:
	return path.strip_edges().replace("\\", "/").rstrip("/").to_lower()


# Prepend `clean` to the recents array already loaded in `config`, dropping any
# existing entry that shares its canonical key and capping at RECENT_LIMIT. The
# existing tail is sanitized too, so each rewrite also trims stale entries from
# disk. The caller saves.
static func _merge_recent(config: ConfigFile, clean: String) -> void:
	var key := canonical_key(clean)
	var merged := PackedStringArray([clean])
	for text in _sanitize(config.get_value(SECTION, RECENT_KEY, PackedStringArray())):
		if canonical_key(text) == key:
			continue
		merged.append(text)
		if merged.size() >= RECENT_LIMIT:
			break
	config.set_value(SECTION, RECENT_KEY, merged)


# Drop empty, invalid, and duplicate entries (keeping most-recent-first order),
# capped at RECENT_LIMIT. Defensive against a corrupt / non-array stored value.
static func _sanitize(raw: Variant) -> PackedStringArray:
	var out := PackedStringArray()
	if not (raw is PackedStringArray or raw is Array):
		return out
	var seen := {}
	for entry in raw:
		var text := String(entry).strip_edges()
		if text.is_empty() or not is_valid_root(text):
			continue
		var key := canonical_key(text)
		if seen.has(key):
			continue
		seen[key] = true
		out.append(text)
		if out.size() >= RECENT_LIMIT:
			break
	return out
