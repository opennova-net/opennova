class_name GameShell
extends Node3D

## The typed surface the game shell hands its tooling: the debug adapter, the
## probe context and runner, and the debug-control table depend on THIS type
## and never on MainGame's privates (ADR 0043 rule 11; retires the
## GameShellSeams Callable record). The base is the null shell: every read
## answers "unavailable" (null / false / "" / ERR_UNAVAILABLE), so a test fakes
## a shell by overriding the public verbs it needs and nothing else. MainGame is
## the one production shell; every supplier resolves live per call, never cached.

# --- live suppliers -----------------------------------------------------------

## The permanent world node (a loaded mission is `get_world().is_loaded()`).
func get_world() -> GameWorld:
	return null


## The loaded world's runtime (its MissionRoot), null between missions.
func get_runtime() -> MissionRoot:
	return null


func get_player_presenter() -> LocalPlayerPresenter:
	return null


func get_hud_presenter() -> GameHudPresenter:
	return null


func get_menu_shell() -> MenuShell:
	return null


## The shell's in-world armory surface.
func get_armory_presenter() -> ArmoryPresenter:
	return null


## The joiner's deploy-map screen.
func get_deploy_presenter() -> DeployScreenPresenter:
	return null


func get_dev_tools() -> DevTools:
	return null


func get_frame_stats() -> FrameStats:
	return null


## The mounted runtime root.
func current_resource_root() -> ResourceRoot:
	return null


func get_game_debug_adapter() -> GameDebugAdapter:
	return null


# --- shell-state reads --------------------------------------------------------

## The shell's state name: "menu" / "world" / "paused" / "armory" / "deploy" /
## "end_round"; "" from the null shell.
func shell_state_name() -> String:
	return ""


## A world load / start-mission splash owns the viewport.
func is_world_loading() -> bool:
	return false


## The F3 dev tools are open.
func is_dev_tools_open() -> bool:
	return false


# --- mission verbs (the probe runner's, ADR 0041) -----------------------------

func start_mission(_bms_name: String) -> Error:
	return ERR_UNAVAILABLE


func start_saved_mission(_saved_path: String, _bms_name: String,
		_profile: Dictionary = {}) -> Error:
	return ERR_UNAVAILABLE


## The gated public return leg.
func return_to_menu() -> Error:
	return ERR_UNAVAILABLE


# --- shell action legs (the debug adapter's game_control verbs) ---------------

## The shell's resume leg: closes pause/armory and hands play back.
func resume() -> void:
	pass


## Graceful runtime quit.
func request_quit() -> void:
	pass


## Open the ESC pause overlay.
func mcp_open_ingame_menu() -> Error:
	return ERR_UNAVAILABLE


## The armory's direct-open.
func mcp_open_armory() -> Error:
	return ERR_UNAVAILABLE


# --- capture presentation actions ---------------------------------------------

## The shell-owned world_only capture presentation: begin snapshots/hides
## presentation; end restores the exact prior state after the async readback,
## including failure/cancellation.
func mcp_begin_world_only_capture() -> Error:
	return ERR_UNAVAILABLE


func mcp_end_world_only_capture() -> void:
	pass


## The screenshot-scoped HUD-hidden capture presentation; the witness is
## sampled inside the correlated completed-draw callback while the transaction
## is active (null when the shell has no HUD to witness).
func begin_hud_hidden_capture() -> Error:
	return ERR_UNAVAILABLE


func finish_hud_hidden_capture() -> void:
	pass


func hud_hidden_capture_witness() -> HudHiddenCaptureWitness:
	return null
