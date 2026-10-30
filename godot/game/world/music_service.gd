extends Node

## The autoload "MusicService": the node that holds the one MusicDirector (and thereby its
## AudioStreamPlayer pool) for the process. The one music context, its pair, its open order and
## the game context's seed are the engine's (runtime/audio/music_context.h, through the
## director's open_menu_context / open_game_context / stop_context); the menu shell and the game
## world both open their context through this one node.

# The longest a quit waits for the mixer to release stopped playbacks; the
# WASAPI driver retries a lost endpoint on about this cadence, and a live
# mixer drains within a few frames.
const PLAYBACK_DRAIN_TIMEOUT_MSEC := 1000

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


## Waits while a track still streams onto the engine mixer's music channels
## (MusicDirector.has_pending_playback); the director's stop ends it at once.
## The wait is bounded: after `timeout_msec` the caller quits with the track
## streaming (the forced-exit ordering tracked in
## docs/world/npc-mission-completion.md "Runtime shutdown"). Returns false on
## that timeout.
func await_playback_stopped(timeout_msec: int = PLAYBACK_DRAIN_TIMEOUT_MSEC) -> bool:
	var deadline := Time.get_ticks_msec() + timeout_msec
	while is_instance_valid(_director) and _director.has_pending_playback():
		if Time.get_ticks_msec() >= deadline:
			push_warning("MusicService: playback drain timed out; quitting with live playback (forced-exit ordering, see mus-sbf-re.md)")
			return false
		await get_tree().process_frame
	return true


func set_var(idx: int, value: int) -> void:
	if _director != null:
		_director.set_var(idx, value)


func get_var(idx: int) -> int:
	return _director.get_var(idx) if _director != null else 0
