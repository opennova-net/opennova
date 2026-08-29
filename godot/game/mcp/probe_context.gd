class_name ProbeContext
extends RefCounted

## Everything a running probe touches (docs/mcp.md, ADR 0041): its typed
## args and artifact directory, live accessors onto the shell (functions,
## never cached references: a probe must not hold a Simulation or Node
## across an await), the waits that replace the old boot tails, the mission
## verbs, guarded mutations that finish() undoes in reverse, capture helpers,
## and the log/progress channel game_probe op=status reads back.

const LOCAL_PLAYER_TIMEOUT_MS := 240_000
const MS_PER_SECOND := 1000.0

var run_id := ""
var name := ""
## The validated arguments (ProbeSchema output, defaults filled in).
var args: Dictionary = {}
## Absolute directory for this run's files (user://probe-runs/<run>).
var artifact_dir := ""
## Set by the runner (cancel, watchdog); long waits must check it.
var cancelled := false
var tree: SceneTree = null
var seams: GameShellSeams = null

var _line_sink := Callable()
var _artifacts: Array[Dictionary] = []
var _restores: Array[Callable] = []
var _progress: Dictionary = {}
var _finished := false


# --- live seams -----------------------------------------------------------------

func game() -> MainGame:
	return _supply(seams.game_source if seams != null else Callable()) as MainGame


func world() -> GameWorld:
	return _supply(seams.world_source if seams != null else Callable()) as GameWorld


func runtime() -> MissionPresentation:
	return _supply(seams.runtime_source if seams != null else Callable()) as MissionPresentation


func sim() -> Simulation:
	var live_world := world()
	return live_world.get_sim() if live_world != null else null


func presenter() -> LocalPlayerPresenter:
	return _supply(seams.presenter_source if seams != null else Callable()) as LocalPlayerPresenter


func hud_presenter() -> GameHudPresenter:
	return _supply(seams.hud_presenter_source if seams != null else Callable()) as GameHudPresenter


func menu_shell() -> MenuShell:
	return _supply(seams.menu_shell_source if seams != null else Callable()) as MenuShell


func armory_presenter() -> ArmoryPresenter:
	return _supply(seams.armory_presenter_source if seams != null else Callable()) as ArmoryPresenter


func deploy_presenter() -> DeployScreenPresenter:
	return _supply(seams.deploy_presenter_source if seams != null else Callable()) as DeployScreenPresenter


func dev_tools() -> DevTools:
	return _supply(seams.dev_tools_source if seams != null else Callable()) as DevTools


func frame_stats() -> FrameStats:
	return _supply(seams.frame_stats_source if seams != null else Callable()) as FrameStats


func viewport() -> Viewport:
	return _supply(seams.viewport_source if seams != null else Callable()) as Viewport


func camera() -> Camera3D:
	var live_viewport := viewport()
	return live_viewport.get_camera_3d() if live_viewport != null else null


func resource_root() -> ResourceRoot:
	return _supply(seams.resource_root_source if seams != null else Callable()) as ResourceRoot


func effect_world() -> EffectWorld:
	var live_world := world()
	return live_world.get_effect_world() if live_world != null else null


func adapter() -> GameMcpAdapter:
	return _supply(seams.adapter_source if seams != null else Callable()) as GameMcpAdapter


# --- waits ------------------------------------------------------------------------

func wait_frames(count: int) -> void:
	for _i in range(count):
		if cancelled or tree == null:
			return
		await tree.process_frame


func wait_ms(duration_ms: int) -> void:
	var deadline := Time.get_ticks_msec() + duration_ms
	while Time.get_ticks_msec() < deadline and not cancelled and tree != null:
		await tree.process_frame


## Wait for the loaded simulation to advance `seconds` of mission time (the
## engine's fixed logic-tick cadence, Simulation.tick_dt()); falls back to
## wall time without a sim.
func wait_mission_seconds(seconds: float) -> void:
	var live_sim := sim()
	if live_sim == null:
		await wait_ms(int(seconds * MS_PER_SECOND))
		return
	var target := int(live_sim.get_logic_tick()) + int(ceil(seconds / Simulation.tick_dt()))
	while not cancelled and tree != null:
		var current := sim()
		if current == null or int(current.get_logic_tick()) >= target:
			return
		await tree.process_frame


