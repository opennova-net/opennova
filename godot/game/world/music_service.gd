extends Node

## The autoload "MusicService": the node that holds the one MusicDirector (and thereby the music
## stream over the engine mixer) for the process. The one music context, its pair, its open
## order and the game context's seed are the engine's (runtime/audio/music_context.h, through
## the director's open_menu_context / open_game_context / stop_context); the menu shell and the
## game world both open their context through this one node.

var _director: MusicDirector = null


func _ready() -> void:
	_director = MusicDirector.new()
	_director.name = "MusicDirector"
	_director.auto_start = false
	add_child(_director)


func _exit_tree() -> void:
	stop_context()


func director() -> MusicDirector:
	return _director


## "", "menu" or "game": which context is open.
func current_context() -> String:
	return _director.get_context() if _director != null else ""


## The script the open context loaded (null with none).
func current_script() -> MusicScript:
	return _director.get_context_script() if _director != null else null


static func resolve_menu_music_pair(root: ResourceRoot) -> MusicPair:
	return MusicDirector.resolve_menu_music_pair(root)


static func resolve_game_music_pair(root: ResourceRoot) -> MusicPair:
	return MusicDirector.resolve_game_music_pair(root)


## The menu context over `root`; the overrides name a loose script or bank the root resolves
## (the shell's dev seams). True when the context runs.
func open_menu_context(root: ResourceRoot, script_override := "", bank_override := "") -> bool:
	return _director != null and _director.open_menu_context(root, script_override, bank_override)


## The game context at a mission start, with its variable seed.
func open_game_context(root: ResourceRoot) -> bool:
	return _director != null and _director.open_game_context(root)


func stop_context() -> void:
	if _director != null:
		_director.stop_context()


func set_var(idx: int, value: int) -> void:
	if _director != null:
		_director.set_var(idx, value)


func get_var(idx: int) -> int:
	return _director.get_var(idx) if _director != null else 0
