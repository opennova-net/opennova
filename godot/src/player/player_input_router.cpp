#include "player/player_input_router.h"

#include "player/key_lock_device.h"
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
#include <runtime/world/player_look.h> // pass_through_close_axis, kCursorCentreX / Y

#include <cmath>
#include <string_view>

using namespace godot;

namespace {

// Retail's auto-run word and its turn-off over the keyboard-lock device (D-NET-563; engine
// controls::PlayerActionPoll::scroll_lock_on and PlayerActionFrame::scroll_lock_off carry
// the witness).
bool scroll_lock_device_on() {
	return key_lock_device::on(opennova::controls::kVkScroll);
}

bool scroll_lock_device_turn_off() {
	return key_lock_device::set(opennova::controls::kVkScroll, false);
}

// The held USE row's catalog row (useitem; its dispatch code is the action head's key).
constexpr int kUseItemRow = 44;

// The Num Lock state the process start found, which its exit puts back; the exit runs only
// once the start leg ran.
bool s_key_locks_started = false;
bool s_saved_num_lock = false;

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
	// A keyboard key fires its row through the key queue's events (the shell's
	// KeyboardPass); the mouse, joystick and scripted arms here.
	bool held_off_keyboard(const char *p_token) const override {
		return controls_.is_valid() && controls_->is_token_held_off_keyboard(p_token);
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
		// The role's arms (engine inmatch::Role::dispatch_record): never handed here.
		case Action::Respawn: break;
		case Action::MedicRequest: break;
	}
}

} // namespace

void PlayerInputRouter::setup(Node *p_world, LocalPlayerPresenter *p_presenter,
		const Ref<ControlsModel> &p_controls) {
	world_id_ = p_world != nullptr ? ObjectID(p_world->get_instance_id()) : ObjectID();
	presenter_id_ = p_presenter != nullptr ? ObjectID(p_presenter->get_instance_id()) : ObjectID();
	controls_ = p_controls;
}

// The process's keyboard-lock legs (D-NET-563; engine controls::kVkNumLock carries the
// witness).
void PlayerInputRouter::start_process_num_lock() {
	// The command line's init: Num Lock's state saved and the lock forced on.
	s_key_locks_started = true;
	s_saved_num_lock = key_lock_device::on(opennova::controls::kVkNumLock);
	key_lock_device::set(opennova::controls::kVkNumLock, true);
}

void PlayerInputRouter::start_process_scroll_lock() {
	// The subsystems' init: Scroll Lock off, ahead of any input frame.
	key_lock_device::set(opennova::controls::kVkScroll, false);
}

void PlayerInputRouter::exit_process_key_locks() {
	// The subsystems' shutdown: Scroll Lock off, Num Lock as the start found it, Caps Lock off.
	if (!s_key_locks_started) return;
	s_key_locks_started = false;
	key_lock_device::set(opennova::controls::kVkScroll, false);
	key_lock_device::set(opennova::controls::kVkNumLock, s_saved_num_lock);
	key_lock_device::set(opennova::controls::kVkCapital, false);
}

void PlayerInputRouter::owe_scroll_lock_turn_off() {
	scroll_lock_turn_off_owed_ = true;
}

