extends Node3D

# Runtime shell: boots into the game's menu front-end (NovaMenuHost, driving the
# .mnu menu set + audio from the chosen resource dir) and hands off to a GameWorld
# when the player starts a mission, with pause + return-to-menu on demand. The
# engine ships no game data; everything (menus, audio, terrain, missions) loads
# from the chosen resource dir. The first-launch directory picker lives here
# (runtime-only); headless probes set the dir explicitly and never block on it.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const DebugOverlayScript := preload("res://engine/debug/nova_debug_overlay.gd")
const NetKillFeedScript := preload("res://game/net_killfeed.gd")
const MissionObjectPlacer := preload("res://engine/mission/mission_object_placer.gd")

# Re-summon the game-folder picker. The original engine has no "change game dir"
# control (the game *is* its install folder); this is an OpenNova convenience so a
# wrong / menu-less folder can be re-picked without restarting. Front-end only.
const CHANGE_DIR_KEY := KEY_F9
# The mission debug overlay (entities / sim transport / script variables).
const DEBUG_OVERLAY_KEY := KEY_F3
# Phase 2.5 (the moving player, NOVA_PLAYER): faithful first-person camera. The eye is +1.0
# world unit above the player [orig: Camera_ComputeThirdPersonView @0x437d10]; F4 swaps to a
# behind+above third person [orig: ThirdPersonCamera_Update @0x437af0]. The mouse drives look
# yaw/pitch (pitch clamped ±80° [orig: Input_HandleActionBinding_0 @0x4e1330]).
const PLAYER_EYE_HEIGHT := 1.0          # +0x10000 = +1.0 world unit above Position
const PLAYER_PITCH_CLAMP_DEG := 80.0    # ±954437120 BAM
const PLAYER_MOUSE_SENS_DEG := 0.12     # degrees per mouse pixel (tunable)
const PLAYER_TP_DISTANCE := 5.0         # 3P camera distance behind the player
const PLAYER_TP_HEIGHT := 1.5           # 3P camera height bump
# First-person weapon viewmodel placement, witnessed from weapon.def `pos` (hip) / `tpos` (ADS).
# The original adds the equipped weapon's view-bias offset to the eye in view-local space, rotated by
# the view orientation, then draws the gun (gfx1) + character arms at that view root
# [orig: Player_UpdateFirstPersonCamera @0x4dd380 -> g_view_euler_translation_out;
# Player_RenderFirstPersonViewModel @0x4ded60]. The weapon.def parser stores the pos/tpos POSITION as
# `atof(str) * 256.0` (a 16.16 fixed-point world coord; scale flt_7D1D70 @0x544770) and the ROTATION
# as degrees -> 32-bit BAM (`* 0x0B60B60` = 2^32/360) [orig: weapon.def 'tpos' handler @0x54471f].
# The camera ftol's the stored float and adds it straight onto g_view_pos (16.16), so the net WORLD
# offset is simply `file_value / 256`. The view-local frame is (x = right, y = forward, z = up): the
# dominant `pos[2]` is the grip's DOWN offset (barrel reaches forward via the model), not depth — see
# `_viewmodel_offset` for the axis map and derivation. The Sighted/ADS path swaps `pos` -> `tpos`
# (WeaponDef.AltCamOffset @0x10C, read when entity Flags & 2).
# Units are WPN_AK47AUTO (REVX02\WEAPON.DEF) — hardcoded with the fixed-default model until a
# weapon.def Godot binding resolves the equipped weapon's pos/tpos per-weapon. (Swapped from WPN_MP5SD
# to confirm the placement generalizes; AK47AUTO pos is a near-pure vertical drop = a clean test.)
const WEAPON_DEF_POS_SCALE := 256.0                                    # flt_7D1D70: file unit -> /256 world units
const PLAYER_VIEWMODEL_POS_UNITS := Vector3(10.0, 0.0, -201.0)         # weapon.def `pos`  (hip)
const PLAYER_VIEWMODEL_TPOS_UNITS := Vector3(-28.046, 21.531, -187.857)  # weapon.def `tpos` (ADS/sighted)
# FP viewmodel model-facing rotation, euler DEGREES, camera-local. Our NovaObjectModel mesh is
# model-native (Y-up, only X-negated) so it must NOT get the world-object bms_to_godot_basis; this
# lays the gun barrel down-range relative to the view. The small per-weapon `pos`-rotation columns
# (Bone.rot, yaw/pitch/roll BAM — AK47AUTO = 0.0 / 0.0 / 1.0 deg) are a separate fine-tune, deferred.
const PLAYER_VIEWMODEL_ROT := Vector3(0.0, 180.0, 0.0)

