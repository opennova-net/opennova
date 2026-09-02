extends Node

# The F3 Physics window's shell leg: DevTools (C++) cannot build the GDScript
# collision debug view, so the window's "Show collision" checkbox queues a
# typed view toggle that this node drains into GameWorld.set_collision_debug
# each frame, and the live toggle state plus the overlay's drawable count are
# mirrored back so the window's checkbox and "boxes drawn" line track the
# toggle wherever it was flipped (the window, game_debug, or MCP). A scene
# child of MainGame (main_game.tscn) beside the RayViewBridge it mirrors.

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
		tools.set_physics_view_state(false, 0)
		return
	var toggle: int = tools.take_physics_view_toggle()
	if toggle >= 0:
		world.debug_views().set_collision_debug(toggle == 1)
	tools.set_physics_view_state(world.debug_views().is_collision_debug(),
			world.debug_views().collision_debug_drawable_count())
