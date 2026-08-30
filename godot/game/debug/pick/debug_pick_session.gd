class_name DebugPickSession
extends RefCounted
## The shell's debug-pick state in one place: the pick list the world highlights,
## the Shift+F6 crosshair flow with its toast, the tools-open click-catcher
## latch, and the one forward that makes a landed pick the F3 Entities window's
## selection (DevTools.select_entity, carrying only the engine handle). Owned by
## MainGame; a world is handed in per mission and per policy change.

var list := DebugPickList.new()
var flow := DebugPickFlow.new()
var _click_active := false


## Wire the pick -> Entities-window selection forward once (the DevTools node
## lives as long as the shell).
func setup(dev_tools: DevTools) -> void:
	if not list.picked.is_connected(dev_tools.select_entity):
		list.picked.connect(dev_tools.select_entity)


## A fresh mission gets a fresh pick set (stale handles never cross sessions);
## the world renders/curates the shell-owned list from here on.
func begin_world(world: GameWorld) -> void:
	list.clear()
	world.set_pick_debug(list)


## Install or remove the world's click catcher; a no-op without a world or when
## the policy did not change (the catcher node is rebuilt only on the edge).
func sync_click_policy(world: GameWorld, enabled: bool) -> void:
	if world == null or enabled == _click_active:
		return
	_click_active = enabled
	world.set_pick_click_enabled(enabled)


func is_click_active() -> bool:
	return _click_active


## Shift+F6: pick whatever the crosshair is on, with a brief on-screen toast.
func pick_at_crosshair(sim: Simulation, camera: Camera3D, toast_mount: Node) -> void:
	flow.pick_at_crosshair(sim, camera, list, toast_mount)
