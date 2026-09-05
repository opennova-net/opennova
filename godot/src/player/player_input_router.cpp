#include "player/player_input_router.h"

#include "player/local_player_presenter.h"
#include "simulation/simulation.h"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/global_constants.hpp>

#include <cmath>

#include <runtime/world/player_present.h>

using namespace godot;

namespace {

const Vector3 kNoSample(INFINITY, INFINITY, INFINITY);

} // namespace

PlayerInputRouter::PlayerInputRouter() {
	weapon_category_tokens_ = ControlsModel::weapon_category_tokens();
}

void PlayerInputRouter::setup(Node *p_world, LocalPlayerPresenter *p_presenter,
		const Ref<ControlsModel> &p_controls) {
	world_id_ = p_world != nullptr ? ObjectID(p_world->get_instance_id()) : ObjectID();
	presenter_id_ = p_presenter != nullptr ? ObjectID(p_presenter->get_instance_id()) : ObjectID();
	controls_ = p_controls;
}

void PlayerInputRouter::teardown() {
	world_id_ = ObjectID();
	presenter_id_ = ObjectID();
	controls_.unref();
	input_override_.unref();
}

// The sim, re-resolved per use: mission reloads free the runtime and its sim,
// so a cached reference would go stale (the presenter follows the same rule).
Ref<Simulation> PlayerInputRouter::sim() const {
	Node *world = Object::cast_to<Node>(ObjectDB::get_instance(world_id_));
	if (world == nullptr) {
		return Ref<Simulation>();
	}
	return Ref<Simulation>(world->call("get_sim"));
}

LocalPlayerPresenter *PlayerInputRouter::presenter() const {
	return Object::cast_to<LocalPlayerPresenter>(ObjectDB::get_instance(presenter_id_));
}

bool PlayerInputRouter::pressed(const char *p_token) const {
	return controls_.is_valid() && controls_->is_token_pressed(p_token);
}

void PlayerInputRouter::set_input_override(const Ref<PlayerMoveIntent> &p_intent) {
	input_override_ = p_intent;
}

Ref<MissionFrameInput> PlayerInputRouter::before_world_tick(double p_delta, bool p_capture_mouse,
		bool p_gameplay_input_active) {
	Ref<MissionFrameInput> frame_input;
	frame_input.instantiate();
	frame_input->set_delta_seconds(p_delta);
	++frame_sequence_;
	frame_input->set_sequence(frame_sequence_);
	LocalPlayerPresenter *owner = presenter();
	if (owner == nullptr) {
		return frame_input;
	}
	if (!owner->has_player()) {
		owner->set_fly_camera_locked(false);
		release_mouse_capture();
		owner->clear_models();
		look_delta_ = Vector2();
		return frame_input;
	}
	const Ref<Simulation> tick_sim = sim();
	if (tick_sim.is_valid() && tick_sim->is_local_spectator()) {
		// A spectator owns no body motor. Submit the neutral frame while the
		// existing FlyCamera consumes the viewport input; the world/session
		// cadence continues through GameWorld.tick as normal.
		owner->set_fly_camera_locked(false);
		release_mouse_capture();
		owner->clear_models();
		look_delta_ = Vector2();
		return frame_input;
	}
	owner->set_fly_camera_locked(true);
	Input *input = Input::get_singleton();
	if (p_capture_mouse && input->get_mouse_mode() != Input::MOUSE_MODE_CAPTURED) {
		input->set_mouse_mode(Input::MOUSE_MODE_CAPTURED);
	}
	owner->ensure_models();
	// A live UI overlay keeps the world ticking but must actively submit a
	// neutral movement frame. Skipping this call leaves the sim holding its
	// previous input, so a player who opened the armory while running would
	// keep running under it. Scripted input (probes, automation) arrives
	// through set_input_override: the same seven bits a device produces, so
	// the sim sees one contract. u/d ride the LEAN keys there: retail packs
	// lean_left/lean_right as MoveOrder bits 0x40/0x80, and the aircraft mover
	// reads those same two bits as descend/ascend - the collective is the lean
	// pair, overloaded.
	bool forward = false, back = false, left = false, right = false;
	bool lean_left = false, lean_right = false, jump = false;
	if (p_gameplay_input_active) {
		read_input_state(forward, back, left, right, lean_left, lean_right, jump);
	}
	frame_input->set_movement(forward, back, left, right, lean_left, lean_right, jump);
	frame_input->set_look_delta(p_gameplay_input_active ? look_delta_ : Vector2());
	look_delta_ = Vector2();
	if (tick_sim.is_valid()) {
		// Feed the sim the head-bone eye for the 3P anchor chase [orig: the
		// chase target is Position + CameraOffset @0x437b70; CameraOffset is
		// the posed head bone, computed sim-side in the original @0x4b6bb3 --
		// in the port, the render skeleton is the sample source (D-INF-18)].
		// Feed the head RELATIVE TO THE AVATAR ROOT. The render skeleton is a
		// frame behind the sim, so an absolute head point carries a frame of
		// travel with it - invisible on foot, but 5-10 u in a helicopter, which
		// put the cockpit camera behind the aircraft. A body-relative delta
		// carries none and the sim re-anchors it to the live position.
		const Vector3 head = owner->avatar_head_world();
		const Vector3 root = owner->avatar_root_world();
		const bool head_ok = head != kNoSample;
		tick_sim->set_local_player_eye(head_ok ? head : Vector3(), head_ok);
		// ...and the SAME sample as a body-relative delta. The render skeleton
		// is a frame behind the sim, so an absolute head carries a frame of
		// travel: invisible on foot, 5-10 u in a helicopter, which put the
		// cockpit camera behind the aircraft. The delta carries none, and the
		// sim re-anchors it to the live position for the seated eye.
		const bool delta_ok = head_ok && root != kNoSample;
		tick_sim->set_local_player_eye_offset(delta_ok ? head - root : Vector3(), delta_ok);
	}
	sample_weapon_input(frame_input, p_gameplay_input_active);
	sample_hud_input(p_gameplay_input_active);
	return frame_input;
}