## The old StandaloneGameProbe.boot tail: wait until the world holds a local
## player, then leave the start-mission splash and the reveal. False on
## timeout or cancellation.
func wait_for_local_player(timeout_ms := LOCAL_PLAYER_TIMEOUT_MS) -> bool:
	var deadline := Time.get_ticks_msec() + timeout_ms
	while not cancelled and tree != null:
		var live_sim := sim()
		if live_sim != null and live_sim.has_local_player():
			return await wait_world_ready(maxi(deadline - Time.get_ticks_msec(), 0))
		if Time.get_ticks_msec() >= deadline:
			return false
		await tree.process_frame
	return false


## Leave the loading presentation: dismiss the SP start-mission splash through
## the shell's public seam until the shell reports the world revealed.
func wait_world_ready(timeout_ms := LOCAL_PLAYER_TIMEOUT_MS) -> bool:
	var deadline := Time.get_ticks_msec() + timeout_ms
	while not cancelled and tree != null:
		var shell := game()
		if shell == null:
			return false
		if not shell.is_world_loading():
			await tree.process_frame
			return true
		shell.dismiss_start_mission_splash()
		if Time.get_ticks_msec() >= deadline:
			return false
		await tree.process_frame
	return false


# --- mission verbs -------------------------------------------------------------------

func start_mission(bms_name: String) -> Error:
	if seams == null or not seams.start_mission.is_valid():
		return ERR_UNAVAILABLE
	return seams.start_mission.call(bms_name)


func start_saved_mission(saved_path: String, bms_name: String, profile: Dictionary = {}) -> Error:
	if seams == null or not seams.start_saved_mission.is_valid():
		return ERR_UNAVAILABLE
	return seams.start_saved_mission.call(saved_path, bms_name, profile)


func return_to_menu() -> Error:
	if seams == null or not seams.return_to_menu.is_valid():
		return ERR_UNAVAILABLE
	return seams.return_to_menu.call()


## The saved-BMS boot the capture probes share: leave a loaded world, start
## `saved_path` as `bms_name` with the local-player profile the deploy screen
## would have staged, then wait for the local player and the reveal. Returns
## "" or the failure text.
func load_saved_mission(saved_path: String, bms_name: String, profile: Dictionary = {}) -> String:
	var live_world := world()
	if live_world != null and live_world.is_loaded():
		var leave := return_to_menu()
		if leave != OK:
			return "could not leave the loaded world: %s" % error_string(leave)
		var leave_deadline := Time.get_ticks_msec() + LOCAL_PLAYER_TIMEOUT_MS
		while not cancelled and tree != null:
			var current := world()
			if current == null or not current.is_loaded():
				break
			if Time.get_ticks_msec() >= leave_deadline:
				return "timed out leaving the loaded world"
			await tree.process_frame
	var load_error: Array[String] = [""]
	var loading_world := world()
	if loading_world != null:
		loading_world.load_failed.connect(
				func(message: String) -> void: load_error[0] = message, CONNECT_ONE_SHOT)
	var start := start_saved_mission(saved_path, bms_name, profile)
	if start != OK:
		return "could not start %s from %s: %s" % [bms_name, saved_path, error_string(start)]
	var deadline := Time.get_ticks_msec() + LOCAL_PLAYER_TIMEOUT_MS
	while not cancelled and tree != null:
		if not load_error[0].is_empty():
			return load_error[0]
		var live_sim := sim()
		if live_sim != null and live_sim.has_local_player():
			break
		if Time.get_ticks_msec() >= deadline:
			return "timed out loading %s" % bms_name
		await tree.process_frame
	if cancelled:
		return "cancelled while loading %s" % bms_name
	if not await wait_world_ready(maxi(deadline - Time.get_ticks_msec(), 0)):
		return "timed out dismissing the start splash for %s" % bms_name
	return ""


# --- guarded mutations (undone by finish, in reverse) ------------------------------------

## Register the undo of a mutation the probe made itself.
func defer_restore(restore: Callable) -> void:
	_restores.append(restore)


func set_time_scale(scale: float) -> void:
	var previous := Engine.time_scale
	defer_restore(func() -> void: Engine.time_scale = previous)
	Engine.time_scale = scale


## Stop the shell and the world from processing (a still frame to capture).
func freeze_shell() -> void:
	var shell := game()
	if shell == null:
		return
	var previous := shell.process_mode
	defer_restore(func() -> void:
		if is_instance_valid(shell):
			shell.process_mode = previous)
	shell.process_mode = Node.PROCESS_MODE_DISABLED


func unfreeze_shell() -> void:
	var shell := game()
	if shell != null:
		shell.process_mode = Node.PROCESS_MODE_INHERIT


