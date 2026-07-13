class_name NovaRuntimeResourceSession
extends RefCounted

## One launch-scoped resource decision. The game host parses command-line state
## once, starts this session once, then gives this session's single mounted root
## to both the menu and GameWorld.

enum Mode {
	PACKED_ONLY,
	PACKED_WITH_LOOSE_OVERRIDE,
	ONED_LOOSE_ONLY,
}

var _root
var _mount_dir := ""
var _expansion := ""
var _mode: int = Mode.PACKED_ONLY
var _expansion_locked := false
var _save_expansion := Callable()
var _last_error := ""


## Start the launch session. resource_root is injectable for contract tests; the
## production host omits it and this module owns a new NovaResourceRoot.
func start(
	game_resource_dir: String,
	saved_expansion: String,
	launch,
	resource_root = null,
	save_expansion: Callable = Callable()
) -> int:
	_root = resource_root if resource_root != null else NovaResourceRoot.new()
	_save_expansion = save_expansion
	_expansion_locked = bool(launch.has_expansion_override)
	var oned_root := String(launch.oned_resource_root).strip_edges()
	if _expansion_locked:
		_expansion = String(launch.expansion).strip_edges()
	elif not oned_root.is_empty():
		# ONED owns a separate root and expansion context. A Play launch with no
		# explicit /exp must not inherit the standalone game's saved expansion.
		_expansion = ""
	else:
		_expansion = saved_expansion.strip_edges()
	if not oned_root.is_empty():
		_mode = Mode.ONED_LOOSE_ONLY
		_mount_dir = oned_root
	elif bool(launch.loose_override):
		_mode = Mode.PACKED_WITH_LOOSE_OVERRIDE
		_mount_dir = game_resource_dir.strip_edges()
	else:
		_mode = Mode.PACKED_ONLY
		_mount_dir = game_resource_dir.strip_edges()
	_last_error = ""
	var err := _mount(_expansion)
	if err != OK:
		_last_error = _root_error()
	return err


## Change expansion through the same mode and root chosen at launch. Explicit
## /exp sessions are read-only and never modify saved game settings.
func select_expansion(name: String) -> int:
	if _expansion_locked:
		return ERR_UNAUTHORIZED
	var requested := name.strip_edges()
	if requested == _expansion:
		return OK
	var previous := _expansion
	var err := _mount(requested)
	if err != OK:
		_last_error = _root_error()
		_mount(previous)
		return err
	_expansion = requested
	_last_error = ""
	if _save_expansion.is_valid():
		_save_expansion.call(_expansion)
	return OK


func get_root():
	return _root


func get_mode() -> int:
	return _mode


func get_expansion() -> String:
	return _expansion


func is_expansion_locked() -> bool:
	return _expansion_locked


func is_oned_session() -> bool:
	return _mode == Mode.ONED_LOOSE_ONLY


func get_last_error() -> String:
	return _last_error


func get_resource_dir() -> String:
	return _mount_dir


func list_expansions() -> PackedStringArray:
	if _root == null:
		return PackedStringArray()
	if _mode == Mode.ONED_LOOSE_ONLY:
		return _root.list_loose_expansions(_mount_dir)
	return _root.list_expansions(_mount_dir)


func _mount(expansion: String) -> int:
	match _mode:
		Mode.ONED_LOOSE_ONLY:
			return int(_root.mount_loose_runtime(_mount_dir, expansion))
		Mode.PACKED_WITH_LOOSE_OVERRIDE:
			return int(_root.mount_runtime(_mount_dir, expansion, true))
		_:
			return int(_root.mount_runtime(_mount_dir, expansion, false))


func _root_error() -> String:
	if _root != null and _root.has_method("get_last_error"):
		return String(_root.get_last_error())
	return "resource mount failed"