void PlayerInputRouter::teardown() {
	world_id_ = ObjectID();
	presenter_id_ = ObjectID();
	if (controls_.is_valid()) controls_->set_scan_gates(false, false);
	controls_.unref();
	input_override_.unref();
	row_samples_.clear();
	row_events_.clear();
	action_events_.clear();
	sample_records_.clear();
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
	// The window's messages were read since the last frame: the keyboard state holds any
	// turn-off sent then (retail pumps them at once after its send). A turn-off the Escape
	// key's reset owes goes out here, ahead of the frame's sample, which then reads the word
	// off (owe_scroll_lock_turn_off).
	scroll_lock_turned_off_ = false;
	if (scroll_lock_turn_off_owed_) {
		scroll_lock_turn_off_owed_ = false;
		scroll_lock_turned_off_ = scroll_lock_device_turn_off();
	}
	++frame_sequence_;
	frame_input->set_sequence(frame_sequence_);
	frame_player_ = false;
	frame_gameplay_input_active_ = p_gameplay_input_active;
	frame_menu_open_ = p_menu_open;
	LocalPlayerPresenter *owner = presenter();
	if (owner == nullptr) {
		return frame_input;
	}
	// The frame's head steps the devices to the logic tick its first update runs as;
	// the session runs every update's input frame through this router: its keyboard
	// pass and its talk and HUD rows (run_update_rows), then its movement and action
	// rows (sample_tick).
	frame_input->set_tick_source(this, ObjectID(owner->get_instance_id()));
	const Ref<Simulation> tick_sim = sim();
	Vector2 scripted_look;
	tick_spectating_ = begin_input_frame(
			tick_sim, tick_sim.is_valid() ? tick_sim->get_logic_tick() : -1, scripted_look);
	// The look waits for the next update's sample, a frame that drains none
	// included, as the session kept it for the next tick.
	tick_scripted_look_ += scripted_look;
	// The rows' state this frame sees waits for the next update's poll: a frame
	// whose bank drains no update runs no input frame, and its key events stay
	// queued, a release and a new press included (engine inmatch::TickInputSource
	// carries the witness).
	queue_row_sample();
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
	if (!tick_spectating_) owner->ensure_models();
	frame_player_ = true;
	// The frame's head reads: the mouse motion as the cursor offset in whole pixels,
	// its sub-pixel rest carried to the next frame, which the session's mouse pump
	// reads in the frame's first update and spreads over the frame's updates
	// (engine world::MouseAxisPump), and the menu-screen bit the paused updates
	// read.
	if (p_gameplay_input_active) {
		const Vector2i whole(
				static_cast<int32_t>(look_delta_.x), static_cast<int32_t>(look_delta_.y));
		look_delta_ -= Vector2(whole);
		frame_input->set_cursor_delta(whole);
	} else {
		look_delta_ = Vector2();
	}
	frame_input->set_menu_open(frame_menu_open_);
	return frame_input;
}

opennova::inmatch::InputPacket PlayerInputRouter::sample_tick(uint32_t p_logic_tick) {
	// The device already stands at p_logic_tick: the frame's head or this update's
	// run_update_rows stepped it, and the update's keyboard pass and rows ran ahead of
	// the action rows.
	(void)p_logic_tick;
	Ref<MissionFrameInput> tick_input;
	tick_input.instantiate();
	tick_input->set_sequence(frame_sequence_);
	if (frame_player_) {
		sample_input_frame(tick_input, tick_scripted_look_, tick_spectating_);
	} else {
		// No sample takes the action rows' events: they are dropped.
		action_events_.clear();
	}
	tick_scripted_look_ = Vector2();
	return tick_input->native_value().player;
}

