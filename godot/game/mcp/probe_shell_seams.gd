class_name ProbeShellSeams
extends RefCounted

## The Callables the game shell hands the probe runner: live suppliers of the
## shell's private presenters (resolved on every call, never cached) and the
## three mission verbs. MainGame fills one in and installs it on its debug
## adapter; ProbeContext reads through it. Every member is optional: an unset
## Callable reads as "unavailable in this shell".

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
## func(bms_name: String) -> Error
var start_mission := Callable()
## func(saved_path: String, bms_name: String, profile: Dictionary) -> Error
var start_saved_mission := Callable()
## func() -> Error
var return_to_menu := Callable()


## The game shell's seams: its private presenters as suppliers, everything
## else bound to the shell's public methods by name (MainGame: get_dev_tools,
## get_frame_stats, get_viewport, current_resource_root,
## get_game_debug_adapter, start_mission, start_saved_mission,
## return_to_menu); the runtime rides the world's get_runtime().
static func for_shell(shell: Node, world: Callable, presenter: Callable,
		hud_presenter: Callable, menu_shell: Callable,
		armory_presenter := Callable()) -> ProbeShellSeams:
	var seams := ProbeShellSeams.new()
	seams.game_source = func() -> Node: return shell
	seams.world_source = world
	seams.runtime_source = func() -> Variant:
		var live: Variant = world.call()
		return live.get_runtime() if live is GameWorld and is_instance_valid(live) else null
	seams.presenter_source = presenter
	seams.hud_presenter_source = hud_presenter
	seams.menu_shell_source = menu_shell
	seams.armory_presenter_source = armory_presenter
	seams.dev_tools_source = Callable(shell, "get_dev_tools")
	seams.frame_stats_source = Callable(shell, "get_frame_stats")
	seams.viewport_source = Callable(shell, "get_viewport")
	seams.resource_root_source = Callable(shell, "current_resource_root")
	seams.adapter_source = Callable(shell, "get_game_debug_adapter")
	seams.start_mission = Callable(shell, "start_mission")
	seams.start_saved_mission = Callable(shell, "start_saved_mission")
	seams.return_to_menu = Callable(shell, "return_to_menu")
	return seams
