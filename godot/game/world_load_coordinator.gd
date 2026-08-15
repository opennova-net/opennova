class_name WorldLoadCoordinator
extends RefCounted

## Owns one asynchronous shell-to-world load handoff: the cancellable operation,
## loading-screen presentation, progress/session signal wiring, completed-frame
## barrier, loader invocation, and deferred settlement. MainGame owns only shell
## mode and the decision to reveal or tear down the world.
## [orig: render_loading_screen @ 0x521d10; LoadingScreen_UpdateAndPresent
## @ 0x586be0; LoadingScreen_ReleaseEffect @ 0x525d52]

signal load_failed(reason: String)

## Forwarded from the screen when the SP start-mission splash finishes its
## final background-only frame [orig: show_start_mission_splash returns ->
## LoadingScreen_ReleaseEffect @ 0x525d45].
signal splash_dismissed

var _owner: Node = null
var _root: ResourceRoot = null
var _world: GameWorld = null
var _layer: CanvasLayer = null
var _screen: LoadingScreen = null
var _operation: WorldLoadOperation = null
var _presentation_active := false
var _load_info: Dictionary = {}


func can_start() -> bool:
	return _operation == null or _operation.is_settled()


func start(owner: Node, root: ResourceRoot, world: GameWorld,
		load_info: Dictionary, loader: Callable) -> WorldLoadOperation:
	if not can_start() or owner == null or world == null or not loader.is_valid():
		return null
	_owner = owner
	_root = root
	_world = world
	_load_info = load_info
	_operation = WorldLoadOperation.new()
	_presentation_active = true
	_show_screen(load_info)
	_run(_operation, loader)
	return _operation


func cancel_current() -> WorldLoadOperation:
	var operation := _operation
	if operation != null:
		operation.cancel()
	_presentation_active = false
	return operation


func current_operation() -> WorldLoadOperation:
	return _operation


func finish_presentation() -> void:
	_presentation_active = false
	dismiss()


func dismiss() -> void:
	_presentation_active = false
	if _world != null:
		if _world.load_progress.is_connected(_on_load_progress):
			_world.load_progress.disconnect(_on_load_progress)
		if _world.join_session_identified.is_connected(_on_join_session_identified):
			_world.join_session_identified.disconnect(_on_join_session_identified)
	if _screen != null:
		_screen.queue_free()
		_screen = null


func has_background() -> bool:
	return _screen != null and _screen.has_background()


## Whether this load carries an MP session — the SP splash gate's session
## half; SP `load_info` never sets `in_session`
## [orig: the !is_in_session leg of the splash gate @ 0x525d38].
func is_session_load() -> bool:
	return bool(_load_info.get("in_session", false))


## The screen's custom-background flag (the retail g_loadscreen_has_custom_bg
## analog); false once the presentation is down.
func has_custom_background() -> bool:
	return _screen != null and _screen.has_custom_background()


func is_splash_active() -> bool:
	return _screen != null and _screen.is_splash_active()


## Raise the SP start-mission splash on the held screen. The screen's
## dismissal edge is forwarded on `splash_dismissed`; a torn-down screen
## drops the pending edge with it.
func begin_start_mission_splash() -> bool:
	if _screen == null or not _presentation_active:
		return false
	if not _screen.begin_start_mission_splash(_root):
		return false
	_screen.splash_dismissed.connect(
			func() -> void: splash_dismissed.emit(),
			CONNECT_ONE_SHOT)
	return true


