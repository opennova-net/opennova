class_name GameShellSeams
extends RefCounted

## The one typed record of Callable seams the game shell hands its debug
## adapter and the probe runner (ADR 0017/0041; ADR 0034 d3 retired loose
## Callable bundles): live suppliers of the shell's private presenters
## (resolved on every call, never cached), the shell-state reads, the mission
## and screen verbs, and the capture presentation actions. MainGame fills one
## in and GameDebugAdapter.configure() adopts it; ProbeContext reads through
## the same record. Every member is optional: an unset Callable reads as
## "unavailable in this shell".

# --- live suppliers -----------------------------------------------------------

## func() -> Node (the game shell)
var game_source := Callable()
## func() -> GameWorld
var world_source := Callable()
## func() -> MissionPresentation (the loaded world's runtime)
var runtime_source := Callable()
## func() -> LocalPlayerPresenter
var presenter_source := Callable()
## func() -> GameHudPresenter
var hud_presenter_source := Callable()
## func() -> MenuShell
var menu_shell_source := Callable()
## func() -> ArmoryPresenter (the shell's in-world armory surface)
var armory_presenter_source := Callable()
## func() -> DeployScreenPresenter (the joiner's deploy-map screen)
var deploy_presenter_source := Callable()
## func() -> DevTools
var dev_tools_source := Callable()
## func() -> FrameStats
var frame_stats_source := Callable()
## func() -> Viewport (the shell's)
var viewport_source := Callable()
## func() -> ResourceRoot (the mounted runtime root)
var resource_root_source := Callable()
## func() -> GameMcpAdapter
var adapter_source := Callable()

# --- shell-state reads --------------------------------------------------------

## func() -> String (the shell's state name: "menu"/"world"/"paused"/...)
var shell_state_source := Callable()
## func() -> bool (a world load / start-mission splash owns the viewport)
var world_loading_source := Callable()
## func() -> bool (the F3 dev tools are open)
var dev_tools_open_source := Callable()

# --- mission verbs (the probe runner's, ADR 0041) -----------------------------

## func(bms_name: String) -> Error
var start_mission := Callable()
## func(saved_path: String, bms_name: String, profile: Dictionary) -> Error
var start_saved_mission := Callable()
## func() -> Error (the gated public return leg)
var return_to_menu := Callable()

# --- shell action legs (the debug adapter's game_control verbs) ---------------

## func() -> void: the shell's resume leg (closes pause/armory, hands play back)
var resume_action := Callable()
## func() -> void: the shell's raw teardown-to-menu leg (the adapter gates it)
var return_to_menu_action := Callable()
## func() -> void: graceful runtime quit
var quit_action := Callable()
## func() -> Error: open the ESC pause overlay
var open_ingame_menu_action := Callable()
## func() -> Error: the armory's direct-open
var open_armory_action := Callable()

# --- capture presentation actions ---------------------------------------------

## func() -> Error / func() -> void: the shell-owned world_only capture
## presentation (begin snapshots/hides presentation; end restores the exact
## prior state after the async readback, including failure/cancellation).
var render_capture_begin_action := Callable()
var render_capture_end_action := Callable()
## func() -> Error / func() -> void / func() -> HudHiddenCaptureWitness: the
## screenshot-scoped HUD-hidden capture presentation; the witness is sampled
## inside the correlated completed-draw callback while the transaction is
## active.
var hud_hidden_capture_begin_action := Callable()
var hud_hidden_capture_end_action := Callable()
var hud_hidden_capture_witness_source := Callable()


## The game shell's seams: its private presenters and state reads arrive as
## supplied Callables, everything else binds the shell's public methods by name
## (MainGame: get_dev_tools, get_frame_stats, get_viewport,
## current_resource_root, get_game_debug_adapter, start_mission,
## start_saved_mission, return_to_menu, is_dev_tools_open, request_quit,
## mcp_open_ingame_menu, mcp_open_armory, the capture begin/end/witness set).
## MainGame stamps the remaining private-leg fields (shell_state_source,
## world_loading_source, resume_action, return_to_menu_action) before handing
## the record to configure().
static func for_shell(shell: MainGame, world: Callable, runtime: Callable,
		presenter: Callable, hud_presenter: Callable, menu_shell: Callable,
		armory_presenter := Callable(), deploy_presenter := Callable()) -> GameShellSeams:
	var seams := GameShellSeams.new()
	seams.game_source = func() -> Node: return shell
	seams.world_source = world
	seams.runtime_source = runtime
	seams.presenter_source = presenter
	seams.hud_presenter_source = hud_presenter
	seams.menu_shell_source = menu_shell
	seams.armory_presenter_source = armory_presenter
	seams.deploy_presenter_source = deploy_presenter
	seams.dev_tools_source = shell.get_dev_tools
	seams.frame_stats_source = shell.get_frame_stats
	seams.viewport_source = shell.get_viewport
	seams.resource_root_source = shell.current_resource_root
	seams.adapter_source = shell.get_game_debug_adapter
	seams.start_mission = shell.start_mission
	seams.start_saved_mission = shell.start_saved_mission
	seams.return_to_menu = shell.return_to_menu
	seams.dev_tools_open_source = shell.is_dev_tools_open
	seams.quit_action = shell.request_quit
	seams.open_ingame_menu_action = shell.mcp_open_ingame_menu
	seams.open_armory_action = shell.mcp_open_armory
	seams.render_capture_begin_action = shell.mcp_begin_world_only_capture
	seams.render_capture_end_action = shell.mcp_end_world_only_capture
	seams.hud_hidden_capture_begin_action = shell.begin_hud_hidden_capture
	seams.hud_hidden_capture_end_action = shell.finish_hud_hidden_capture
	seams.hud_hidden_capture_witness_source = shell.hud_hidden_capture_witness
	return seams
