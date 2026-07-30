class_name EditorGameRunBridge
extends RefCounted

## Typed boundary between the stable editor MCP service and ONED's managed
## standalone-game lifecycle. The session owns process state; the optional
## commands let the workstation keep toolbar presentation in sync.

var session: ShellGameSession = null

var _run_game := Callable()
var _stop_game := Callable()
var _state_changed := Callable()


func _init(
		game_session: ShellGameSession = null,
		run_game: Callable = Callable(),
		stop_game: Callable = Callable(),
		state_changed: Callable = Callable()) -> void:
	session = game_session
	_run_game = run_game
	_stop_game = stop_game
	_state_changed = state_changed


func start(mode: String) -> bool:
	if _run_game.is_valid():
		return bool(_run_game.call(mode))
	return session != null and session.start_mode(mode)


func stop() -> bool:
	if _stop_game.is_valid():
		return bool(_stop_game.call())
	return session != null and session.stop()


func notify_state_changed() -> void:
	if _state_changed.is_valid():
		_state_changed.call()