## The shell-policy wrapper the load-complete path calls: gate on a
## single-player load with a custom (sidecar) background, raise the splash,
## and run its device legs — hide the OS cursor (the splash draws the cursor
## art itself [orig: Mouse_SetCallback(0) @ 0x520862], restored on the
## dismissal edge) and fire the one-shot START_MISSION (fire-and-forget;
## playback never gates dismissal [orig: play @ 0x5208fd; the exit tests
## read only the input state @ 0x520a2d/0x520a36])
## [orig: show_start_mission_splash @ 0x520820, called @ 0x525d42, gated
## @ 0x525d38 on g_loadscreen_has_custom_bg + single player]. The headless
## skip is deliberate shell policy, not witnessed behavior: fixture SP loads
## resolve a sidecar image and would otherwise hold the presentation forever
## with no input to dismiss it.
func maybe_begin_start_mission_splash(audio: MissionAudio) -> bool:
	if is_session_load() or not has_custom_background() \
			or DisplayServer.get_name() == "headless":
		return false
	if not begin_start_mission_splash():
		return false
	Input.set_mouse_mode(Input.MOUSE_MODE_HIDDEN)
	# The restore rides the SCREEN's edge so an aborted presentation drops the
	# pending restore with the freed screen instead of arming a stale one-shot
	# on this long-lived coordinator. It runs after the forwarded reveal in
	# the same emission — same frame, order immaterial.
	_screen.splash_dismissed.connect(func() -> void:
			Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE),
			CONNECT_ONE_SHOT)
	if audio != null:
		audio.ui_soundset(HudPos.loading_splash_sound_set())
	return true


func _run(operation: WorldLoadOperation, loader: Callable) -> void:
	var screen := _screen
	if screen != null:
		var prepared := await screen.prepare_for_blocking_load(operation)
		if _is_cancelled_or_stale(operation):
			_settle(operation)
			return
		if not prepared:
			load_failed.emit(
					"loading screen left the SceneTree before mission load")
			_settle(operation)
			return
	else:
		await _owner.get_tree().process_frame
		if _is_cancelled_or_stale(operation):
			_settle(operation)
			return
	var result = loader.call()
	var err := int(result) if result != null else OK
	# GameWorld normally emits load_failed inside the loader call. If that path
	# already dismissed this presentation, suppress the duplicate fallback.
	if err != OK and operation == _operation and _presentation_active:
		load_failed.emit(error_string(err))
	_settle(operation)


func _is_cancelled_or_stale(operation: WorldLoadOperation) -> bool:
	return operation.is_cancelled() or operation != _operation


func _settle(operation: WorldLoadOperation) -> void:
	if operation == null:
		return
	operation.settle()
	if operation == _operation:
		_operation = null


func _show_screen(load_info: Dictionary) -> void:
	dismiss()
	_presentation_active = true
	if _root == null:
		return
	# A missing sidecar/loadscrn image leaves the screen dark; LoadingScreen owns
	# that exact texture-miss path [orig: render_loading_screen @ 0x521eb0].
	if _layer == null:
		_layer = CanvasLayer.new()
		_layer.name = "LoadingLayer"
		_layer.layer = 3
		_owner.add_child(_layer)
	_screen = LoadingScreen.new()
	_screen.name = "LoadingScreen"
	_layer.add_child(_screen)
	_screen.setup(_root, load_info)
	# CanvasLayer is not a Control parent, so full-rect anchors have no layout
	# rectangle to resolve against.
	_screen.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_screen.position = Vector2.ZERO
	_screen.size = _screen.get_viewport_rect().size
	if not _world.load_progress.is_connected(_on_load_progress):
		_world.load_progress.connect(_on_load_progress)
	if not _world.join_session_identified.is_connected(_on_join_session_identified):
		_world.join_session_identified.connect(_on_join_session_identified)


func _on_join_session_identified(info: Dictionary) -> void:
	# The joiner's 0x7B record fills the same session strings retail resolves
	# before its wire-header world load [orig: parse_server_session_variables
	# @ 0x5202f0 -> loading title/mission buffers @ 0x51f533/0x51f53a].
	if _screen != null and _presentation_active:
		_screen.update_session_info(_root, info)


func _on_load_progress(percent: int) -> void:
	if _screen != null:
		_screen.set_progress(percent)
		_screen.present()
