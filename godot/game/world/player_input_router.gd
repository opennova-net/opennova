class_name PlayerInputRouter
extends RefCounted
const LocalPlayerPresenter := preload("res://game/world/local_player_presenter.gd")

# The local player's input sampling/routing, split out of LocalPlayerPresenter
# (W4-4): the movement-state sampling into the sim, the weapon trigger/switch
# edge latches, the gameplay keys (B/N/NVG/stance), mouse look, and mouse
# capture/release. The presenter keeps the camera cluster and the
# avatar/viewmodel presentation; the camera MODE is the sim's resolved word
# (the arbiter over the chase preference and the seat), mirrored by the
# presenter — no key here flips it. The presenter's before_world_tick /
# handle_key_input / handle_input stay the externally pinned names and delegate here.

# The world serves the sim; the presenter serves the presentation surfaces this
# router drives around the sample (fly-camera lock, model lifetime, the
# head-bone eye). Untyped for the same reason as the presenter's _world: GUT
# harness worlds serve value-only doubles.
var _world: GameWorld = null
var _presenter: LocalPlayerPresenter = null
var _input_source := Callable()
var _fire_was_held := false
var _autofire_env := OS.get_environment("NW_LAN_AUTOFIRE")
var _reload_was_down := false
var _scope_was_down := false
var _medic_was_down := false
var _look_delta := Vector2.ZERO
var _elapsed := 0.0
var _frame_sequence := 0
# The manual weapon-switch keys — the retail defaults from the shipped binding
# catalog: rows 28-36 Knife '1' / Secondary '2' / Primary '3' / Flashbang '4' /
# FragGrenade '5' / SmokeGrenade '6' / Accessory '7' / Detonator '8' / medpack '9'
# fire the category actions 201-209 (categories 1..9), rows 39/40 cycleweaponP '['
# / cycleweaponN ']' cycle prev/next [orig: input cases 200-210 @ 0x4e1144 ->
# Player_SwitchToWeaponByHandle((action-200)*65); cases 212/214 ->
# Player_CycleWeaponSlot @ 0x4dfe70; engine/runtime/controls k_catalog rows].
static var _WEAPON_CATEGORY_TOKENS: Array = Array(ControlsModel.weapon_category_tokens())
var _category_was_down := 0
var _cycle_prev_was_down := false
var _cycle_next_was_down := false
var _radar_out_was_down := false
var _radar_in_was_down := false
var _map_toggle_was_down := false


func setup(world: GameWorld, presenter: LocalPlayerPresenter) -> void:
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