enum State { MENU, WORLD, PAUSED }

@onready var _world: GameWorld = $World
@onready var _camera: Camera3D = $Camera3D
@onready var _hud: CanvasLayer = $HUD
@onready var _menu_host = $MenuLayer/MenuHost

var _picker: FileDialog
var _root: NovaResourceRoot
var _state: int = State.MENU
var _host_wired := false
var _debug_overlay  # NovaDebugOverlay, lazily built on the first F3
var _net_killfeed   # net spectator kill feed, built while in a net session
var _player_look_yaw := 0.0    # the local player's look yaw (mission deg), from the mouse
var _player_look_pitch := 0.0  # the local player's look pitch (deg), from the mouse, ±80°
var _player_third_person := false  # F4 toggles first/third person
var _player_avatar: Node3D = null  # host-managed soldier body (shown in 3P); null until built
var _player_viewmodel: Node3D = null  # host-managed FP arms+weapon (shown in 1P); null until built


func _ready() -> void:
	if _world == null or _camera == null or _menu_host == null:
		return
	# Esc toggles pause/resume in a world (the fly camera reports the key; the
	# host decides what it means).
	if _camera.has_signal("escape_pressed") and not _camera.is_connected("escape_pressed", _on_camera_escape):
		_camera.connect("escape_pressed", _on_camera_escape)
	# Net-replay connect mode: when NW_REPLAY is set (the env all F5/F6 instances
	# inherit from the editor), skip the menu and dial the replay tool / server
	# directly — each instance gets slotted into a role on connect.
	if not OS.get_environment("NW_REPLAY").is_empty():
		_enter_net_session()
		return
	var dir := ResourceDirSettings.get_resource_dir()
	if dir.is_empty():
		_request_resource_dir()
		return
	_enter_menu(dir)


# F9 (re)opens the asset-folder picker from the front-end so the player can point
# the runtime at a different game folder. Restricted to the menu state so an active
# mission is never yanked out from under a remount; ignored while a picker is open.
func _unhandled_key_input(event: InputEvent) -> void:
	var key := event as InputEventKey
	if key == null or not key.pressed or key.echo:
		return
	if key.keycode == CHANGE_DIR_KEY and _can_summon_dir_picker():
		_request_resource_dir()
		get_viewport().set_input_as_handled()
		return
	if key.keycode == DEBUG_OVERLAY_KEY:
		_toggle_debug_overlay()
		get_viewport().set_input_as_handled()
		return
	# F4 toggles first/third person for the local player [orig: dword_A890C8 mode flag].
	if key.keycode == KEY_F4 and _world.has_local_player():
		_player_third_person = not _player_third_person
		get_viewport().set_input_as_handled()


# F3: the mission debug overlay over the live runtime. Built lazily; without a
# running mission it just reports so (the runtime source re-resolves per
# refresh, so reloads and menu round-trips never leave it stale).
func _toggle_debug_overlay() -> void:
	if _debug_overlay == null:
		_debug_overlay = DebugOverlayScript.new()
		_debug_overlay.name = "DebugOverlay"
		var host: Node = _hud if _hud != null else self
		host.add_child(_debug_overlay)
		_debug_overlay.set_runtime_source(_current_runtime)
	_debug_overlay.toggle()


func _current_runtime():
	return _world.get_runtime() if _world != null else null


# Whether the folder picker may be summoned right now: only from the menu front-end
# and only when one is not already open. Pure predicate so it is unit-testable
# headless (the native dialog itself cannot be shown without a display).
func _can_summon_dir_picker() -> bool:
	return _state == State.MENU and _picker == null


# --- Menu state ---------------------------------------------------------------

func _enter_menu(dir: String) -> void:
	if _root == null or _root.get_root_dir() != dir:
		var root := _mount_runtime_root(dir)
		if root == null:
			_request_resource_dir()
			return
		_root = root
	_state = State.MENU
	_world.visible = false
	_set_hud_visible(false)
	_wire_host()
	if not _menu_host.setup(_root):
		push_warning("MainGame: no menu found in resource dir (looked for %s)" % _menu_host.main_menu_file)
	_menu_host.show_menu()


func _wire_host() -> void:
	if _host_wired:
		return
	_host_wired = true
	_menu_host.start_requested.connect(_on_start_requested)
	_menu_host.exit_to_desktop_requested.connect(_on_exit_to_desktop)
	_menu_host.return_to_menu_requested.connect(_on_return_to_menu)
	_menu_host.resume_requested.connect(_on_resume)
	if _menu_host.has_signal("novaworld_requested"):
		_menu_host.novaworld_requested.connect(_on_novaworld_requested)


