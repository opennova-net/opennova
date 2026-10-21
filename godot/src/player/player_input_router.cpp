#include "player/player_input_router.h"

#include "player/local_player_presenter.h"
#include "simulation/simulation.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/global_constants.hpp>

#include <base/gameprofile/game_type.h>
#include <runtime/controls/controls.h> // action_code (the HUD rows' dispatch codes)
#include <runtime/world/player_look.h> // pass_through_close_axis

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
	// A digit the special-key chain already took (the quit dialog's swallow)
	// reads up here too, so it reaches neither the seat nor the menu picks.
	bool digit_down(int p_digit) const override {
		if (controls_.is_valid()) return controls_->is_vk_down(0x30 + p_digit);
		Input *input = Input::get_singleton();
		return input != nullptr &&
				input->is_physical_key_pressed(static_cast<Key>(KEY_0 + p_digit));
	}
	bool keys_down(const char *p_token) const override {
		return pressed(p_token) || (controls_.is_valid() && controls_->is_token_key_down(p_token));
	}
	// Shift's sign bit reads the key-state array, which a menu close's reset
	// clears (engine controls::KeyStateReset carries the witness).
	bool fire_signed(const char *p_token) const override {
		return controls_.is_valid() && controls_->is_token_fire_signed(String(p_token));
	}

private:
	const Ref<ControlsModel> &controls_;
};