// The weapon trigger input: LMB fire (held + edge), R reload (raw edge -- the
// full-magazine/empty-reserve refusal is the SIM's dispatch gate), RMB the
// ADS toggle REQUEST (the sim gates it and owns the engaged state). Only
// while the mouse is captured - UI clicks never fire. [orig: the binding
// dispatch cases 0x95 fire / 0xD3 reload / 6 scope,
// Input_HandleActionBinding_0 @0x4e0420 -- ported in engine/runtime/world
// weapon_fsm + Simulation]
void PlayerInputRouter::sample_weapon_input(const Ref<MissionFrameInput> &p_frame_input,
		bool p_gameplay_input_active) {
	const Ref<Simulation> weapon_sim = sim();
	Input *input = Input::get_singleton();
	const bool captured = p_gameplay_input_active &&
			input->get_mouse_mode() == Input::MOUSE_MODE_CAPTURED;
	const bool fire_held = captured && input->is_mouse_button_pressed(MOUSE_BUTTON_LEFT);
	const bool fire_edge = fire_held && !fire_was_held_;
	fire_was_held_ = fire_held;
	const bool reload_down = captured && pressed("magazine");
	const bool reload_edge = reload_down && !reload_was_down_;
	reload_was_down_ = reload_down;
	// The RMB scope request shadows the configurable `scope` row (catalog row
	// 105): the raw button is read, not the binding (ported verbatim; a
	// tracked divergence follow-up).
	const bool scope_down = captured && input->is_mouse_button_pressed(MOUSE_BUTTON_RIGHT);
	if (scope_down && !scope_was_down_ && weapon_sim.is_valid()) {
		weapon_sim->request_local_player_scope_toggle();
	}
	scope_was_down_ = scope_down;
	// The dead player's medic call: an action binding, so it samples whenever
	// the router runs -- the death screen holds the mouse free and retail's
	// binding dispatch still fires it there; the sim's gates (dead + the
	// 310-tick cooldown) make a stray press inert.
	// [orig: Input_HandleActionBinding case 217 @0x49b4b4 (row 64 MedicReq)]
	const bool medic_down = pressed("MedicReq");
	const bool medic_edge = medic_down && !medic_was_down_;
	medic_was_down_ = medic_down;
	// Latches update even with no sim (the deleted forwarders no-op'd
	// downstream): a key held across a mission reload must not fire a spurious
	// edge on the first frame the new sim appears.
	p_frame_input->set_weapon_input(fire_held, fire_edge, reload_edge, medic_edge);
	send_weapon_switch_input(captured);
}