# --- Resource dir picker (first launch) ---------------------------------------

func _request_resource_dir() -> void:
	if DisplayServer.get_name() == "headless" or _picker != null:
		return
	_picker = FileDialog.new()
	_picker.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	_picker.access = FileDialog.ACCESS_FILESYSTEM
	_picker.use_native_dialog = true
	_picker.title = "Select your OpenNova asset directory"
	_picker.dir_selected.connect(_on_dir_selected)
	_picker.canceled.connect(_on_dir_canceled)
	add_child(_picker)
	_picker.popup_centered_ratio(0.6)


func _on_dir_selected(dir: String) -> void:
	_cleanup_picker()
	var root := _mount_runtime_root(dir)
	if root == null:
		_request_resource_dir()
		return
	_root = root
	ResourceDirSettings.set_resource_dir(dir)
	_enter_menu(dir)


# Mount `dir` as the runtime resource root (packed PFFs, `/exp` expansion, `/d` loose
# override). Warns and returns null on failure.
func _mount_runtime_root(dir: String) -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	var expansion := NovaLaunchFlags.expansion(ResourceDirSettings.get_expansion())
	if root.mount_runtime(dir, expansion, NovaLaunchFlags.loose_override_enabled()) != OK:
		push_warning("MainGame: %s" % root.get_last_error())
		return null
	return root


func _on_dir_canceled() -> void:
	_cleanup_picker()
	_request_resource_dir()


func _cleanup_picker() -> void:
	if _picker != null:
		_picker.queue_free()
		_picker = null


# --- NovaWorld (online multiplayer) ------------------------------------------

var _novaworld_panel: NovaWorldPanel

func _on_novaworld_requested() -> void:
	if _novaworld_panel != null:
		return
	_novaworld_panel = NovaWorldPanel.new()
	# Dev default: localhost. A prod build sets the server host from the
	# resolved server IP before showing the panel.
	_menu_host.hide_menu()
	$MenuLayer.add_child(_novaworld_panel)
	_novaworld_panel.closed.connect(_on_novaworld_closed)


func _on_novaworld_closed() -> void:
	if _novaworld_panel != null:
		_novaworld_panel.queue_free()
		_novaworld_panel = null
	_menu_host.show_menu()


# --- Menu <-> world transitions ----------------------------------------------

func _on_start_requested(bms_name: String) -> void:
	_menu_host.hide_menu()
	_world.visible = true
	_set_hud_visible(true)
	_state = State.WORLD
	if not _world.world_loaded.is_connected(_on_world_loaded):
		_world.world_loaded.connect(_on_world_loaded)
	if not _world.load_failed.is_connected(_on_world_load_failed):
		_world.load_failed.connect(_on_world_load_failed)
	_world.load_mission(bms_name)


# Spectate a net session (no menu). The source (replay tool or a real server) is
# at NW_REPLAY="host:port"; the map name comes off the wire, so only the resource
# dir is needed: NW_REPLAY_DIR (else the persisted one), NW_REPLAY_LOOSE for a flat
# extract, and NW_REPLAY_ITEMS as an optional items.def override.
func _enter_net_session() -> void:
	var ep := OS.get_environment("NW_REPLAY")
	var parts := ep.split(":")
	_menu_host.hide_menu()
	_world.visible = true
	_set_hud_visible(true)
	_state = State.WORLD
	if not _world.world_loaded.is_connected(_on_world_loaded):
		_world.world_loaded.connect(_on_world_loaded)
	if not _world.load_failed.is_connected(_on_world_load_failed):
		_world.load_failed.connect(_on_world_load_failed)
	var err := _world.load_net_session({
		"replay_host": parts[0] if parts.size() > 0 else "127.0.0.1",
		"replay_port": int(parts[1]) if parts.size() > 1 else 42000,
		"dir": OS.get_environment("NW_REPLAY_DIR"),
		"loose": not OS.get_environment("NW_REPLAY_LOOSE").is_empty(),
		"items": OS.get_environment("NW_REPLAY_ITEMS"),
		"camera": _camera,
	})
	if err != OK:
		push_warning("MainGame: net session failed to start (%d)" % err)
		return
	# Kill feed over the spectator: reads the same decoded event stream NetEventView
	# draws in 3D, posting kill / objective lines to a top-right HUD feed.
	if _net_killfeed == null:
		_net_killfeed = NetKillFeedScript.new()
		_net_killfeed.name = "NetKillFeed"
		var host: Node = _hud if _hud != null else self
		host.add_child(_net_killfeed)
	_net_killfeed.set_client(_world.get_net_client())