void PlayerInputRouter::run_update_rows(uint32_t p_logic_tick, bool p_new_sample,
		opennova::inmatch::PauseRowWord &r_pause) {
	// An update's input frame polls its talk and HUD rows, a paused update's too:
	// the pause row's case dispatches from the drain of the update that reads it
	// (engine inmatch::TickInputSource carries the witness). A later update of a
	// running frame steps the device to the logic tick it runs as first.
	const Ref<Simulation> rows_sim = sim();
	if (p_new_sample) {
		tick_spectating_ = begin_input_frame(rows_sim, p_logic_tick, tick_scripted_look_);
		queue_row_sample();
	}
	// Under a menu screen no input frame runs, so nothing drains the key events:
	// they wait, in order, for the close's reset (reset_input_state) or, under
	// DEATH's exit confirmation, for the first input frame after its NO raises
	// the word (the shell's KeyboardPass carries the witness).
	if (frame_menu_open_) return;
	// The input frame's head: the shell's keyboard pass drains the key queue, its
	// special-key legs land in this update, and the row events it queues dispatch
	// below and in this update's sample; a frame that runs no update keeps its keys
	// (engine inmatch::TickInputSource carries the witness). Its legs may run the
	// input reset, which drops the rows queued so far.
	if (keyboard_pass_.is_valid()) keyboard_pass_.call();
	// A later paused update of the same frame reads the state the frame left.
	if (row_samples_.empty()) queue_row_sample();
	// The keyboard pass's events since the last update's input frame, one dispatch
	// each in their queue order: a talk or HUD row's key-down (an OS repeat
	// included) fires its row once, with the sign its fire carried (D-CTRL-6;
	// engine hud_toggles_dispatch and ChatEntry::dispatch_rows carry the witness).
	// The keyboard pass runs ahead of the mouse
	// and joystick passes, so its events dispatch first (engine
	// controls::PlayerActionPoll::key_events carries the witness: one record per
	// queued event, no merge, each dispatched by the drain).
	const auto pause_row_ran = [&]() {
		r_pause.flip();
		// The pause row's case resets the key queue behind its flip (engine
		// controls::KeyEventQueue::reset carries the witness).
		key_events()->reset();
	};
	for (const RowEvent &event : row_events_) {
		const opennova::controls::ActionDef *def = opennova::controls::action_for_code(event.code);
		if (def == nullptr) continue;
		const std::string_view token(def->token);
		for (int row = 0; row < HudChatEntry::ROW_COUNT; ++row)
			if (token == opennova::hud::chat_talk_row_token(row))
				poll_talk_rows(rows_sim, 1 << row);
		for (int row = 0; row < HudToggles::ROW_COUNT; ++row)
			if (token == opennova::hud::hud_toggle_row_token(row) &&
					poll_hud_toggles(rows_sim, 1 << row, event.sign))
				pause_row_ran();
	}
	row_events_.clear();
	// The off-keyboard arms' states in their order, so a press, its release and a
	// new press of a mouse or joystick arm seen by frames that ran no update each
	// reach the rows; a pause row flips the word at once, and the states behind it
	// meet the head with it (engine inmatch::TickInputSource carries the witness).
	for (const RowSample &sample : row_samples_) {
		const int talk_fired = sample.talk & ~talk_held_;
		const int hud_fired = sample.hud & ~hud_held_;
		// The mousescale fire's sign: the mouse arm's (Shift held; engine
		// BindingSet::fire_signed).
		const bool mouse_scale_negative = controls_.is_valid() &&
				controls_->is_token_fire_signed(String(opennova::hud::hud_toggle_row_token(
						HudToggles::ROW_MOUSE_SCALE)));
		talk_held_ = sample.talk;
		hud_held_ = sample.hud;
		poll_talk_rows(rows_sim, talk_fired);
		if (poll_hud_toggles(rows_sim, hud_fired, mouse_scale_negative)) pause_row_ran();
	}
	row_samples_.clear();
}

void PlayerInputRouter::queue_records(opennova::controls::InputEventQueue &r_records) {
	// Behind the records already waiting (the console's); the queue's cap drops the rest.
	for (const opennova::controls::InputEventRecord &record : sample_records_)
		(void)r_records.push(record);
	sample_records_.clear();
}

void PlayerInputRouter::dispatch_record(const opennova::controls::InputEventRecord &p_record) {
	const Ref<Simulation> record_sim = sim();
	if (record_sim.is_null()) return;
	if (const auto request = opennova::controls::player_action_request(p_record))
		apply_player_action(*record_sim.ptr(), *request);
}

void PlayerInputRouter::queue_row_event(int p_code, bool p_signed, int p_key) {
	queue_key_row_event(opennova::controls::KeyRowEvent{p_code, p_signed, p_key, false});
}

void PlayerInputRouter::queue_key_row_event(const opennova::controls::KeyRowEvent &p_event) {
	// A talk or HUD row's event dispatches in the next update's drain
	// (run_update_rows); any other row's in the next device sample's action poll.
	const opennova::controls::ActionDef *def = opennova::controls::action_for_code(p_event.code);
	if (def != nullptr) {
		const std::string_view token(def->token);
		bool update_row = false;
		for (int row = 0; row < HudChatEntry::ROW_COUNT; ++row)
			update_row = update_row || token == opennova::hud::chat_talk_row_token(row);
		for (int row = 0; row < HudToggles::ROW_COUNT; ++row)
			update_row = update_row || token == opennova::hud::hud_toggle_row_token(row);
		if (update_row) {
			row_events_.push_back(RowEvent{p_event.code, p_event.sign});
			return;
		}
	}
	action_events_.push_back(p_event);
}

