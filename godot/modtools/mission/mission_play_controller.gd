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
# F3 while playing, forwarded to the workspace's NovaDebugOverlay — the same key
# the game shell binds (main_game.DEBUG_OVERLAY_KEY), so play-in-editor and the
# runtime share the gesture.
signal debug_overlay_requested

const GameWorldScene := preload("res://engine/world/game_world.tscn")
const LocalPlayerHostScript := preload("res://engine/world/local_player_host.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")
const PlayInputRouterScript := preload("res://modtools/mission/mission_play_input_router.gd")

var _viewport: SubViewport
var _world  # GameWorld
var _camera: Camera3D
var _player_host: LocalPlayerHost
var _container: SubViewportContainer
var _ui_overlay_layer: CanvasLayer
var _ui_overlay: Control
var _armory: NovaArmoryHost
var _hud_host: NovaGameHudHost
var _input_router: Node
var _status: Label
var _playing := false
# True while the workspace's debug overlay is up: the mouse stays free so the
# overlay is clickable. The game shell gets this via its pause menu (Esc);
# play-in-editor has no pause state, so the overlay suspends capture instead.
var _capture_suspended := false


func _ready() -> void:
	var container := SubViewportContainer.new()
	container.name = "PlayViewportContainer"
	container.stretch = true
	container.mouse_filter = Control.MOUSE_FILTER_STOP
	container.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(container)
	_container = container

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

	# One clipped overlay surface tracking the embedded play panel. The HUD and
	# armory share it. It rides its own CanvasLayer at the game shell's $HUD
	# layer (viewmodel pass 0 < HUD 1 < menus 2), or the FP viewmodel pass —
	# a CanvasLayer itself — draws the gun OVER every HUD element; and the rect
	# syncs to the panel each frame (a CanvasLayer child cannot anchor to a
	# sibling Control, and an anchors-only preset on a fresh zero-rect Control
	# keeps it zero-sized: with clip_contents that clipped the HUD and the
	# armory to nothing in play-in-editor).
	_ui_overlay_layer = CanvasLayer.new()
	_ui_overlay_layer.name = "GameplayOverlayLayer"
	_ui_overlay_layer.layer = 1
	add_child(_ui_overlay_layer)
	_ui_overlay = Control.new()
	_ui_overlay.name = "GameplayOverlay"
	_ui_overlay.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_ui_overlay.clip_contents = true
	_ui_overlay_layer.add_child(_ui_overlay)
	_sync_overlay_rect()

	# The in-game HUD — the SAME NovaGameHudHost the game shell mounts (crosshair,
	# ammo cluster, mission text), so play-in-editor shows the game's HUD. Added
	# before the armory so its overlay draws beneath the WEAPON screen.
	_hud_host = NovaGameHudHost.new()
	_hud_host.name = "GameHudHost"
	add_child(_hud_host)
	_hud_host.setup(_world, _player_host, _ui_overlay)

	# The in-world armory — the SAME NovaArmoryHost the game shell mounts (one
	# armory code path; editor-runtime parity). The weapon.mnu overlay parents to
	# this control, over the play viewport.
	_armory = NovaArmoryHost.new()
	_armory.name = "ArmoryHost"
	add_child(_armory)
	_armory.setup(_world, _player_host, _ui_overlay)
	_armory.opened.connect(func() -> void:
		if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
			Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE))

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
	_capture_suspended = false
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
	if _armory != null:
		_armory.close()
		_armory.teardown()  # the built menu holds the play world's resource root
	if _hud_host != null:
		_hud_host.teardown()
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


# Track the play panel with the overlay: the overlay lives on a CanvasLayer (so
# it draws above the FP viewmodel pass), which cannot anchor to the panel — copy
# the rect instead. Cheap no-op compares; runs every frame while mounted.
func _sync_overlay_rect() -> void:
	if _ui_overlay == null or _container == null:
		return
	var r := _container.get_global_rect()
	if _ui_overlay.global_position != r.position:
		_ui_overlay.global_position = r.position
	if _ui_overlay.size != r.size:
		_ui_overlay.size = r.size


# The literal game-shell per-frame order (main_game._process): drive the loaded
# world's foliage + runtime + audio around the play camera.
func _process(delta: float) -> void:
	_sync_overlay_rect()
	if _playing and _world != null and _world.is_loaded():
		# The armory overlay frees the mouse like the debug overlay does — play
		# keeps ticking (LIVE armory, no world-stop leg) but input idles and the
		# per-tick capture pauses so the menu takes the clicks.
		var player_live := not _capture_suspended and not (_armory != null and _armory.is_open())
		if _player_host != null:
			_player_host.before_world_tick(delta, player_live, player_live)
		_world.tick(_camera.global_position, _camera.global_transform, delta)
		if _player_host != null:
			_player_host.after_world_tick()
		if _hud_host != null:
			_hud_host.tick()  # the shared per-frame HUD info rebuild


## The workspace flips this with its debug overlay: while the overlay is up the
## mouse stays visible (and the per-tick capture pauses) so its controls take
## clicks; closing it lets the next tick recapture. The world keeps running —
## the overlay is a live inspector, not a pause.
func set_capture_suspended(suspended: bool) -> void:
	_capture_suspended = suspended
	if suspended and Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
		Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


# While playing, the game owns the keyboard — the game shell's parity stage is
# _unhandled_key_input (main_game.gd), but inside the editor the shell UI (focused
# controls, menu accelerators, workspace key handlers) would consume the play keys
# first; _input-level interception while _playing reproduces the shell's effective
# priority. Esc still stops via the same path.
func _input(event: InputEvent) -> void:
	if not _playing:
		return
	if handle_viewport_input(event):
		get_viewport().set_input_as_handled()


func _unhandled_input(event: InputEvent) -> void:
	if handle_viewport_input(event):
		get_viewport().set_input_as_handled()


func handle_viewport_input(event: InputEvent) -> bool:
	if not _playing:
		return false
	# While the armory overlay is up, the MENU owns the input: only Esc is claimed
	# (close-and-resume, the game shell's gesture); every other event falls through
	# to the GUI stage so the weapon.mnu controls take the clicks.
	if _armory != null and _armory.is_open():
		if event is InputEventKey:
			var akey := event as InputEventKey
			if akey.pressed and not akey.echo and akey.keycode == KEY_ESCAPE:
				_armory.close()
				return true
		return false
	if event is InputEventKey:
		var key := event as InputEventKey
		if key.pressed and not key.echo and key.keycode == KEY_ESCAPE:
			stop_requested.emit()
			return true
		if key.pressed and not key.echo and key.keycode == KEY_F3:
			debug_overlay_requested.emit()
			return true
		# The armory key — the same binding the game shell owns (main_game.ARMORY_KEY,
		# the use-item key; retail default SHIFT), zone-gated inside the shared host
		# [orig: useitem action 177 @0x4e0b3f].
		if key.pressed and not key.echo and key.keycode == KEY_SHIFT:
			if _armory != null and _armory.try_open():
				return true
		if _player_host != null and _player_host.handle_key_input(event, true):
			return true
	# Mouse-look only while the play session owns the mouse — the game shell
	# gates on the captured mouse (main_game._unhandled_input); here capture is
	# re-asserted per tick unless the debug overlay suspended it, so suspension
	# IS the free-mouse state: the overlay takes the clicks, not the look.
	if _player_host != null and _player_host.handle_input(event, not _capture_suspended):
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