func set_menu_visible(visible: bool) -> void:
	var shell := menu_shell()
	if shell == null:
		return
	var previous := shell.visible
	defer_restore(func() -> void:
		if is_instance_valid(shell):
			shell.visible = previous)
	shell.visible = visible


## The shell window at an exact size (a capture's resolution); windowed for
## the run, the previous mode and size back at finish. False without a window.
func set_window_size(size: Vector2i) -> bool:
	var live_viewport := viewport()
	var window: Window = live_viewport.get_window() if live_viewport != null else null
	if window == null:
		return false
	var previous_mode := window.mode
	var previous_size := window.size
	defer_restore(func() -> void:
		if is_instance_valid(window):
			window.mode = previous_mode
			window.size = previous_size)
	window.mode = Window.MODE_WINDOWED
	window.size = size
	return true


## Runs every deferred restore in reverse order; idempotent. The runner calls
## it on every exit path.
func finish() -> void:
	if _finished:
		return
	_finished = true
	while not _restores.is_empty():
		var restore: Callable = _restores.pop_back()
		if restore.is_valid():
			restore.call()


# --- capture -----------------------------------------------------------------------

## Save the next drawn frame of a viewport (the shell's by default) as
## <artifact_dir>/<label>.png; returns the path ("" when nothing rendered).
func capture_png(label: String, target: Viewport = null) -> String:
	var live_viewport := target if target != null else viewport()
	if live_viewport == null or tree == null:
		return ""
	await RenderingServer.frame_post_draw
	var texture := live_viewport.get_texture()
	var image: Image = texture.get_image() if texture != null else null
	if image == null or image.is_empty():
		return ""
	var path := artifact_dir.path_join("%s.png" % _safe_label(label))
	if image.save_png(path) != OK:
		return ""
	artifact(label, path, "png")
	return path


## A lossless render bundle through the adapter's capture (the game_capture_bundle
## path); the PNG/JSON it wrote are recorded as artifacts. Returns the bundle
## metadata or {"error": ...}.
func capture_bundle(capture_args: Dictionary = {}) -> Dictionary:
	var live_adapter := adapter()
	if live_adapter == null:
		return { "error": "no game adapter" }
	var request := capture_args.duplicate(true)
	request["include_image"] = false
	var value: Variant = await live_adapter.capture_mcp_render_bundle(
			request, func() -> bool: return cancelled)
	if not (value is Dictionary):
		return { "error": "the render capture returned nothing" }
	var bundle: Dictionary = value
	bundle.erase("image_bytes")
	for key in ["image_path", "diagnostics_path"]:
		if bundle.has(key):
			artifact(String(bundle.get("label", "render")) + "." + key.get_slice("_", 0),
					String(bundle[key]), String(bundle[key]).get_extension())
	return bundle


## Record a file the probe produced (under artifact_dir or elsewhere).
func artifact(label: String, path: String, kind := "") -> void:
	var entry := {
		"label": label,
		"path": path,
		"kind": kind if not kind.is_empty() else path.get_extension(),
		"bytes": 0,
		"sha256": "",
	}
	if FileAccess.file_exists(path):
		var file := FileAccess.open(path, FileAccess.READ)
		if file != null:
			entry["bytes"] = file.get_length()
			file.close()
		entry["sha256"] = FileAccess.get_sha256(path)
	_artifacts.append(entry)


func artifacts() -> Array[Dictionary]:
	return _artifacts.duplicate()


# --- log / progress -----------------------------------------------------------------------

## One line into the run's log (op=status streams it) and the "probe" source
## of game_logs. Never print(): the probe's output IS this channel.
func log(text: String) -> void:
	if _line_sink.is_valid():
		_line_sink.call(text)
	if McpLogHub.instance != null:
		McpLogHub.instance.note("probe", "info", "[%s] %s" % [name, text])


## Publish the run's current progress object (op=status reports it).
func progress(value: Dictionary) -> void:
	_progress = value.duplicate(true)


func progress_value() -> Dictionary:
	return _progress


func set_line_sink(sink: Callable) -> void:
	_line_sink = sink


func _supply(source: Callable) -> Variant:
	if not source.is_valid():
		return null
	var value: Variant = source.call()
	if value is Object and not is_instance_valid(value):
		return null
	return value


static func _safe_label(label: String) -> String:
	var out := ""
	for character in label:
		out += character if character.is_valid_identifier() or character in ["-", "."] else "_"
	return out if not out.is_empty() else "capture"
