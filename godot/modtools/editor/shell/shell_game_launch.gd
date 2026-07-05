class_name ShellGameLaunch
extends RefCounted

## The "See in game" affordance (maturity slice F3): one top-bar gesture that
## launches the game runtime over the SAME authoring directory the editor has
## mounted, using the engine's own loose-file override — the retail `/d` dev
## flag the runtime already honors ([orig: `/d` loose-override @ 0x4a7310],
## NovaLaunchFlags). The runtime stays PFF-mount-first; `/d` overlays the
## authored loose files exactly as retail did, so every workspace's G axis is
## the same gesture: save, launch, see it.
##
## The editor and the runtime share one persisted resource directory
## (NovaResourceDirSettings), so the spawned game mounts the authoring dir
## without any hand-off; `/exp` and `/game` are passed explicitly so the launch
## never depends on stale config. One button + spawn, not a session manager:
## the runtime owns its own mount errors (an authoring dir without PFFs
## reports honestly in the game), and the editor never tracks the process.
##
## Shell sub-controller in the EditorResourceLibrary style: owns one surface
## (the top-bar button) over injected nodes + Callables (the A8 captured-lambda
## rule), so tests drive the composition without touching the OS.

const RUNTIME_SCENE := "res://game/main_game.tscn"
## The packaged two-product install (ADR 0015): the game exe ships beside the
## editor exe (export_presets.cfg: opennova.exe / opennova-modtools.exe).
const PACKAGED_RUNTIME_CANDIDATES: Array[String] = ["opennova.exe", "opennova.x86_64", "opennova"]

const TOOLTIP_READY := "See in game: launch the game with your saved files on top (loose-file override)."
const TOOLTIP_NEEDS_DIR := "See in game: pick a resource directory first (Settings)."
const TOOLTIP_NO_RUNTIME := "See in game: no game runtime is available beside this editor."


## One spawn plan (typed record, ADR 0017): which binary to run and with what
## arguments. launch_plan() returns null when no runtime is reachable.
class LaunchPlan:
	extends RefCounted

	var path: String
	var args: PackedStringArray

	static func make(plan_path: String, plan_args: PackedStringArray) -> LaunchPlan:
		var plan := LaunchPlan.new()
		plan.path = plan_path
		plan.args = plan_args
		return plan

var _button: Button
# func() -> String: the authoring/resource dir the editor has mounted ("" = none).
var _resource_dir: Callable
# func() -> String: the persisted expansion name ("" = base game).
var _expansion: Callable
# func() -> String: the persisted game code ("jo", "jodemo", ...).
var _game_code: Callable
# func(path: String, args: PackedStringArray) -> int: process spawn, pid or -1.
var _spawn: Callable
# func(path: String) -> bool: file-existence probe (the packaged runtime exe).
var _file_exists: Callable
var _show_status: Callable


func setup(
	button: Button,
	resource_dir: Callable,
	expansion: Callable,
	game_code: Callable,
	spawn: Callable,
	file_exists: Callable,
	show_status: Callable
) -> void:
	_button = button
	_resource_dir = resource_dir
	_expansion = expansion
	_game_code = game_code
	_spawn = spawn
	_file_exists = file_exists
	_show_status = show_status
	if _button != null:
		_button.icon = EditorIconLibrary.resolve(&"play_in_game")
		if not _button.pressed.is_connected(_on_pressed):
			_button.pressed.connect(_on_pressed)
	refresh()


## The retail-style runtime flags: `/d` always (the whole point of the
## gesture), `/exp` when an expansion is mounted, `/game` always so the SCR
## key choice never rides stale config.
static func runtime_flags(expansion: String, game_code: String) -> PackedStringArray:
	var flags := PackedStringArray(["/d"])
	var exp_name := expansion.strip_edges()
	if not exp_name.is_empty():
		flags.append("/exp")
		flags.append(exp_name)
	var code := game_code.strip_edges().to_lower()
	flags.append("/game")
	flags.append(code if not code.is_empty() else "jo")
	return flags


## Where the runtime lives, as a LaunchPlan. The packaged game exe beside the
## editor wins (the shipped two-product layout); running from source falls
## back to this binary re-run on the game scene. Null when neither is
## available.
static func launch_plan(
	editor_exe: String,
	project_dir: String,
	dev_mode: bool,
	expansion: String,
	game_code: String,
	file_exists: Callable
) -> LaunchPlan:
	var exe_dir := editor_exe.get_base_dir()
	for candidate in PACKAGED_RUNTIME_CANDIDATES:
		var path := exe_dir.path_join(candidate)
		if bool(file_exists.call(path)):
			return LaunchPlan.make(path, runtime_flags(expansion, game_code))
	if dev_mode:
		var args := PackedStringArray(["--path", project_dir, RUNTIME_SCENE, "--"])
		args.append_array(runtime_flags(expansion, game_code))
		return LaunchPlan.make(editor_exe, args)
	return null


func available() -> bool:
	return _current_plan() != null


func can_launch() -> bool:
	return available() and not String(_resource_dir.call()).is_empty()


## Re-gate the button (called by the shell whenever the resource root changes).
func refresh() -> void:
	if _button == null:
		return
	var has_runtime := available()
	_button.disabled = not (has_runtime and not String(_resource_dir.call()).is_empty())
	if not has_runtime:
		_button.tooltip_text = TOOLTIP_NO_RUNTIME
	elif _button.disabled:
		_button.tooltip_text = TOOLTIP_NEEDS_DIR
	else:
		_button.tooltip_text = TOOLTIP_READY


func launch() -> bool:
	var plan := _current_plan()
	if plan == null or String(_resource_dir.call()).is_empty():
		refresh()
		return false
	var pid := int(_spawn.call(plan.path, plan.args))
	if pid < 0:
		_show_status.call("Could not launch the game runtime.", 0.0, &"error")
		return false
	_show_status.call("Game launched — your saved files override the packed data (/d).", 0.0, &"info")
	return true


func _current_plan() -> LaunchPlan:
	return launch_plan(
		OS.get_executable_path(),
		ProjectSettings.globalize_path("res://"),
		OS.has_feature("editor"),
		String(_expansion.call()),
		String(_game_code.call()),
		_file_exists
	)


func _on_pressed() -> void:
	launch()