// The category keys (1..9) and the cycle pair ('['/']'): edge-triggered
// requests into the sim's switch walks; the sim applies the witnessed
// stance/FSM gates and answers through the event drain (switch_to_weapon /
// switch_denied).
void PlayerInputRouter::send_weapon_switch_input(bool p_captured) {
	const Ref<Simulation> switch_sim = sim();
	// The seat rows default to Ctrl+1..Ctrl+0 and the weapon categories to the
	// bare digits; the binding's own modifier keeps them apart, so neither
	// side is gated on the mount state here. Off a mount the seat action is
	// an engine no-op (no slot list), and a control seat refuses the category
	// switch inside the engine's walk, as retail does.
	// [orig: Input_HandleActionBinding_0 cases 0xB6..0xBF @0x4E0B81..0x4E0C22;
	//  Player_SwitchToWeaponByHandle parentSlot gate @0x4e0192]
	static const char *seat_tokens[] = {"seat1", "seat2", "seat3", "seat4", "seat5",
			"seat6", "seat7", "seat8", "seat9", "seat10"};
	for (int i = 0; i < 10; ++i) {
		if (opennova::world::latched_key_edge(pressed(seat_tokens[i]), p_captured,
				seat_was_down_[i]) && switch_sim.is_valid())
			switch_sim->local_player_select_seat(i);
	}
	int down_mask = 0;
	for (int64_t i = 0; i < weapon_category_tokens_.size(); ++i) {
		if (p_captured && pressed(weapon_category_tokens_[i].utf8().get_data())) {
			down_mask |= 1 << i;
			if ((category_was_down_ & (1 << i)) == 0 && switch_sim.is_valid()) {
				switch_sim->request_local_player_weapon_category(
						static_cast<Simulation::WeaponCategory>(i + 1));
			}
		}
	}
	category_was_down_ = down_mask;
	const bool prev_down = p_captured && pressed("cycleweaponP");
	if (prev_down && !cycle_prev_was_down_ && switch_sim.is_valid()) {
		switch_sim->request_local_player_weapon_cycle(-1);
	}
	cycle_prev_was_down_ = prev_down;
	const bool next_down = p_captured && pressed("cycleweaponN");
	if (next_down && !cycle_next_was_down_ && switch_sim.is_valid()) {
		switch_sim->request_local_player_weapon_cycle(1);
	}
	cycle_next_was_down_ = next_down;
}

// The retail radar-zoom bindings are ordinary configurable key rows applying
// one multiplicative step on the down edge: radarout GROWS the world-extent
// value (x1.15 toward 0x100000) and radarin shrinks it (x0.85 toward 4096).
// huddetail (dispatch code 19) is a live arm of the IN-GAME dispatcher, the
// declutter cycle, and GameHudPresenter samples it beside the other HUD rows
// (hud-re.md D-CTRL-4). [orig: Input_HandleActionBinding @0x49AD40 -- radarout
//  row 48 = case 361 @0x49beaf, radarin row 49 = case 360 @0x49bcb0; code 19 ->
//  Input_HandleActionBinding_0 @0x4e060b..0x4e0624 -> CRenderState_SetLayerVisibility
//  @0x59B0F0]
void PlayerInputRouter::sample_hud_input(bool p_active) {
	// The down-edge latches ride the RAW key state (the engine's
	// latched_key_edge, world/player_present.h, carries the retail scan's
	// witness): a key held across an armory/F3 window must NOT re-fire when
	// the gate reopens.
	const Ref<Simulation> hud_sim = sim();
	if (opennova::world::latched_key_edge(pressed("radarout"), p_active, radar_out_was_down_) &&
			hud_sim.is_valid()) {
		hud_sim->request_hud_radar_zoom(1);
	}
	if (opennova::world::latched_key_edge(pressed("radarin"), p_active, radar_in_was_down_) &&
			hud_sim.is_valid()) {
		hud_sim->request_hud_radar_zoom(-1);
	}
	// map_toggle (row 98, default M) cycles the big-map mode: off -> the
	// north-up window -> fullscreen -> off. The retail arm lives in the
	// IN-GAME dispatcher, not the menu-context one.
	// [orig: row 98 code 28 -> the @0x4e0662 arm -> HUD_CycleMapMode
	//  @0x520bc0 (0->2->3->0)]
	if (opennova::world::latched_key_edge(pressed("map_toggle"), p_active, map_toggle_was_down_) &&
			hud_sim.is_valid()) {
		hud_sim->request_hud_map_cycle();
	}
}