func _on_world_loaded() -> void:
	_menu_host.enter_game_music()


func _on_world_load_failed(reason: String) -> void:
	push_warning("MainGame: mission load failed: %s" % reason)
	if _root != null:
		_enter_menu(_root.get_root_dir())


func _on_camera_escape() -> void:
	# Esc: pause <-> resume while in a world; ignored in the main menu (EXIT quits).
	if _state == State.WORLD:
		_pause()
	elif _state == State.PAUSED:
		_on_resume()


func _pause() -> void:
	_state = State.PAUSED
	_menu_host.open_ingame_menu()  # game.mnu overlay over the kept-loaded world
	_menu_host.show_menu()


func _on_resume() -> void:
	if _state != State.PAUSED:
		return
	_menu_host.hide_menu()
	_state = State.WORLD


func _on_return_to_menu() -> void:
	_world.unload()
	if _net_killfeed != null:
		_net_killfeed.queue_free()
		_net_killfeed = null
	if _root != null:
		_enter_menu(_root.get_root_dir())


func _on_exit_to_desktop() -> void:
	get_tree().quit()


# CanvasLayer contents toggle: hide/show the HUD's CanvasItem children (the FPS
# label + debug label) so they do not draw over the menu.
func _set_hud_visible(v: bool) -> void:
	if _hud == null:
		return
	for c in _hud.get_children():
		if c is CanvasItem:
			(c as CanvasItem).visible = v


# Drive the loaded world's per-frame foliage coverage. Tick whenever a world is
# loaded and not paused (the pause menu freezes it); tick() itself no-ops until the
# world finishes loading. Gating on "loaded, not paused" rather than State.WORLD
# also lets a host that drives load_world() directly (the headless runtime probe,
# which stays in MENU) keep dispatching foliage.
func _process(delta: float) -> void:
	# Release the captured mouse while paused / unloaded so the menus stay usable.
	if _state == State.PAUSED or not _world.is_loaded():
		if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
			Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
		return
	# Phase 2.5 (NOVA_PLAYER): drive the local player and follow it with the first-person
	# camera. Input is set BEFORE tick() so the sim applies it net-before-logic this frame.
	var has_player: bool = _world.has_local_player()
	if has_player:
		if _state == State.WORLD and Input.get_mouse_mode() != Input.MOUSE_MODE_CAPTURED:
			Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
		if _player_avatar == null:
			_player_avatar = _world.build_local_player_avatar()
		if _player_viewmodel == null:
			_player_viewmodel = _world.build_local_player_viewmodel()
		_drive_local_player(delta)
	elif _player_avatar != null or _player_viewmodel != null:
		if _player_avatar != null:
			_player_avatar.queue_free()
			_player_avatar = null
		if _player_viewmodel != null:
			_player_viewmodel.queue_free()
			_player_viewmodel = null
	_world.tick(_camera.global_position, _camera.global_transform)
	if has_player:
		_update_player_camera()


# WASD is the 8-way move relative to the look (W/S forward/back, A/D strafe); the mouse turns
# the look (see _unhandled_input). Shift runs. [orig: Player_PackInputStateToEntity @0x4df450]
func _drive_local_player(_delta: float) -> void:
	var fwd: bool = Input.is_key_pressed(KEY_W)
	var back: bool = Input.is_key_pressed(KEY_S)
	var left: bool = Input.is_key_pressed(KEY_A)
	var right: bool = Input.is_key_pressed(KEY_D)
	var run: bool = Input.is_key_pressed(KEY_SHIFT)
	_world.set_local_player_input(fwd, back, left, right, run, _player_look_yaw, _player_look_pitch)


# Mouse-look: turn the look yaw (X) and pitch (Y, clamped ±80°). [orig: mouse -> entity
# Yaw@+0x10 / Pitch@+0x14, Input_HandleActionBinding_0 @0x4e1330]. Signs are tunable.
func _unhandled_input(event: InputEvent) -> void:
	if not (event is InputEventMouseMotion):
		return
	if _state != State.WORLD or not _world.is_loaded() or not _world.has_local_player():
		return
	if Input.get_mouse_mode() != Input.MOUSE_MODE_CAPTURED:
		return
	var mm := event as InputEventMouseMotion
	_player_look_yaw += mm.relative.x * PLAYER_MOUSE_SENS_DEG
	_player_look_pitch = clampf(_player_look_pitch - mm.relative.y * PLAYER_MOUSE_SENS_DEG,
		-PLAYER_PITCH_CLAMP_DEG, PLAYER_PITCH_CLAMP_DEG)


