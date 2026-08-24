class_name ResourceDirSettings
extends RefCounted

## The RUNTIME's persisted settings: the asset directory it mounts (the on-disk folder of
## .3di models and their textures), plus the player preferences that belong to playing rather
## than authoring.
##
## ONED keeps its own ([OnedSettings], `user://oned.cfg`). The two shared one config until the
## editor and the game routinely pointed at different things — ONED authors the loose asset tree
## while the game runs a PACKED dir built from it, and sharing meant packing silently repointed
## the editor at its own output. Clean cut: neither reads the other's file, nothing is migrated.
##
## Lives under game/ (not modtools/) because the runtime export excludes modtools/*.

const CONFIG_PATH := "user://opennova.cfg"
const SECTION := "resources"
const DIR_KEY := "resource_dir"
const EXPANSION_KEY := "expansion"
const GAME_KEY := "game"
const PLAYER_SECTION := "player"
const CROSSHAIR_STYLE_KEY := "crosshair_style"


## The persisted resource directory, or "" when unset / no longer a valid dir.
static func get_resource_dir() -> String:
	var dir := String(ConfigStore.read(CONFIG_PATH, SECTION, DIR_KEY, ""))
	return dir if is_valid_root(dir) else ""


## Persist the resource directory, preserving any other sections in the config. A
## valid, non-empty directory is also recorded at the front of the recently used
## list (surfaced by the editor's "Recent directories…" dropdown); clearing ("")
## or an invalid path leaves that list untouched. This is the single seam both the
## editor (via resource_library.save_state) and the runtime (main_game) go
## through, so recording is automatic for both with one disk write.
static func set_resource_dir(path: String) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, DIR_KEY, path.strip_edges())


## The persisted expansion name (e.g. "jox01"), or "" for the base game. Not validated
## here (there is no dir context); resource_library.gd drops a name that no longer matches
## an expansion under the live root.
static func get_expansion() -> String:
	return String(ConfigStore.read(CONFIG_PATH, SECTION, EXPANSION_KEY, ""))


## Persist the expansion name, preserving any other sections in the config.
static func set_expansion(name: String) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, EXPANSION_KEY, name.strip_edges())


## The persisted game code (e.g. "jodemo"), or "jo" when unset. Selects the SCR decode
## key. A `/game` launch flag overrides this (see LaunchFlags.game).
static func get_game() -> String:
	var code := String(ConfigStore.read(CONFIG_PATH, SECTION, GAME_KEY, "jo")) 			.strip_edges().to_lower()
	return code if not code.is_empty() else "jo"


## Persist the game code, preserving any other sections in the config.
static func set_game(code: String) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, GAME_KEY, code.strip_edges().to_lower())


## The player's retail crosshair index (MIN = cross01.tga, MAX = cross25.tga;
## the range lives on the engine binding, HudOverlay.MIN/MAX_CROSSHAIR_STYLE).
## Clamp corrupt or out-of-range values so HUD asset lookup always stays inside
## the authored XHAIR_APPEARANCE table.
static func get_crosshair_style() -> int:
	return clampi(int(ConfigStore.read(
			CONFIG_PATH, PLAYER_SECTION, CROSSHAIR_STYLE_KEY,
			HudOverlay.MIN_CROSSHAIR_STYLE)),
		HudOverlay.MIN_CROSSHAIR_STYLE, HudOverlay.MAX_CROSSHAIR_STYLE)


## Persist the selected retail crosshair index, preserving every other setting.
static func set_crosshair_style(style: int) -> void:
	ConfigStore.write(CONFIG_PATH, PLAYER_SECTION, CROSSHAIR_STYLE_KEY,
		clampi(style, HudOverlay.MIN_CROSSHAIR_STYLE, HudOverlay.MAX_CROSSHAIR_STYLE))


## A resource library is a real asset directory on disk, never inside the app's
## own user-data dir (rejects leaked temp/test paths).
static func is_valid_root(path: String) -> bool:
	return ResourceRoot.is_valid_root(path)


## A case/slash-insensitive comparison key for a directory path. Mirrors the C++
## ResourceRoot::normalize_dir rule (which is not bound to GDScript) so
## "D:\Game", "D:/Game/" and "d:/game" collapse to one entry. Comparison only:
## the original stripped path is what gets stored and displayed.
static func canonical_key(path: String) -> String:
	return path.strip_edges().replace("\\", "/").rstrip("/").to_lower()