void PlayerInputRouter::run_key_release_pass() {
	if (controls_.is_null()) return;
	const Ref<ControlsModel> &controls = controls_;
	const auto key_state = [&controls](int p_vk) { return controls->is_key_state_down(p_vk); };
	for (const opennova::controls::KeyRowEvent &event :
			controls_->native_bindings().release_pass(key_events()->press_latch(), key_state))
		queue_key_row_event(event);
}

void PlayerInputRouter::queue_row_sample() {
	const RowSample sample{talk_rows_down(), hud_rows_down()};
	if (row_samples_.empty() || row_samples_.back().talk != sample.talk ||
			row_samples_.back().hud != sample.hud)
		row_samples_.push_back(sample);
}

int PlayerInputRouter::talk_rows_down() const {
	int rows_down = 0;
	if (controls_.is_null()) return rows_down;
	for (int row = 0; row < HudChatEntry::ROW_COUNT; ++row)
		if (controls_->is_token_held_off_keyboard(opennova::hud::chat_talk_row_token(row)))
			rows_down |= 1 << row;
	return rows_down;
}

int PlayerInputRouter::hud_rows_down() const {
	int rows_down = 0;
	if (controls_.is_null()) return rows_down;
	for (int row = 0; row < HudToggles::ROW_COUNT; ++row)
		if (controls_->is_token_held_off_keyboard(opennova::hud::hud_toggle_row_token(row)))
			rows_down |= 1 << row;
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

void PlayerInputRouter::poll_talk_rows(const Ref<Simulation> &p_sim, int p_rows_fired) {
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
	hud_chat_->dispatch_rows(p_rows_fired, frame_gameplay_input_active_, p_sim, chat_gametext_,
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
    p_input->set_view_keys(active && pressed("FreeLook"),
        active && pressed("look_up"), active && pressed("look_down"),
        active && pressed("turn_left"), active && pressed("turn_right"));
	// The mouse motion rides the frame's head (before_world_tick). The scripted
	// device's look pixels are axis input at their own tick, as onHook's driver
	// adds them to the axis word in that input frame, under the gate real motion
	// meets on its way in (the shell hands handle_input only live gameplay with the
	// mouse captured).
	const bool captured = input->get_mouse_mode() == Input::MOUSE_MODE_CAPTURED;
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
	// The keyboard pass's action row events since the last sample.
	gate.key_events.swap(action_events_);
	action_events_.clear();
	// The Scroll Lock word is a device the scripted input replaces like the keys.
	gate.scroll_lock_on =
			input_override_.is_null() && !scroll_lock_turned_off_ && scroll_lock_device_on();
	// The held USE row's arm through the sim's press latch (engine
	// LocalPlayer::take_use_item_press), once its event passed the dispatcher's
	// head by the row's flags: the deploy overlay, the round-over gate and the
	// pause word drop it as the dead bit does, under a pass-through screen's
	// input frame too (engine inmatch::Role::action_head_admits carries the
	// witness). A dropped press arms nothing.
	gate.take_use_item_arm = [&action_sim]() {
		using Arm = opennova::controls::UseItemPressArm;
		if (action_sim.is_null()) return Arm::kMountChain;
		if (!action_sim->action_head_admits(opennova::controls::action_code(kUseItemRow)))
			return Arm::kTaken;
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
	// Retail's auto-run rides the forward key (engine controls::PlayerActionFrame::
	// auto_run_forward). A case that turns its toggle off passes the dispatcher's head by its
	// code; one turn-off per frame, as the keyboard state reads it only once the window's
	// messages are read.
	p_input->set_movement(forward || actions.auto_run_forward, back, left, right, lean_left,
			lean_right, jump);
	for (const int code : actions.scroll_lock_off) {
		if (scroll_lock_turned_off_) break;
		if (code < 0 || (action_sim.is_valid() && action_sim->action_head_admits(code)))
			scroll_lock_turned_off_ = scroll_lock_device_turn_off();
	}
	p_input->set_weapon_input(actions.fire_held, actions.fire_edge, actions.reload_edge);
	p_input->set_to_special_input(actions.to_special_held, actions.to_special_edge);
	p_input->set_menu_open(frame_menu_open_);
	for (const auto &request : actions.requests) {
		// An action row's request is a record the update's drain dispatches in queue order
		// (queue_records, dispatch_record); the special-key and USE arms run here.
		if (request.code >= 0) {
			sample_records_.push_back({request.code, 0, 0, 0, request.sign,
					opennova::controls::InputEventOrigin::Device});
			continue;
		}
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
					 active, gate.key_events))
			apply_player_action(*action_sim.ptr(), request);
	}
}

bool PlayerInputRouter::poll_hud_toggles(const Ref<Simulation> &p_sim, int p_rows_fired,
		bool p_mouse_scale_negative) {
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
	// The row events are the keyboard pass's key-downs (an OS repeat fires its
	// row again) and this router's press edges of the mouse, joystick and scripted
	// arms (run_update_rows); the cycles and the window toggles are the engine's
	// dispatch (engine hud/hud_toggles.h hud_toggles_dispatch); the HUD presenter
	// applies the event words at its frame.
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
	const int64_t events = hud_toggles_->dispatch(p_rows_fired, frame_gameplay_input_active_,
			p_sim->is_mp_session(),
			opennova::game_type::is_objective(static_cast<uint32_t>(p_sim->get_session_game_type())),
			!p_sim->local_player_dead(), role != Simulation::ROLE_JOINER,
			role != Simulation::ROLE_DEDICATED_HOST, p_mouse_scale_negative, head_drops);
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
	// The reset's head: the key queue, then the key state (engine
	// controls::KeyEventQueue::reset carries the witness).
	key_events()->reset();
	if (controls_.is_valid()) controls_->reset_key_state();
	key_events()->clear_press_latch();
	// The reset's event queue memset drops the rows a no-update frame left waiting.
	row_samples_.clear();
	row_events_.clear();
	action_events_.clear();
	sample_records_.clear();
	const Ref<Simulation> reset_sim = sim();
	if (reset_sim.is_valid()) reset_sim->clear_deferred_input();
}

Ref<KeyEventQueue> PlayerInputRouter::key_events() {
	if (key_events_.is_null()) key_events_.instantiate();
	return key_events_;
}

void PlayerInputRouter::push_key_event(const Ref<InputEventKey> &p_event) {
	if (p_event.is_null()) return;
	// The key-down handler sets the key's state (the shell's key_down_event), then
	// queues the message, whose Ctrl and Shift bits read the key state (with no binding
	// model, the event's own; engine controls::KeyEventQueue and key_event_flag carry
	// the witness).
	const bool ctrl =
			controls_.is_valid() ? controls_->is_vk_down(0x11) : p_event->is_ctrl_pressed();
	const bool shift =
			controls_.is_valid() ? controls_->is_vk_down(0x10) : p_event->is_shift_pressed();
	key_events()->push_key(p_event, ctrl, shift);
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
	row_samples_.clear();
	row_events_.clear();
	action_events_.clear();
	sample_records_.clear();
	tick_scripted_look_ = Vector2();
}

void PlayerInputRouter::release_mouse_capture() {
	Input *input = Input::get_singleton();
	if (input->get_mouse_mode() == Input::MOUSE_MODE_CAPTURED) {
		input->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
	}
}

void PlayerInputRouter::release_capture_for_screen() {
	Input *input = Input::get_singleton();
	if (input->get_mouse_mode() != Input::MOUSE_MODE_CAPTURED) return;
	input->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
	DisplayServer::get_singleton()->warp_mouse(
			Vector2i(opennova::world::kCursorCentreX, opennova::world::kCursorCentreY));
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
