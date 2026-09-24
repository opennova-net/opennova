class_name DebugHostFixture
extends DebugShellHost

## The debug-control table's shell seam, faked for tests (ADR 0043 rule 11:
## an interface class faked by overriding its hooks): the owners a test lends
## the table (a REAL MissionRoot whose Simulation is the engine row owner, an
## optional world, presenter and viewport-bearing node), a settable authority
## fact, and counters for the shell's resume and return-to-menu legs. Every
## hook answers live, so a test swaps `runtime` between calls the way a
## mission reload swaps the shell's.

var runtime: MissionRoot = null
var world: GameWorld = null
var player: LocalPlayerPresenter = null
## The node whose viewport the viewport rows mutate (a test passes itself).
var viewport_node: Node = null
var authority := true
var resume_calls := 0
var return_to_menu_calls := 0
var return_to_menu_result: Error = OK


## A host over one real runtime (WorldFixture.boot_mission_data).
static func for_runtime(p_runtime: MissionRoot, p_viewport_node: Node = null) -> DebugHostFixture:
	var host := DebugHostFixture.new()
	host.runtime = p_runtime
	host.viewport_node = p_viewport_node
	return host


func _world() -> GameWorld:
	return world if world != null and is_instance_valid(world) else null


func _runtime() -> MissionRoot:
	return runtime if runtime != null and is_instance_valid(runtime) else null


func _player_presenter() -> LocalPlayerPresenter:
	return player if player != null and is_instance_valid(player) else null


func _viewport() -> Viewport:
	if viewport_node == null or not is_instance_valid(viewport_node) \
			or not viewport_node.is_inside_tree():
		return null
	return viewport_node.get_viewport()


## The shell's resume leg plays the runtime (the game's leg also closes a
## pause overlay, which a fixture has none of).
func _resume() -> Error:
	resume_calls += 1
	var live := _runtime()
	if live == null:
		return ERR_UNAVAILABLE
	live.play()
	return OK


func _return_to_menu() -> Error:
	return_to_menu_calls += 1
	return return_to_menu_result


func _has_debug_authority() -> bool:
	return authority
