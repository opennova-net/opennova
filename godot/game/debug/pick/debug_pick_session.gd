class_name DebugPickSession
extends RefCounted
## The shell's F3 debug-session state in one place: the pick list, the
## Shift+F6 crosshair flow with its toast, the tools-open click-catcher latch,
## and the one forward that makes a landed pick the F3 Entities window's
## selection (DevTools.select_entity, carrying only the engine handle). Owned
## by MainGame; a world is handed in per mission and per policy change.

const PICK_CATCHER_NAME := "PickClickCatcher"

var list := DebugPickList.new()
var flow := DebugPickFlow.new()
var _click_active := false


## Wire the pick -> Entities-window selection forward once (the DevTools node
## lives as long as the shell).
func setup(dev_tools: DevTools) -> void:
	if not list.picked.is_connected(dev_tools.select_entity):
		list.picked.connect(dev_tools.select_entity)


## A fresh mission gets a fresh pick set (stale handles never cross sessions);
## the click catcher follows the latch onto the new world.
func begin_world(world: GameWorld) -> void:
	list.clear()
	if _click_active:
		_set_click_catcher(world, true)


## Install or remove the world's click catcher; a no-op without a world or when
## the policy did not change (the catcher node is rebuilt only on the edge).
func sync_click_policy(world: GameWorld, enabled: bool) -> void:
	if world == null or enabled == _click_active:
		return
	_click_active = enabled
	_set_click_catcher(world, enabled)


func is_click_active() -> bool:
	return _click_active


## Shift+F6: pick whatever the crosshair is on, with a brief on-screen toast.
func pick_at_crosshair(sim: Simulation, camera: Camera3D, toast_mount: Node) -> void:
	flow.pick_at_crosshair(sim, camera, list, toast_mount)


## While the dev tools are open (mouse released), a world click ray-picks the
## entity under the cursor into the pick list. The catcher lives under the
## world so it sees clicks in the world viewport's coordinates; the old one
## detaches before its deferred destruction so the name stays stable.
func _set_click_catcher(world: GameWorld, enabled: bool) -> void:
	var existing: Node = world.get_node_or_null(NodePath(PICK_CATCHER_NAME))
	if existing != null:
		world.remove_child(existing)
		existing.queue_free()
	if not enabled:
		return
	var catcher := PickClickCatcher.new()
	catcher.name = PICK_CATCHER_NAME
	world.add_child(catcher)
	catcher.setup(world, list)