void apply_player_action(Simulation &p_sim, const opennova::controls::PlayerActionRequest &p_request) {
	using Action = opennova::controls::PlayerAction;
	// A binding row's event passes the dispatcher's head by its row's flags
	// (engine inmatch::Role::action_head_admits carries the witness).
	if (p_request.code >= 0 && !p_sim.action_head_admits(p_request.code)) return;
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
		case Action::Binoculars: p_sim.request_local_player_binoculars_toggle(); break;
		case Action::NightVision: p_sim.request_local_player_nvg_toggle(); break;
		case Action::NvgGain: p_sim.request_local_player_nvg_gain(p_request.value); break;
		case Action::WaypointCycle: p_sim.request_waypoint_cycle(p_request.value); break;
		case Action::Spectate: p_sim.request_spectate_action(p_request.value); break;
		// Sent over the session; the router closes the menu beside it.
		case Action::EmotePick: p_sim.send_voice_menu_pick(false, p_request.value); break;
		case Action::RadioPick: p_sim.send_voice_menu_pick(true, p_request.value); break;
		// The shell opens the screen (PlayerInputRouter::take_armory_request /
		// take_vehicle_menu_request).
		case Action::OpenArmory: break;
		case Action::OpenVehicleMenu: break;
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
	if (controls_.is_valid()) controls_->set_scan_gates(false, false);
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
		bool p_gameplay_input_active, bool p_menu_open) {
	Ref<MissionFrameInput> frame_input;
	frame_input.instantiate();
	frame_input->set_delta_seconds(p_delta);
	++frame_sequence_;
	frame_input->set_sequence(frame_sequence_);
	frame_player_ = false;
	frame_gameplay_input_active_ = p_gameplay_input_active;
	frame_menu_open_ = p_menu_open;
	LocalPlayerPresenter *owner = presenter();
	if (owner == nullptr) {
		return frame_input;
	}
	// The frame's sample is its first update's movement and action rows, at the
	// logic tick that update runs as; the session asks for each later update's
	// (sample_tick) and for every update's talk and HUD rows (run_update_rows).
	frame_input->set_tick_source(this, ObjectID(owner->get_instance_id()));
	const Ref<Simulation> tick_sim = sim();
	Vector2 scripted_look;
	const bool spectating = begin_input_frame(
			tick_sim, tick_sim.is_valid() ? tick_sim->get_logic_tick() : -1, scripted_look);
	// The rows this sample sees down wait for the next update's poll: a frame whose
	// bank drains no update runs no input frame, and its key events stay queued
	// (engine inmatch::TickInputSource carries the witness).
	latched_talk_rows_ |= talk_rows_down();
	latched_hud_rows_ |= hud_rows_down();
	if (!owner->has_player()) {
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
	if (!spectating) owner->ensure_models();
	frame_player_ = true;
	sample_input_frame(frame_input, scripted_look, spectating);
	return frame_input;
}

opennova::inmatch::InputPacket PlayerInputRouter::sample_tick(uint32_t p_logic_tick) {
	// The device already stands at p_logic_tick: this update's run_update_rows
	// stepped it and polled the rows ahead of the action rows.
	(void)p_logic_tick;
	Ref<MissionFrameInput> tick_input;
	tick_input.instantiate();
	tick_input->set_sequence(frame_sequence_);
	if (frame_player_) sample_input_frame(tick_input, tick_scripted_look_, tick_spectating_);
	tick_scripted_look_ = Vector2();
	return tick_input->native_value().player;
}

bool PlayerInputRouter::run_update_rows(uint32_t p_logic_tick, bool p_new_sample) {
	// An update's input frame polls its talk and HUD rows, a paused update's too:
	// the pause row's case dispatches from the drain of the update that reads it
	// (engine inmatch::TickInputSource carries the witness). A later update of a
	// running frame steps the device to the logic tick it runs as first.
	const Ref<Simulation> rows_sim = sim();
	if (p_new_sample)
		tick_spectating_ = begin_input_frame(rows_sim, p_logic_tick, tick_scripted_look_);
	const int talk_rows = talk_rows_down() | latched_talk_rows_;
	const int hud_rows = hud_rows_down() | latched_hud_rows_;
	latched_talk_rows_ = 0;
	latched_hud_rows_ = 0;
	poll_talk_rows(rows_sim, talk_rows);
	return poll_hud_toggles(rows_sim, hud_rows);
}

int PlayerInputRouter::talk_rows_down() const {
	int rows_down = 0;
	for (int row = 0; row < HudChatEntry::ROW_COUNT; ++row)
		if (pressed(opennova::hud::chat_talk_row_token(row))) rows_down |= 1 << row;
	return rows_down;
}

int PlayerInputRouter::hud_rows_down() const {
	int rows_down = 0;
	for (int row = 0; row < HudToggles::ROW_COUNT; ++row)
		if (pressed(opennova::hud::hud_toggle_row_token(row))) rows_down |= 1 << row;
	return rows_down;
}

bool PlayerInputRouter::begin_input_frame(const Ref<Simulation> &p_sim, int64_t p_logic_tick,
		Vector2 &r_scripted_look) {
	// The scripted input device (devtools/scripted_input.h) steps on the logic
	// tick this sample is keyed to, ahead of every device read below.
	const Ref<ScriptedInput> scripted =
			controls_.is_valid() ? controls_->get_scripted_input() : Ref<ScriptedInput>();
	if (scripted.is_valid()) {
		if (p_sim.is_valid()) controls_->advance_scripted_input(p_logic_tick);
		r_scripted_look = scripted->take_look();
	}
	// The death screen samples the same device input: the engine's filter
	// hands it to the free-fly motor or the chase orbit, and the binding scan
	// admits only the death-screen rows (PlayerActionPoll::death_screen).
	const bool spectating = p_sim.is_valid() && p_sim->local_death_screen_active();
	// The key scan's gates for every poll of this input frame, the HUD's after
	// the world tick too: the death screen, and the scan word, set on a
	// dedicated host and otherwise while the local body is dead (engine
	// BindingSet::set_scan_gates carries the witness).
	if (controls_.is_valid())
		controls_->set_scan_gates(spectating, p_sim.is_valid() &&
				(p_sim->session_role() == Simulation::ROLE_DEDICATED_HOST ||
						p_sim->local_player_dead()));
	return spectating;
}

void PlayerInputRouter::poll_talk_rows(const Ref<Simulation> &p_sim, int p_rows_down) {
	// The talk rows are action bindings the input frame's drain dispatches
	// every tick, after any typed keys reached an open line, so a one-tick
	// press inside a multi-tick frame opens its line on its tick; the command
	// prompt opens with or without a local player, as a Serve Only host's does
	// (engine hud/hud_chat_entry.h ChatEntry::poll_rows carries the witness and
	// its open point). The key
	// sampling is this router's; the dispatch gates, the capture, the talk
	// debounce and the flood are the engine's chat entry (ChatEntryFacts::frame,
	// inmatch/chat_flood.h); the HUD presenter draws the line at its frame.
	if (hud_chat_.is_null() || p_sim.is_null()) return;
	hud_chat_->poll_rows(p_rows_down, frame_gameplay_input_active_, p_sim, chat_gametext_,
			p_sim->get_main_frame_counter());
}

void PlayerInputRouter::set_hud_chat(const Ref<HudChatEntry> &p_chat,
		const Ref<RtxtStringFile> &p_gametext) {
	hud_chat_ = p_chat;
	chat_gametext_ = p_gametext;
}

void PlayerInputRouter::sample_input_frame(const Ref<MissionFrameInput> &p_input,
		const Vector2 &p_scripted_look, bool p_spectating) {
	const bool active = frame_gameplay_input_active_;
	Input *input = Input::get_singleton();
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
	if (active) {
		read_input_state(forward, back, left, right, lean_left, lean_right, jump);
	}
	p_input->set_movement(forward, back, left, right, lean_left, lean_right, jump);
    p_input->set_view_keys(active && pressed("FreeLook"),
        active && pressed("look_up"), active && pressed("look_down"),
        active && pressed("turn_left"), active && pressed("turn_right"));
	// The mouse motion as the cursor offset in whole pixels, its sub-pixel
	// rest carried to the next sample (the session's mouse pump spreads the
	// offset over the frame's ticks; engine world::MouseAxisPump). The
	// scripted device's look pixels are axis input at their own tick, as
	// onHook's driver adds them to the axis word in that input frame, under
	// the gate real motion meets on its way in (the shell hands handle_input
	// only live gameplay with the mouse captured).
	const bool captured = input->get_mouse_mode() == Input::MOUSE_MODE_CAPTURED;
	if (active) {
		const Vector2i whole(
				static_cast<int32_t>(look_delta_.x), static_cast<int32_t>(look_delta_.y));
		look_delta_ -= Vector2(whole);
		p_input->set_cursor_delta(whole);
	} else {
		look_delta_ = Vector2();
	}
	// A pass-through screen's close owes this sample its cursor offset, axis
	// input of this tick alone (close_pass_through_screen).
	p_input->set_look_delta(active && captured ? p_scripted_look + close_axis_ : Vector2());
	close_axis_ = Vector2();
	const Ref<Simulation> action_sim = sim();
	opennova::controls::PlayerActionPoll gate;
	gate.active = active;
	gate.captured = captured;
	gate.simulation_available = action_sim.is_valid();
	gate.keyboard_captured = controls_.is_valid() && controls_->is_keyboard_captured();
	gate.emotes_menu_open = hud_toggles_.is_valid() && hud_toggles_->is_emotes_menu_open();
	gate.radio_menu_open = hud_toggles_.is_valid() && hud_toggles_->is_radio_menu_open();
	gate.death_screen = p_spectating;
	gate.menu_open = frame_menu_open_;
	// The held USE row's arm through the sim's press latch (engine
	// LocalPlayer::take_use_item_press).
	gate.take_use_item_arm = [&action_sim]() {
		using Arm = opennova::controls::UseItemPressArm;
		if (action_sim.is_null()) return Arm::kMountChain;
		switch (action_sim->take_use_item_press()) {
			case Simulation::USE_ITEM_ARM_MOUNT_CHAIN: return Arm::kMountChain;
			case Simulation::USE_ITEM_ARM_ARMORY: return Arm::kOpenArmory;
			// The vehicle-loadout arm opens vehicle.mnu when the ground's team
			// byte is 0 or the player's, else takes the press (engine
			// local_player_vehicle_zone_team_matches carries the witness).
			case Simulation::USE_ITEM_ARM_VEHICLE_LOADOUT:
				return action_sim->local_player_vehicle_zone_team_matches() ? Arm::kOpenVehicleMenu
				                                                            : Arm::kTaken;
			default: return Arm::kTaken;
		}
	};
	const auto actions = actions_.poll(GodotActionSource(controls_), gate);
	p_input->set_weapon_input(actions.fire_held, actions.fire_edge,
			actions.reload_edge, actions.medic_edge);
	p_input->set_to_special_input(actions.to_special_held, actions.to_special_edge);
	p_input->set_menu_open(frame_menu_open_);
	p_input->set_respawn_input(actions.respawn_edge);
	for (const auto &request : actions.requests) {
		// A menu pick closes its menu whether or not a session carries it
		// (engine hud_toggles_close_voice_menu carries the witness).
		const bool radio = request.action == opennova::controls::PlayerAction::RadioPick;
		if ((request.action == opennova::controls::PlayerAction::EmotePick || radio) &&
				hud_toggles_.is_valid())
			hud_toggles_->close_voice_menu(radio);
		if (request.action == opennova::controls::PlayerAction::OpenArmory) {
			armory_requested_ = true;
			continue;
		}
		if (request.action == opennova::controls::PlayerAction::OpenVehicleMenu) {
			vehicle_menu_requested_ = true;
			continue;
		}
		if (action_sim.is_valid()) apply_player_action(*action_sim.ptr(), request);
	}
	// The death screen's own rows (the spectate sub-mode and target).
	if (p_spectating && action_sim.is_valid()) {
		for (const auto &request : actions_.poll_spectator(GodotActionSource(controls_),
					 active))
			apply_player_action(*action_sim.ptr(), request);
	}
}

bool PlayerInputRouter::poll_hud_toggles(const Ref<Simulation> &p_sim, int p_rows_down) {
	// The HUD's key rows are action bindings like the rest: the input frame's
	// key scan fires them and its drain dispatches them, every tick, so a row
	// pressed for one tick of a multi-tick frame fires on that tick. hudcolor
	// (row 76) defaults to Ctrl+F6 beside huddetail's bare F6 (row 50): the
	// binding sampler's two passes keep them apart (Ctrl+F6 fires only
	// hudcolor, F6 only huddetail), and its first match gives a key both rows
	// carry to hudcolor alone (code 10 before 19; engine
	// BindingSet::pressed_key). showhud (row 27) ships unbound. Ctrl is a
	// binding modifier; Shift or Alt held gates nothing, as the key scan has no
	// such gate (engine hud::HudKeyEdge carries the witness).
	// [orig: Input_ProcessFrame @0x49D520 (the Input_ProcessPlayerFrame call
	//  @0x49D58B, the drain's Input_HandleActionBinding call @0x49D5D8); the key
	//  scan's fire @0x49d42f (the modifier pass) and @0x49d488 (the fallback);
	//  huddetail dispatch @0x4E0601; showhud dispatch @0x4E0561]
	// The key sampling is this router's; the edge latches, the cycles and the
	// window toggles are the engine's poll (engine hud/hud_toggles.h); the HUD
	// presenter applies the event words at its frame.
	if (hud_toggles_.is_null() || p_sim.is_null()) return false;
	// Each row's event passes the dispatcher's head by its catalog row's flags
	// (the pre-round timer, the deploy overlay, the round-over gate, the dead
	// operand): the rows the role's head drops this input frame (engine
	// hud::HudKeyPoll::head_drops carries the witness).
	int head_drops = 0;
	for (int row = 0; row < HudToggles::ROW_COUNT; ++row)
		if (!p_sim->action_head_admits(opennova::controls::action_code(
					opennova::hud::hud_toggle_row_catalog_row(row))))
			head_drops |= 1 << row;
	// The connection mode's two bits: an authority is any role but a joiner, a
	// session peer any role but a dedicated host. The mousescale row steps by
	// its fire's sign bit (Shift on a modifier-less key).
	const int role = p_sim->session_role();
	const int64_t events = hud_toggles_->poll(p_rows_down, frame_gameplay_input_active_,
			p_sim->is_mp_session(),
			opennova::game_type::is_objective(static_cast<uint32_t>(p_sim->get_session_game_type())),
			!p_sim->local_player_dead(), role != Simulation::ROLE_JOINER,
			role != Simulation::ROLE_DEDICATED_HOST,
			controls_.is_valid() &&
					controls_->is_token_fire_signed(String(opennova::hud::hud_toggle_row_token(
							HudToggles::ROW_MOUSE_SCALE))),
			head_drops);
	if (events != 0) hud_toggle_events_.push_back(events);
	// The pause row's flip reaches the session at this update (engine
	// inmatch::Session::run_updates); the HUD presenter's later apply of the event
	// finds the session already there.
	return (events & HudToggles::EVENT_PAUSE_TOGGLED) != 0;
}

PackedInt64Array PlayerInputRouter::take_hud_toggle_events() {
	PackedInt64Array out;
	for (const int64_t events : hud_toggle_events_) out.push_back(events);
	hud_toggle_events_.clear();
	return out;
}

void PlayerInputRouter::consume_use_hold() {
	actions_.consume_use_hold();
}


bool PlayerInputRouter::take_armory_request() {
	const bool requested = armory_requested_;
	armory_requested_ = false;
	return requested;
}

bool PlayerInputRouter::take_vehicle_menu_request() {
	const bool requested = vehicle_menu_requested_;
	vehicle_menu_requested_ = false;
	return requested;
}

void PlayerInputRouter::reset_input_state() {
	if (controls_.is_valid()) controls_->reset_key_state();
	actions_.clear_key_press_latches();
	// The key queue's reset drops the rows a no-update frame left waiting.
	latched_talk_rows_ = 0;
	latched_hud_rows_ = 0;
	const Ref<Simulation> reset_sim = sim();
	if (reset_sim.is_valid()) reset_sim->clear_deferred_input();
}

void PlayerInputRouter::close_pass_through_screen() {
	// The device read: the cursor's position in the window's client pixels, the
	// coordinates the original's mouse-move handler keeps.
	DisplayServer *display = DisplayServer::get_singleton();
	const Vector2i client = display->mouse_get_position() - display->window_get_position();
	int32_t axis[2] = {};
	opennova::world::pass_through_close_axis(client.x, client.y, axis);
	close_axis_ = Vector2(static_cast<real_t>(axis[0]), static_cast<real_t>(axis[1]));
}

// Mouse-look: the motion events add up the cursor offset the input frame
// hands the session in whole pixels; the session's mouse pump spreads it into
// the axis words the SIM's witnessed integer pipeline scales (the sim owns
// sensitivity, the scoped zoom reduction, Y-invert, and the pitch clamps).
// (engine: runtime/world/player_look.h MouseAxisPump, runtime/world/local_player.cpp)
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
	close_axis_ = Vector2();
	latched_talk_rows_ = 0;
	latched_hud_rows_ = 0;
	tick_scripted_look_ = Vector2();
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
// (engine: runtime/world/player_input.h)
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