func before_world_tick(delta: float, capture_mouse: bool = false,
		gameplay_input_active: bool = true) -> MissionFrameInput:
	var frame_input := MissionFrameInput.new()
	frame_input.delta_seconds = delta
	_frame_sequence += 1
	frame_input.sequence = _frame_sequence
	if _presenter == null:
		return frame_input
	if not _presenter.has_player():
		_presenter.set_fly_camera_locked(false)
		release_mouse_capture()
		_presenter.clear_models()
		_look_delta = Vector2.ZERO
		return frame_input
	_presenter.set_fly_camera_locked(true)
	if capture_mouse and Input.get_mouse_mode() != Input.MOUSE_MODE_CAPTURED:
		Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	_presenter.ensure_models()
	# A live UI overlay keeps the world ticking but must actively submit a neutral
	# movement frame. Skipping this call leaves the sim holding its previous input,
	# so a player who opened the armory while running would keep running under it.
	var state := _read_input_state() if gameplay_input_active else {}
	var sim = _sim()
	# Test automation (net-capture branch): NW_FLY_HOLD holds a movement key so an
	# unattended session can fly a helicopter the player has been seated in.
	# Value is a direction word: f/b/l/r. Inert without the env var.
	var fly_hold := OS.get_environment("NW_FLY_HOLD")
	# NW_FLY_HOLD2 + NW_FLY_HOLD_AT switch to a second direction word after N
	# seconds, so one unattended run can climb and then come back down -- the
	# only way to exercise the landing legs without a human at the stick.
	var hold_at := float(OS.get_environment("NW_FLY_HOLD_AT"))
	if hold_at > 0.0 and _elapsed >= hold_at:
		var second := OS.get_environment("NW_FLY_HOLD2")
		if not second.is_empty():
			fly_hold = second
	_elapsed += delta
	if not fly_hold.is_empty():
		# u/d ride the LEAN keys: retail packs lean_left/lean_right as MoveOrder
		# bits 0x40/0x80, and the aircraft mover reads those same two bits as
		# descend/ascend - the collective is the lean pair, overloaded.
		state = {
			"forward": fly_hold.contains("f"),
			"back": fly_hold.contains("b"),
			"left": fly_hold.contains("l"),
			"right": fly_hold.contains("r"),
			"lean_left": fly_hold.contains("d"),
			"lean_right": fly_hold.contains("u"),
		}
	frame_input.set_movement(
			_bool(state, "forward"),
			_bool(state, "back"),
			_bool(state, "left"),
			_bool(state, "right"),
			_bool(state, "lean_left"),
			_bool(state, "lean_right"),
			_bool(state, "jump"))
	frame_input.look_delta = _look_delta if gameplay_input_active else Vector2.ZERO
	# NW_FLY_TURN=<mouse counts per second> feeds a steady yaw so an unattended
	# session can fly a circuit. The pilot's steer target IS the look heading, so
	# turning the look turns the aircraft - no separate steer channel exists.
	var fly_turn := float(OS.get_environment("NW_FLY_TURN"))
	if fly_turn != 0.0:
		frame_input.look_delta = Vector2(fly_turn * delta, 0.0)
	_look_delta = Vector2.ZERO
	if sim != null:
		# Feed the sim the head-bone eye for the 3P anchor chase [orig: the chase target
		# is Position + CameraOffset @0x437b70; CameraOffset is the posed head bone,
		# computed sim-side in the original @0x4b6bb3 — in the port, the render skeleton is
		# the sample source (D-INF-18)].
		# Feed the head RELATIVE TO THE AVATAR ROOT. The render skeleton is a
		# frame behind the sim, so an absolute head point carries a frame of
		# travel with it - invisible on foot, but 5-10 u in a helicopter, which
		# put the cockpit camera behind the aircraft. A body-relative delta
		# carries none and the sim re-anchors it to the live position.
		var head: Vector3 = _presenter.avatar_head_world()
		var root: Vector3 = _presenter.avatar_root_world()
		sim.set_local_player_eye(head if head != Vector3.INF else Vector3.ZERO,
				head != Vector3.INF)
		# ...and the SAME sample as a body-relative delta. The render skeleton is
		# a frame behind the sim, so an absolute head carries a frame of travel:
		# invisible on foot, 5-10 u in a helicopter, which put the cockpit camera
		# behind the aircraft. The delta carries none, and the sim re-anchors it
		# to the live position for the seated eye.
		var delta_ok := head != Vector3.INF and root != Vector3.INF
		sim.set_local_player_eye_offset(head - root if delta_ok else Vector3.ZERO,
				delta_ok)
	_sample_weapon_input(frame_input, gameplay_input_active)
	_sample_hud_input(gameplay_input_active)
	return frame_input


