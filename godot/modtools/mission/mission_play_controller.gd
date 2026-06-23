extends Control

# Play-in-editor: boots the REAL game world — the same game_world.tscn the game
# shell instances, the same load path, the same MissionRuntime tick — inside an
# editor-owned SubViewport, fed the editor's resource root and the OPEN
# in-memory mission (unsaved edits play). This node IS the play viewport the
# workspace mounts in place of the edit viewport; input prioritization is
# structural: while this is mounted, the edit input router is out of the tree,
# so gizmos and picking cannot fire.
#
# The per-frame drive is literally the game shell's loop (main_game._process):
# world.tick(camera_position, camera_transform) -> foliage coverage, runtime logic + present,
# audio. Esc requests Stop; the workspace performs
# the viewport swap-back. Known v1 limits, surfaced in the status line: play
# loads the SAVED terrain/env from the resource root (live sculpt edits are not
# in the play world), and the play world is a second terrain instance in memory
# while it runs.

signal stop_requested
signal status_reported(message: String, is_error: bool)

const GameWorldScene := preload("res://engine/world/game_world.tscn")
const LocalPlayerHostScript := preload("res://engine/world/local_player_host.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const PlayInputRouterScript := preload("res://modtools/mission/mission_play_input_router.gd")

var _viewport: SubViewport
var _world  # GameWorld
var _camera: Camera3D
var _player_host: LocalPlayerHost
var _input_router: Node
var _status: Label
var _playing := false


func _ready() -> void:
	var container := SubViewportContainer.new()
	container.name = "PlayViewportContainer"
	container.stretch = true
	container.mouse_filter = Control.MOUSE_FILTER_STOP
	container.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(container)

	_viewport = SubViewport.new()
	_viewport.name = "PlayViewport"
	# Own 3D world so the played scene (terrain, physics, lighting) never bleeds
	# into the editor's; local input handling so the player host works embedded.
	_viewport.own_world_3d = true
	_viewport.handle_input_locally = true
	container.add_child(_viewport)

	_input_router = PlayInputRouterScript.new()
	_input_router.name = "PlayInputRouter"
	_input_router.input_target = self
	_viewport.add_child(_input_router)

	_world = GameWorldScene.instantiate()
	_world.name = "World"
	_viewport.add_child(_world)

	_camera = Camera3D.new()
	_camera.name = "PlayCamera"
	_camera.position = Vector3(0, 100, 0)
	_camera.current = true
	_viewport.add_child(_camera)

	_player_host = LocalPlayerHostScript.new()
	_player_host.name = "LocalPlayerHost"
	add_child(_player_host)
	_player_host.setup(_world, _camera)

	_status = Label.new()
	_status.name = "PlayStatus"
	_status.set_anchors_preset(Control.PRESET_BOTTOM_LEFT)
	_status.position = Vector2(8, -28)
	_status.add_theme_color_override("font_color", Color(1, 1, 1, 0.85))
	add_child(_status)

	_world.load_failed.connect(func(reason: String) -> void:
		status_reported.emit("Play failed: %s" % reason, true))


## Boot the game world over `mission` (the editor's live document). `bms_name`
## feeds the co-named audio lookups; `resource_root` is the editor's mounted
## VFS, injected so play resolves exactly the assets being authored.
func start(mission: NovaMissionData, bms_name: String, resource_root: NovaResourceRoot) -> Error:
	if _playing:
		return ERR_BUSY
	if mission == null or not mission.is_loaded():
		status_reported.emit("Open a mission before playing.", true)
		return ERR_UNAVAILABLE
	if resource_root == null or resource_root.get_root_dir().is_empty():
		status_reported.emit("No resource directory mounted to play from.", true)
		return ERR_UNCONFIGURED
	_world.set_resource_root(resource_root)
	if _world.has_method("set_playable"):
		_world.set_playable(true)
	var err := int(_world.load_mission_data(mission, bms_name))
	if err != OK:
		_world.unload()
		return err as Error
	_playing = true
	_park_camera(mission)
	_status.text = "Playing %s — saved terrain/env from %s (unsaved sculpt edits are not in the play world). Esc stops." % [
		bms_name, resource_root.get_root_dir().get_file()]
	status_reported.emit("Playing %s with the game loop. Esc to stop." % bms_name, false)
	return OK


## Tear the played world down. Safe to call repeatedly; the workspace swaps the
## edit viewport back after this returns.
func stop() -> void:
	if not _playing:
		return
	_playing = false
	if _player_host != null:
		_player_host.teardown()
	_world.unload()
	if _player_host != null:
		_player_host.setup(_world, _camera)
	_status.text = ""


func is_playing() -> bool:
	return _playing


func get_play_camera() -> Camera3D:
	return _camera


func get_world():
	return _world


func get_player_host():
	return _player_host


# The literal game-shell per-frame order (main_game._process): drive the loaded
# world's foliage + runtime + audio around the play camera.
func _process(delta: float) -> void:
	if _playing and _world != null and _world.is_loaded():
		if _player_host != null:
			_player_host.before_world_tick(delta, true)
		_world.tick(_camera.global_position, _camera.global_transform, delta)
		if _player_host != null:
			_player_host.after_world_tick()


func _unhandled_input(event: InputEvent) -> void:
	if handle_viewport_input(event):
		get_viewport().set_input_as_handled()


func handle_viewport_input(event: InputEvent) -> bool:
	if not _playing:
		return false
	if event is InputEventKey:
		var key := event as InputEventKey
		if key.pressed and not key.echo and key.keycode == KEY_ESCAPE:
			stop_requested.emit()
			return true
		if _player_host != null and _player_host.handle_key_input(event, true):
			return true
	if _player_host != null and _player_host.handle_input(event, true):
		return true
	return false


# Park the play camera near the action until the player host takes over: above the first organic (the usual player
# start area), else the first item, else a high overview. Mission positions are
# BMS-space; the camera lives in the play world's Godot space.
func _park_camera(mission: NovaMissionData) -> void:
	for kind in [NovaMissionData.KIND_ORGANIC, NovaMissionData.KIND_ITEM, NovaMissionData.KIND_BUILDING]:
		if mission.get_entity_count(kind) > 0:
			var entity: Dictionary = mission.get_entity(kind, 0)
			var pos := MissionObjectPlacer.bms_to_godot_position(entity.get("position", Vector3.ZERO))
			_camera.position = pos + Vector3(0, 30, 40)
			_camera.look_at(pos + Vector3(0, 1.5, 0))
			return
	_camera.position = Vector3(0, 150, 0)
