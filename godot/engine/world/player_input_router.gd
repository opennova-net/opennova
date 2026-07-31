class_name PlayerInputRouter
extends RefCounted

# The local player's input sampling/routing, split out of LocalPlayerPresenter
# (W4-4): the movement-state sampling into the sim, the weapon trigger/switch
# edge latches, the gameplay keys (F4/B/N/NVG/stance), mouse look, and mouse
# capture/release. The presenter keeps the camera cluster, the avatar/viewmodel
# presentation, and the third-person FLAG (camera state) — F4 flips it through
# presenter.set_third_person(). The presenter's before_world_tick / handle_key_input /
# handle_input stay the externally pinned names and delegate here.

# The world serves the sim; the presenter serves the presentation surfaces this
# router drives around the sample (fly-camera lock, model lifetime, the
# head-bone eye). Untyped for the same reason as the presenter's _world: GUT
# harness worlds serve value-only doubles.
var _world
var _presenter
var _input_source := Callable()
var _fire_was_held := false
var _reload_was_down := false
var _scope_was_down := false
# The manual weapon-switch keys — the retail defaults from the shipped binding
# catalog: rows 28-36 Knife '1' / Secondary '2' / Primary '3' / Flashbang '4' /
# FragGrenade '5' / SmokeGrenade '6' / Accessory '7' / Detonator '8' / medpack '9'
# fire the category actions 201-209 (categories 1..9), rows 39/40 cycleweaponP '['
# / cycleweaponN ']' cycle prev/next [orig: input cases 200-210 @ 0x4e1144 ->
# Player_SwitchToWeaponByHandle((action-200)*65); cases 212/214 ->
# Player_CycleWeaponSlot @ 0x4dfe70; libs/controls k_catalog rows].
const _WEAPON_CATEGORY_KEYS: Array[Key] = [KEY_1, KEY_2, KEY_3, KEY_4, KEY_5,
		KEY_6, KEY_7, KEY_8, KEY_9]
var _category_was_down := 0
var _cycle_prev_was_down := false
var _cycle_next_was_down := false


func setup(world, presenter) -> void:
	_world = world
	_presenter = presenter


func teardown() -> void:
	_world = null
	_presenter = null
	_input_source = Callable()


# The sim, re-resolved per use: mission reloads free the runtime and its sim,
# so a cached reference would go stale (the presenter follows the same rule).
# Untyped: GUT harness worlds serve value-only sim doubles.
func _sim():
	return _world.get_sim() if _world != null else null


func set_input_source(source: Callable) -> void:
	_input_source = source


func before_world_tick(_delta: float, capture_mouse: bool = false,
		gameplay_input_active: bool = true) -> void:
	if _presenter == null:
		return
	if not _presenter.has_player():
		_presenter.set_fly_camera_locked(false)
		release_mouse_capture()
		_presenter.clear_models()
		return
	_presenter.set_fly_camera_locked(true)
	if capture_mouse and Input.get_mouse_mode() != Input.MOUSE_MODE_CAPTURED:
		Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	_presenter.ensure_models()
	# A live UI overlay keeps the world ticking but must actively submit a neutral
	# movement frame. Skipping this call leaves the sim holding its previous input,
	# so a player who opened the armory while running would keep running under it.
	var state := _read_input_state() if gameplay_input_active else {}
	var sim = _sim()
	if sim != null:
		sim.set_player_input(
			_bool(state, "forward"),
			_bool(state, "back"),
			_bool(state, "left"),
			_bool(state, "right"),
			_bool(state, "lean_left"),
			_bool(state, "lean_right"),
			_bool(state, "jump"))
		# Feed the sim the head-bone eye for the 3P anchor chase [orig: the chase target
		# is Position + CameraOffset @0x437b70; CameraOffset is the posed head bone,
		# computed sim-side in the original @0x4b6bb3 — in the port, the render skeleton is
		# the sample source (D-INF-18)].
		var head: Vector3 = _presenter.avatar_head_world()
		sim.set_local_player_eye(head if head != Vector3.INF else Vector3.ZERO,
				head != Vector3.INF)
	_send_weapon_input()


# The weapon trigger input: LMB fire (held + edge), R reload (raw edge — the
# full-magazine/empty-reserve refusal is the SIM's dispatch gate), RMB the ADS
# toggle REQUEST (the sim gates it and owns the engaged state). Only while the
# mouse is captured - UI clicks never fire. [orig: the binding dispatch cases
# 0x95 fire / 0xD3 reload / 6 scope, Input_HandleActionBinding_0 @0x4e0420 —
# ported in libs/world weapon_fsm + NovaSimulation]
func _send_weapon_input() -> void:
	var sim = _sim()
	var captured := Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED
	var fire_held := captured and Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)
	var fire_edge := fire_held and not _fire_was_held
	_fire_was_held = fire_held
	var reload_down := captured and Input.is_physical_key_pressed(KEY_R)
	var reload_edge := reload_down and not _reload_was_down
	_reload_was_down = reload_down
	var scope_down := captured and Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)
	if scope_down and not _scope_was_down and sim != null:
		sim.request_local_player_scope_toggle()
	_scope_was_down = scope_down
	# Latches update even with no sim (the deleted forwarders no-op'd downstream):
	# a key held across a mission reload must not fire a spurious edge on the
	# first frame the new sim appears.
	if sim != null:
		sim.set_local_player_weapon_input(fire_held, fire_edge, reload_edge)
	_send_weapon_switch_input(captured)