// Edge-triggered gameplay keys. No key here moves the camera: the view rows
// (view1st F2 / viewwithgun F3 / viewchase F4) only write the chase
// PREFERENCE and the FP-gun bit, and the sim's arbiter resolves the mode from
// the preference and the seat -- GameHudPresenter polls those rows beside
// its other HUD rows [orig: Input_HandleActionBinding cases 400/401/402
// @0x49c073..0x49c107; the arbiter Render_ProcessMainSceneFrame @0x5ca1d2;
// full 3P camera + torso-bend witness: docs/world/world-wac-ai-re.md §14
// (D-INF-11), net-re §5.39]. Stance is the witnessed 3-key SELECT -- Z prone,
// X crouch, C stand (catalog ids 9/10/11, defaults Z/X/C) -- each key
// REQUESTS its stance from the sim, which applies the mutual exclusion and
// the ForceCrouch refusal (the C2S 0x1D semantics). [orig: input cases
// 170/169/172 @0x4e0df3/@0x4e0d77/@0x4e0e3e ->
// NapiNPServerMsg_HandleStanceChange @0x501c60]
// The keys below read RAW keycodes rather than the binding table's rows
// (binocular action 26 / NVG action 41 / gain actions 56/57 / the stance rows
// 9/10/11): ported verbatim from the GDScript router, a tracked divergence
// follow-up.
bool PlayerInputRouter::handle_key_input(const Ref<InputEvent> &p_event, bool p_active) {
	LocalPlayerPresenter *owner = presenter();
	if (!p_active || owner == nullptr || !owner->has_player()) {
		return false;
	}
	InputEventKey *key = Object::cast_to<InputEventKey>(p_event.ptr());
	if (key == nullptr) {
		return false;
	}
	if (!key->is_pressed() || key->is_echo()) {
		return false;
	}
	const Key physical = key->get_physical_keycode() != KEY_NONE ? key->get_physical_keycode()
																  : key->get_keycode();
	const Ref<Simulation> key_sim = sim();
	if (physical == KEY_B) {
		if (key_sim.is_valid()) {
			key_sim->request_local_player_binoculars_toggle();
		}
		return true;
	}
	if (physical == KEY_N) {
		if (key_sim.is_valid()) {
			key_sim->request_local_player_nvg_toggle();
		}
		return true;
	}
	if (physical == KEY_EQUAL || physical == KEY_PLUS || physical == KEY_KP_ADD) {
		if (key_sim.is_valid()) {
			key_sim->request_local_player_nvg_gain(1);
		}
		return true;
	}
	if (physical == KEY_MINUS || physical == KEY_KP_SUBTRACT) {
		if (key_sim.is_valid()) {
			key_sim->request_local_player_nvg_gain(-1);
		}
		return true;
	}
	// The stance ids' witness lives at the engine home, engine/runtime/world
	// infantry.h InfantryState::Stance (bound as Simulation.STANCE_*).
	if (key->get_keycode() == KEY_Z) {
		request_stance(Simulation::STANCE_PRONE); // [orig: case 170 sends 0xAA]
		return true;
	}
	if (key->get_keycode() == KEY_X) {
		request_stance(Simulation::STANCE_CROUCH); // [orig: case 169 sends 0xA9]
		return true;
	}
	if (key->get_keycode() == KEY_C) {
		request_stance(Simulation::STANCE_STAND); // [orig: case 172 sends 0xAC]
		return true;
	}
	return false;
}

void PlayerInputRouter::request_stance(int p_stance) {
	const Ref<Simulation> stance_sim = sim();
	if (stance_sim.is_valid()) {
		stance_sim->request_local_player_stance(static_cast<Simulation::Stance>(p_stance));
	}
}

// Mouse-look: raw pixel deltas into the SIM's witnessed integer pipeline
// (the sim owns sensitivity, the scoped zoom reduction, Y-invert, and the
// pitch clamps). [orig: Input_ProcessMouseAxisBindings @0x499680 -> the axis
// cases 166/164]
bool PlayerInputRouter::handle_input(const Ref<InputEvent> &p_event, bool p_active) {
	LocalPlayerPresenter *owner = presenter();
	if (!p_active || owner == nullptr || !owner->has_player()) {
		return false;
	}
	InputEventMouseMotion *motion = Object::cast_to<InputEventMouseMotion>(p_event.ptr());
	if (motion == nullptr) {
		return false;
	}
	look_delta_ += motion->get_relative();
	return true;
}

void PlayerInputRouter::reset() {
	fire_was_held_ = false;
	reload_was_down_ = false;
	scope_was_down_ = false;
	look_delta_ = Vector2();
}

void PlayerInputRouter::release_mouse_capture() {
	Input *input = Input::get_singleton();
	if (input->get_mouse_mode() == Input::MOUSE_MODE_CAPTURED) {
		input->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
	}
}

// The live binding table drives every key below (defaults: WASD move, Q/E
// lean, Space jump -- see engine/runtime/controls k_catalog); jump is
// momentary (the motor jumps once when grounded). There is no run key:
// running is the automatic forward-walk promotion in the sim's body
// selection, suppressed while scoped.
// [orig: Player_PackInputStateToEntity @0x4df450; promotion @0x4b729d]
void PlayerInputRouter::read_input_state(bool &r_forward, bool &r_back, bool &r_left, bool &r_right,
		bool &r_lean_left, bool &r_lean_right, bool &r_jump) const {
	if (input_override_.is_valid()) {
		r_forward = input_override_->get_forward();
		r_back = input_override_->get_back();
		r_left = input_override_->get_left();
		r_right = input_override_->get_right();
		r_lean_left = input_override_->get_lean_left();
		r_lean_right = input_override_->get_lean_right();
		r_jump = input_override_->get_jump();
		return;
	}
	r_forward = pressed("move_forward");
	r_back = pressed("move_back");
	r_left = pressed("strafe_left");
	r_right = pressed("strafe_right");
	r_lean_left = pressed("LeanRoll_left");
	r_lean_right = pressed("LeanRoll_right");
	r_jump = pressed("move_jump");
}