# The weapon trigger input: LMB fire (held + edge), R reload (raw edge — the
# full-magazine/empty-reserve refusal is the SIM's dispatch gate), RMB the ADS
# toggle REQUEST (the sim gates it and owns the engaged state). Only while the
# mouse is captured - UI clicks never fire. [orig: the binding dispatch cases
# 0x95 fire / 0xD3 reload / 6 scope, Input_HandleActionBinding_0 @0x4e0420 —
# ported in engine/runtime/world weapon_fsm + Simulation]
func _sample_weapon_input(frame_input: MissionFrameInput,
		gameplay_input_active: bool) -> void:
	var sim = _sim()
	var captured := gameplay_input_active \
			and Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED
	var fire_held := captured and Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT)
	# Test automation (net-capture branch): NW_LAN_AUTOFIRE holds the trigger in
	# bursts so an unattended self-test session exercises the fire -> damage ->
	# death -> S2C 0x13/0x1E chain. This mission's combat is player-driven, so a
	# passive bot joiner can never reach that path. Inert without the env var.
	if not _autofire_env.is_empty():
		fire_held = int(Time.get_ticks_msec() / 400) % 3 != 0
	var fire_edge := fire_held and not _fire_was_held
	_fire_was_held = fire_held
	var reload_down := captured and ControlsBindings.pressed("magazine")
	var reload_edge := reload_down and not _reload_was_down
	_reload_was_down = reload_down
	var scope_down := captured and Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)
	if scope_down and not _scope_was_down and sim != null:
		sim.request_local_player_scope_toggle()
	_scope_was_down = scope_down
	# The dead player's medic call: an action binding, so it samples whenever
	# the router runs — the death screen holds the mouse free and retail's
	# binding dispatch still fires it there; the sim's gates (dead + the
	# 310-tick cooldown) make a stray press inert.
	# [orig: Input_HandleActionBinding case 217 @0x49b4b4 (row 64 MedicReq)]
	var medic_down := ControlsBindings.pressed("MedicReq")
	var medic_edge := medic_down and not _medic_was_down
	_medic_was_down = medic_down
	# Latches update even with no sim (the deleted forwarders no-op'd downstream):
	# a key held across a mission reload must not fire a spurious edge on the
	# first frame the new sim appears.
	frame_input.set_weapon_input(fire_held, fire_edge, reload_edge, medic_edge)
	_send_weapon_switch_input(captured)


# The category keys (1..9) and the cycle pair ('['/']'): edge-triggered requests
# into the sim's switch walks; the sim applies the witnessed stance/FSM gates and
# answers through the event drain (switch_to_weapon / switch_denied).
func _send_weapon_switch_input(captured: bool) -> void:
	var sim = _sim()
	var down_mask := 0
	for i in _WEAPON_CATEGORY_TOKENS.size():
		if captured and ControlsBindings.pressed(_WEAPON_CATEGORY_TOKENS[i]):
			down_mask |= 1 << i
			if (_category_was_down & (1 << i)) == 0 and sim != null:
				sim.request_local_player_weapon_category(i + 1)
	_category_was_down = down_mask
	var prev_down := captured and ControlsBindings.pressed("cycleweaponP")
	if prev_down and not _cycle_prev_was_down and sim != null:
		sim.request_local_player_weapon_cycle(-1)
	_cycle_prev_was_down = prev_down
	var next_down := captured and ControlsBindings.pressed("cycleweaponN")
	if next_down and not _cycle_next_was_down and sim != null:
		sim.request_local_player_weapon_cycle(1)
	_cycle_next_was_down = next_down


# The retail radar-zoom bindings are ordinary configurable key rows applying
# one multiplicative step on the down edge: radarout GROWS the world-extent
# value (x1.15 toward 0x100000) and radarin shrinks it (x0.85 toward 4096).
# huddetail (dispatch code 19) is a live arm of the IN-GAME dispatcher, the
# declutter cycle, and GameHudPresenter samples it beside the other HUD rows
# (hud-re.md D-CTRL-4). [orig: Input_HandleActionBinding @0x49AD40 — radarout
#  row 48 = case 361 @0x49beaf, radarin row 49 = case 360 @0x49bcb0; code 19 ->
#  Input_HandleActionBinding_0 @0x4e060b..0x4e0624 -> CRenderState_SetLayerVisibility
#  @0x59B0F0]
func _sample_hud_input(active: bool) -> void:
	# The down-edge latches ride the RAW key state — retail's key scan
	# latches the device state and the context only gates which dispatcher
	# arm runs, so a key held across an armory/F3 window must NOT re-fire
	# when the gate reopens (the same rule the hudcolor poll follows).
	# [orig: the @0x49d1f0 scan's per-row down latch reads raw key state]
	var sim = _sim()
	var radar_out_down := ControlsBindings.pressed("radarout")
	if active and radar_out_down and not _radar_out_was_down and sim != null:
		sim.request_hud_radar_zoom(1)
	_radar_out_was_down = radar_out_down
	var radar_in_down := ControlsBindings.pressed("radarin")
	if active and radar_in_down and not _radar_in_was_down and sim != null:
		sim.request_hud_radar_zoom(-1)
	_radar_in_was_down = radar_in_down
	# map_toggle (row 98, default M) cycles the big-map mode: off -> the
	# north-up window -> fullscreen -> off. The retail arm lives in the
	# IN-GAME dispatcher, not the menu-context one.
	# [orig: row 98 code 28 -> the @0x4e0662 arm -> HUD_CycleMapMode
	#  @0x520bc0 (0->2->3->0)]
	var map_down := ControlsBindings.pressed("map_toggle")
	if active and map_down and not _map_toggle_was_down and sim != null:
		sim.request_hud_map_cycle()
	_map_toggle_was_down = map_down