# The category keys (1..9) and the cycle pair ('['/']'): edge-triggered requests
# into the sim's switch walks; the sim applies the witnessed stance/FSM gates and
# answers through the event drain (switch_to_weapon / switch_denied).
func _send_weapon_switch_input(captured: bool) -> void:
	var sim = _sim()
	var down_mask := 0
	for i in _WEAPON_CATEGORY_KEYS.size():
		if captured and Input.is_physical_key_pressed(_WEAPON_CATEGORY_KEYS[i]):
			down_mask |= 1 << i
			if (_category_was_down & (1 << i)) == 0 and sim != null:
				sim.request_local_player_weapon_category(i + 1)
	_category_was_down = down_mask
	var prev_down := captured and Input.is_physical_key_pressed(KEY_BRACKETLEFT)
	if prev_down and not _cycle_prev_was_down and sim != null:
		sim.request_local_player_weapon_cycle(-1)
	_cycle_prev_was_down = prev_down
	var next_down := captured and Input.is_physical_key_pressed(KEY_BRACKETRIGHT)
	if next_down and not _cycle_next_was_down and sim != null:
		sim.request_local_player_weapon_cycle(1)
	_cycle_next_was_down = next_down


# Edge-triggered gameplay keys. F4 toggles first/third person [orig: g_camera_mode
# @ 0xA890C8; view actions 400/402/412 @ 0x49C073; ThirdPersonCamera_Update @0x437af0
# — full 3P camera + torso-bend witness: docs/world/world-wac-ai-re.md §14 (D-INF-11),
# net-re §5.39 2026-07-08 addendum]. The third-person flag is CAMERA state and stays
# on the presenter — F4 flips it through presenter.set_third_person(). Stance is the witnessed
# 3-key SELECT — Z prone, X crouch, C stand (catalog ids 9/10/11, defaults Z/X/C) —
# each key REQUESTS its stance from the sim, which applies the mutual exclusion and
# the ForceCrouch refusal (the C2S 0x1D semantics). [orig: input cases 170/169/172
# @0x4e0df3/@0x4e0d77/@0x4e0e3e -> NapiNPServerMsg_HandleStanceChange @0x501c60]
func handle_key_input(event: InputEvent, active: bool) -> bool:
	if not active or _presenter == null or not _presenter.has_player() \
			or not (event is InputEventKey):
		return false
	var key := event as InputEventKey
	if not key.pressed or key.echo:
		return false
	if key.keycode == KEY_F4:
		_presenter.set_third_person(not _presenter.is_third_person())
		return true
	var physical := key.physical_keycode if key.physical_keycode != 0 else key.keycode
	var sim = _sim()
	if physical == KEY_B:
		if sim != null:
			sim.request_local_player_binoculars_toggle()
		return true
	if physical == KEY_N:
		if sim != null:
			sim.request_local_player_nvg_toggle()
		return true
	if physical == KEY_EQUAL or physical == KEY_PLUS or physical == KEY_KP_ADD:
		if sim != null:
			sim.request_local_player_nvg_gain(1)
		return true
	if physical == KEY_MINUS or physical == KEY_KP_SUBTRACT:
		if sim != null:
			sim.request_local_player_nvg_gain(-1)
		return true
	if key.keycode == KEY_Z:
		_request_stance(2)  # prone [orig: case 170 sends 0xAA]
		return true
	if key.keycode == KEY_X:
		_request_stance(1)  # crouch [orig: case 169 sends 0xA9]
		return true
	if key.keycode == KEY_C:
		_request_stance(0)  # stand [orig: case 172 sends 0xAC]
		return true
	return false


func _request_stance(stance: int) -> void:
	var sim = _sim()
	if sim != null:
		sim.request_local_player_stance(stance)


# Mouse-look: raw pixel deltas into the SIM's witnessed integer pipeline (the sim
# owns sensitivity, the scoped zoom reduction, Y-invert, and the pitch clamps).
# [orig: Input_ProcessMouseAxisBindings @0x499680 -> the axis cases 166/164]
func handle_input(event: InputEvent, active: bool) -> bool:
	if not active or _presenter == null or not _presenter.has_player() \
			or not (event is InputEventMouseMotion):
		return false
	var sim = _sim()
	if sim == null:
		return false
	var mm := event as InputEventMouseMotion
	sim.add_local_player_look(mm.relative.x, mm.relative.y)
	return true


## Drop the trigger latches (the presenter's reset-state path). The switch-key
## latches are deliberately NOT reset: a category key held across a mission
## reload must not fire a spurious switch edge on the first new-sim frame.
func reset() -> void:
	_fire_was_held = false
	_reload_was_down = false
	_scope_was_down = false


func release_mouse_capture() -> void:
	if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
		Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


# WASD is the 8-way move relative to the look (W/S forward/back, A/D strafe);
# Q/E lean (catalog ids 6/7); Space jumps (momentary — the motor jumps once when
# grounded). There is no run key: running is the automatic forward-walk promotion
# in the sim's body selection, suppressed while scoped.
# [orig: Player_PackInputStateToEntity @0x4df450; promotion @0x4b729d]
func _read_input_state() -> Dictionary:
	if _input_source.is_valid():
		var out = _input_source.call()
		if out is Dictionary:
			return out
	return {
		"forward": Input.is_physical_key_pressed(KEY_W),
		"back": Input.is_physical_key_pressed(KEY_S),
		"left": Input.is_physical_key_pressed(KEY_A),
		"right": Input.is_physical_key_pressed(KEY_D),
		"lean_left": Input.is_physical_key_pressed(KEY_Q),
		"lean_right": Input.is_physical_key_pressed(KEY_E),
		"jump": Input.is_physical_key_pressed(KEY_SPACE),
	}


func _bool(state: Dictionary, key: String) -> bool:
	return bool(state.get(key, false))
