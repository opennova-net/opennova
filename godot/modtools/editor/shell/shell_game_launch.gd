class_name ShellGameLaunch
extends RefCounted

## The "See in game" affordance (maturity slice F3): one top-bar gesture that
## launches the game runtime over the SAME authoring directory the editor has
## mounted. ONED passes /d plus a private resource-root handoff; that exact
## combination creates a loose-only runtime session with no PFF dependency.
##
## The private handoff is session-only: ONED and the game keep separate saved
## resource roots. /exp rides along when selected in ONED, but is not persisted
## by the launched game. The editor never tracks the process.
##
## Shell sub-controller in the EditorResourceLibrary style: owns one surface
## (the top-bar button) over injected nodes + Callables (the A8 captured-lambda
## rule), so tests drive the composition without touching the OS.
##
## Per-workspace honesty (TER-1/ENV-1): the active workspace may supply a
## typed EditorWorkspace.GameLaunchNote (via the injected launch_note seam) —
## whether its authored data is staged in the launch directory and what to
## look at after launching. The note refines the tooltip and the post-launch
## status; the launch itself is always the same generic gesture.

const RUNTIME_SCENE := "res://game/main_game.tscn"
## The packaged two-product install (ADR 0015): the game exe ships beside the
## editor exe (export_presets.cfg: opennova.exe / opennova-modtools.exe).
const PACKAGED_RUNTIME_CANDIDATES: Array[String] = ["opennova.exe", "opennova.x86_64", "opennova"]

const TOOLTIP_READY := "See in game: launch the game from ONED's loose files."
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
# func(path: String, args: PackedStringArray) -> int: process spawn, pid or -1.
var _spawn: Callable
# func(path: String) -> bool: file-existence probe (the packaged runtime exe).
var _file_exists: Callable
# func(launch_dir: String) -> EditorWorkspace.GameLaunchNote (or null): the
# ACTIVE workspace's See-in-game note — per-workspace staging honesty for the
# tooltip and the post-launch status. Invalid/absent = the generic copy.
var _launch_note: Callable
var _show_status: Callable


func setup(
	button: Button,
	resource_dir: Callable,
	expansion: Callable,
	spawn: Callable,
	file_exists: Callable,
	launch_note: Callable,
	show_status: Callable
) -> void:
	_button = button
	_resource_dir = resource_dir
	_expansion = expansion
	_spawn = spawn
	_file_exists = file_exists
	_launch_note = launch_note
	_show_status = show_status
	if _button != null:
		_button.icon = EditorIconLibrary.resolve(&"play_in_game")
		if not _button.pressed.is_connected(_on_pressed):
			_button.pressed.connect(_on_pressed)
	refresh()


## /d is the public dev flag. The private root marker distinguishes ONED's
## loose-only session from a normal game's packed-plus-loose /d behavior.
static func runtime_flags(resource_dir: String, expansion: String) -> PackedStringArray:
	var flags := PackedStringArray(["/d"])
	var root_dir := resource_dir.strip_edges()
	if not root_dir.is_empty():
		flags.append("--oned-resource-root")
		flags.append(root_dir)
	var exp_name := expansion.strip_edges()
	if not exp_name.is_empty():
		flags.append("/exp")
		flags.append(exp_name)
	return flags


## Where the runtime lives, as a LaunchPlan. The packaged game exe beside the
## editor wins (the shipped two-product layout); running from source falls
## back to this binary re-run on the game scene. Null when neither is
## available.
static func launch_plan(
	editor_exe: String,
	project_dir: String,
	dev_mode: bool,
	resource_dir: String,
	expansion: String,
	file_exists: Callable
) -> LaunchPlan:
	var exe_dir := editor_exe.get_base_dir()
	for candidate in PACKAGED_RUNTIME_CANDIDATES:
		var path := exe_dir.path_join(candidate)
		if bool(file_exists.call(path)):
			var args := PackedStringArray(["--"])
			args.append_array(runtime_flags(resource_dir, expansion))
			return LaunchPlan.make(path, args)
	if dev_mode:
		var args := PackedStringArray(["--path", project_dir, RUNTIME_SCENE, "--"])
		args.append_array(runtime_flags(resource_dir, expansion))
		return LaunchPlan.make(editor_exe, args)
	return null


func available() -> bool:
	return _current_plan() != null


func can_launch() -> bool:
	return available() and not String(_resource_dir.call()).is_empty()


## Re-gate the button (called by the shell whenever the resource root or the
## active workspace changes).
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
		var tooltip := TOOLTIP_READY
		var note := _workspace_note()
		if note != null and not note.detail.is_empty():
			tooltip += "\n" + note.detail
		_button.tooltip_text = tooltip


func launch() -> bool:
	var plan := _current_plan()
	if plan == null or String(_resource_dir.call()).is_empty():
		refresh()
		return false
	var pid := int(_spawn.call(plan.path, plan.args))
	if pid < 0:
		_show_status.call("Could not launch the game runtime.", 0.0, &"error")
		return false
	var note := _workspace_note()
	if note != null and not note.detail.is_empty():
		# The active workspace's staging honesty: a warn when the authored data
		# has not reached the launch directory yet (the game boots fine either
		# way — it just will not show the unstaged work).
		_show_status.call("Game launched — " + note.detail, 0.0,
			&"info" if note.staged else &"warn")
	else:
		_show_status.call("Game launched — using ONED's loose files (/d).", 0.0, &"info")
	return true


func _workspace_note() -> EditorWorkspace.GameLaunchNote:
	if not _launch_note.is_valid():
		return null
	return _launch_note.call(String(_resource_dir.call())) as EditorWorkspace.GameLaunchNote


func _current_plan() -> LaunchPlan:
	return launch_plan(
		OS.get_executable_path(),
		ProjectSettings.globalize_path("res://"),
		OS.has_feature("editor"),
		String(_resource_dir.call()),
		String(_expansion.call()),
		_file_exists
	)


func _on_pressed() -> void:
	launch()
