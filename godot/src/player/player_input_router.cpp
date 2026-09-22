#include "player/player_input_router.h"

#include "player/local_player_presenter.h"
#include "simulation/simulation.h"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/global_constants.hpp>

#include <cmath>

using namespace godot;

namespace {

class GodotActionSource final : public opennova::controls::PlayerActionSource {
public:
	explicit GodotActionSource(const Ref<ControlsModel> &p_controls) : controls_(p_controls) {}
	bool pressed(const char *p_token) const override {
		return controls_.is_valid() && controls_->is_token_pressed(p_token);
	}
	int pressed_key(const char *p_token) const override {
		return controls_.is_valid() ? controls_->pressed_key_for_token(p_token) : 0;
	}
	bool digit_down(int p_digit) const override {
		Input *input = Input::get_singleton();
		return input != nullptr &&
				input->is_physical_key_pressed(static_cast<Key>(KEY_0 + p_digit));
	}

private:
	const Ref<ControlsModel> &controls_;
};

void apply_player_action(Simulation &p_sim, const opennova::controls::PlayerActionRequest &p_request) {
	using Action = opennova::controls::PlayerAction;
	switch (p_request.action) {
		case Action::ToggleMount: p_sim.local_player_toggle_mount(); break;
		case Action::SelectSeat: p_sim.local_player_select_seat(p_request.value); break;
		case Action::ToggleScope: p_sim.request_local_player_scope_toggle(); break;
		case Action::WeaponCategory:
			p_sim.request_local_player_weapon_category(
					static_cast<Simulation::WeaponCategory>(p_request.value));
			break;
		case Action::WeaponCycle: p_sim.request_local_player_weapon_cycle(p_request.value); break;
		case Action::Stance:
			p_sim.request_local_player_stance(static_cast<Simulation::Stance>(p_request.value));
			break;
		case Action::ScopeZero: p_sim.request_local_player_scope_zero(p_request.value); break;
		case Action::RadarZoom: p_sim.request_hud_radar_zoom(p_request.value); break;
		case Action::MapCycle: p_sim.request_hud_map_cycle(); break;
	}
}

} // namespace

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
    frame_input->set_view_keys(p_gameplay_input_active && pressed("FreeLook"),
        p_gameplay_input_active && pressed("look_up"), p_gameplay_input_active && pressed("look_down"),
        p_gameplay_input_active && pressed("turn_left"), p_gameplay_input_active && pressed("turn_right"));
	frame_input->set_look_delta(p_gameplay_input_active ? look_delta_ : Vector2());
	look_delta_ = Vector2();
	const Ref<Simulation> action_sim = sim();
	const auto actions = actions_.poll(GodotActionSource(controls_),
			{p_gameplay_input_active, input->get_mouse_mode() == Input::MOUSE_MODE_CAPTURED,
					action_sim.is_valid()});
	frame_input->set_weapon_input(actions.fire_held, actions.fire_edge,
			actions.reload_edge, actions.medic_edge);
	for (const auto &request : actions.requests) apply_player_action(*action_sim.ptr(), request);
	return frame_input;
}

void PlayerInputRouter::consume_use_hold() {
	actions_.consume_use_hold();
}

// Edge-triggered gameplay keys. No key here moves the camera: the view rows
// (view1st F2 / viewwithgun F3 / viewchase F4) only write the chase
// PREFERENCE and the FP-gun bit, and the sim's arbiter resolves the mode from
// the preference and the seat -- GameHudPresenter polls those rows beside
// its other HUD rows [orig: Input_HandleActionBinding cases 400/401/402
// @0x49c073..0x49c107; the arbiter Render_ProcessMainSceneFrame @0x5ca1d2;
// full 3P camera + torso-bend witness: docs/world/world-wac-ai-re.md §14
// (D-INF-11), net-re §5.39]. Stance rows are polled by the native action table.
// The keys below still read RAW keycodes rather than the binding table's rows
// (binocular action 26 / NVG action 41 / gain actions 56/57): ported verbatim
// from the GDScript router, a tracked divergence follow-up.
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
	return false;
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
    InputEventMouseButton *button = Object::cast_to<InputEventMouseButton>(p_event.ptr());
    if (button != nullptr && button->is_pressed() && controls_.is_valid() &&
        (button->get_button_index() == MOUSE_BUTTON_WHEEL_UP ||
         button->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN)) {
        const Ref<Simulation> event_sim = sim();
        if (event_sim.is_null()) return false;
        // The event's factor is |delta| / WHEEL_DELTA (0 where the platform
        // reports none: one notch). The native accumulator turns the message
        // stream into whole notches (controls/player_actions.h WheelRemainder).
        const float factor = button->get_factor();
        const int32_t units = factor > 0.0f
                ? static_cast<int32_t>(std::lround(factor * opennova::controls::kWheelDelta))
                : opennova::controls::kWheelDelta;
        int notches = wheel_.feed(
                button->get_button_index() == MOUSE_BUTTON_WHEEL_UP ? units : -units);
        const auto dispatch = [&](MouseButton p_wheel) {
            const String token = controls_->mouse_event_token(p_wheel);
            if (const auto request = opennova::controls::player_wheel_action(token.utf8().get_data()))
                apply_player_action(*event_sim.ptr(), *request);
        };
        for (; notches > 0; --notches) dispatch(MOUSE_BUTTON_WHEEL_UP);
        for (; notches < 0; ++notches) dispatch(MOUSE_BUTTON_WHEEL_DOWN);
        return true;
    }
	InputEventMouseMotion *motion = Object::cast_to<InputEventMouseMotion>(p_event.ptr());
	if (motion == nullptr) {
		return false;
	}
	look_delta_ += motion->get_relative();
	return true;
}

void PlayerInputRouter::reset() {
	actions_.reset();
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