# Edge-triggered gameplay keys. No key here moves the camera: the view rows
# (view1st F2 / viewwithgun F3 / viewchase F4) only write the chase PREFERENCE
# and the FP-gun bit, and the sim's arbiter resolves the mode from the
# preference and the seat — GameHudPresenter polls those rows beside its other
# HUD rows [orig: Input_HandleActionBinding cases 400/401/402 @0x49c073..
# 0x49c107; the arbiter Render_ProcessMainSceneFrame @0x5ca1d2; full 3P camera
# + torso-bend witness: docs/world/world-wac-ai-re.md §14 (D-INF-11), net-re
# §5.39]. Stance is the witnessed 3-key SELECT — Z prone, X crouch, C stand
# (catalog ids 9/10/11, defaults Z/X/C) — each key REQUESTS its stance from the
# sim, which applies the mutual exclusion and the ForceCrouch refusal (the C2S
# 0x1D semantics). [orig: input cases 170/169/172 @0x4e0df3/@0x4e0d77/@0x4e0e3e
# -> NapiNPServerMsg_HandleStanceChange @0x501c60]
func handle_key_input(event: InputEvent, active: bool) -> bool:
	if not active or _presenter == null or not _presenter.has_player() \
			or not (event is InputEventKey):
		return false
	var key := event as InputEventKey
	if not key.pressed or key.echo:
		return false
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
	# The stance ids' witness lives at the engine home, engine/runtime/world
	# infantry.h InfantryState::Stance (bound as Simulation.STANCE_*).
	if key.keycode == KEY_Z:
		_request_stance(Simulation.STANCE_PRONE)  # [orig: case 170 sends 0xAA]
		return true
	if key.keycode == KEY_X:
		_request_stance(Simulation.STANCE_CROUCH)  # [orig: case 169 sends 0xA9]
		return true
	if key.keycode == KEY_C:
		_request_stance(Simulation.STANCE_STAND)  # [orig: case 172 sends 0xAC]
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
	var mm := event as InputEventMouseMotion
	_look_delta += mm.relative
	return true


## Drop the trigger latches (the presenter's reset-state path). The switch-key
## latches are deliberately NOT reset: a category key held across a mission
## reload must not fire a spurious switch edge on the first new-sim frame.
func reset() -> void:
	_fire_was_held = false
	_reload_was_down = false
	_scope_was_down = false
	_look_delta = Vector2.ZERO


func release_mouse_capture() -> void:
	if Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
		Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


# The live binding table drives every key below (defaults: WASD move,
# Q/E lean, Space jump — see engine/runtime/controls k_catalog);
# jump is momentary (the motor jumps once when grounded). There is no run
# key: running is the automatic forward-walk promotion in the sim's body
# selection, suppressed while scoped.
# [orig: Player_PackInputStateToEntity @0x4df450; promotion @0x4b729d]
func _read_input_state() -> Dictionary:
	if _input_source.is_valid():
		var out = _input_source.call()
		if out is Dictionary:
			return out
	return {
		"forward": ControlsBindings.pressed("move_forward"),
		"back": ControlsBindings.pressed("move_back"),
		"left": ControlsBindings.pressed("strafe_left"),
		"right": ControlsBindings.pressed("strafe_right"),
		"lean_left": ControlsBindings.pressed("LeanRoll_left"),
		"lean_right": ControlsBindings.pressed("LeanRoll_right"),
		"jump": ControlsBindings.pressed("move_jump"),
	}


func _bool(state: Dictionary, key: String) -> bool:
	return bool(state.get(key, false))
