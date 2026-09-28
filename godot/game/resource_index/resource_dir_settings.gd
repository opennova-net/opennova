class_name ResourceDirSettings
extends RefCounted

## Persisted game and expansion selection, and the retail install the bundled
## menu's PLAY RETAIL picked (ADR 0048). A --resource-dir is never persisted.

const CONFIG_PATH := "user://opennova.cfg"
const SECTION := "resources"
const EXPANSION_KEY := "expansion"
const GAME_KEY := "game"
const RETAIL_DIR_KEY := "retail_dir"


## The persisted expansion name (e.g. "jox01"), or "" for the base game.
static func get_expansion() -> String:
	return String(ConfigStore.read(CONFIG_PATH, SECTION, EXPANSION_KEY, ""))


## Persist the expansion name, preserving any other sections in the config.
static func set_expansion(name: String) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, EXPANSION_KEY, name.strip_edges())


## The persisted game code (e.g. "jodemo"), or "jo" when unset. Selects the SCR decode
## key. A `/game` launch flag overrides this (see LaunchFlags.game).
static func get_game() -> String:
	var code := String(ConfigStore.read(
			CONFIG_PATH, SECTION, GAME_KEY, "jo")).strip_edges().to_lower()
	return code if not code.is_empty() else "jo"


## Persist the game code, preserving any other sections in the config.
static func set_game(code: String) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, GAME_KEY, code.strip_edges().to_lower())


## The retail install PLAY RETAIL last mounted, or "" when unset. A saved
## directory that no longer holds a game install is forgotten on read (the
## persisted value is cleared), so a stale pick never comes back; the shell
## also clears a saved install that fails to mount (MainGame.play_retail).
static func get_retail_dir() -> String:
	var dir := String(ConfigStore.read(CONFIG_PATH, SECTION, RETAIL_DIR_KEY, "")).strip_edges()
	if dir.is_empty() or ResourceRoot.is_valid_root(dir):
		return dir
	set_retail_dir("")
	return ""


## Persist the picked retail install, preserving any other sections in the config.
static func set_retail_dir(dir: String) -> void:
	ConfigStore.write(CONFIG_PATH, SECTION, RETAIL_DIR_KEY, dir.strip_edges())