# Place the camera from the player's authoritative pose. First person: eye = player + 1.0u
# looking along the facing. Third person (F4): behind + above, looking at the player. The
# mission yaw -> Godot forward mirrors the present remap (x,y,z)->(x,z,-y): a mission facing
# yaw faces (sin yaw, cos yaw) -> Godot (sin yaw, 0, -cos yaw), tilted by pitch.
func _update_player_camera() -> void:
	var pos: Vector3 = _world.local_player_position()
	var yr := deg_to_rad(_world.local_player_yaw_deg())
	var pr := deg_to_rad(_world.local_player_pitch_deg())
	var forward := Vector3(sin(yr) * cos(pr), sin(pr), -cos(yr) * cos(pr))
	var eye := pos + Vector3(0, PLAYER_EYE_HEIGHT, 0)
	if _player_third_person:
		_camera.global_position = eye - forward * PLAYER_TP_DISTANCE + Vector3(0, PLAYER_TP_HEIGHT, 0)
		_camera.look_at(eye, Vector3.UP)
	else:
		_camera.global_position = eye
		_camera.look_at(eye + forward, Vector3.UP)
	# Host-managed avatar: stand it at the player facing the look yaw (the body doesn't pitch);
	# shown in third person, hidden in first (the FP arms viewmodel is a later weapon-phase step).
	if _player_avatar != null and is_instance_valid(_player_avatar):
		_player_avatar.global_position = pos
		_player_avatar.global_basis = MissionObjectPlacer.bms_to_godot_basis(
			Vector3(0.0, _world.local_player_yaw_deg(), 0.0))
		_player_avatar.visible = _player_third_person
		# Drive the body clip from the player's authoritative anim slot (idle/walk/run) — the same
		# slot the present pass feeds NPC models. The model self-ticks its skeleton via _process, so
		# we only SELECT the clip here (play_body_anim is idempotent per tick; do not also advance).
		if _player_avatar.has_method("play_body_anim"):
			_player_avatar.play_body_anim(_world.local_player_anim_slot())
	# First-person weapon viewmodel: sit it in front of the eye, tracking the camera 1:1, shown in
	# first person only (hidden in 3P, where the body avatar shows instead). The original biases the
	# CAMERA by the weapon's `pos`/`tpos` view offset and draws the model at the view root
	# [orig: Player_UpdateFirstPersonCamera @0x4dd380]; placing it in camera space is the faithful
	# structural equivalent (camera.global_transform == the engine view transform here).
	if _player_viewmodel != null and is_instance_valid(_player_viewmodel):
		var vm_basis := Basis.from_euler(Vector3(
			deg_to_rad(PLAYER_VIEWMODEL_ROT.x), deg_to_rad(PLAYER_VIEWMODEL_ROT.y), deg_to_rad(PLAYER_VIEWMODEL_ROT.z)))
		var vm_offset := _viewmodel_offset(PLAYER_VIEWMODEL_POS_UNITS)  # TODO: -> TPOS when ADS (entity Flags & 2)
		_player_viewmodel.global_transform = _camera.global_transform * Transform3D(vm_basis, vm_offset)
		_player_viewmodel.visible = not _player_third_person


# Convert a weapon.def `pos`/`tpos` POSITION (raw file units) into a Godot camera-local offset.
# Faithful to the witnessed pipeline [orig: Player_UpdateFirstPersonCamera @0x4dd380; scale
# flt_7D1D70=256 @0x544770]: the camera adds `ftol(Bone.pos)` straight onto g_view_pos, and at a
# level look the view matrix is identity [orig: Math_BuildFixedPointRotationMatrixYXZ @0x615400], so
# component i lands on world axis i (world Z = up). The view-local frame is therefore
# (x = right, y = forward, z = up) — `pos[2]` is the grip's DOWN offset (the dominant term; the barrel
# reaches forward via the model), NOT depth. Godot camera-local is (x right, y up, -z forward), so:
#   file x (right)   -> Godot  x
#   file y (forward) -> Godot -z
#   file z (up)      -> Godot  y      (e.g. MP5SD pos.z -183 -> grip ~0.715u below the eye)
# The two small lateral/forward terms (x, y) are sign-confirmable by drive; the z->y (down) term is
# the certain one. (oscarmike WeaponManager._jo_to_godot_position agrees on /256 + z->up/down.)
func _viewmodel_offset(units: Vector3) -> Vector3:
	return Vector3(units.x, units.z, -units.y) / WEAPON_DEF_POS_SCALE
