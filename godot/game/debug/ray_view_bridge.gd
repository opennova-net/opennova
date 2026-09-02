extends Node

# The F3 Rays window's shell leg: DevTools (C++) cannot build the GDScript
# ray debug view, so the window's "Show rays" checkbox queues a typed view
# toggle that this node drains into GameWorld.set_ray_debug each frame, and
# the live toggle state is mirrored back so the window's checkbox tracks the
# toggle wherever it was flipped (the window, game_debug, or MCP). A scene
# child of MainGame (main_game.tscn) beside the World node it drives.

var _shell: MainGame = null


func _ready() -> void:
	_shell = get_parent() as MainGame


func _process(_delta: float) -> void:
	if _shell == null:
		return
	var tools := _shell.get_dev_tools()
	if tools == null:
		return
	var world := _shell.get_node_or_null(^"World") as GameWorld
	if world == null:
		tools.set_ray_view_shown(false)
		return
	var toggle: int = tools.take_ray_view_toggle()
	if toggle >= 0:
		world.debug_views().set_ray_debug(toggle == 1)
	tools.set_ray_view_shown(world.debug_views().is_ray_debug())
