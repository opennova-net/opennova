#include "player/player_input_router.h"

#include "player/input_device.h"
#include "player/key_lock_device.h"
#include "player/local_player_presenter.h"
#include "resource_index/resource_root.h"
#include "simulation/simulation.h"
#include "world/game_world.h"
#include "world/game_world_access.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>

#include <runtime/controls/controls.h> // title_catalog
#include <runtime/hud/hud_toggles.h> // hud_key_press_keys
#include <runtime/world/player_look.h> // kCursorCentreX / Y

using namespace godot;

namespace {

// The Num Lock state the process start found, which its exit puts back; the exit runs only
// once the start leg ran.
bool s_key_locks_started = false;
bool s_saved_num_lock = false;

// The binding catalog the controls' live records follow (the launch title's).
const opennova::controls::Catalog &catalog_of(const Ref<ControlsModel> &p_controls) {
	return p_controls.is_valid() ? p_controls->native_bindings().catalog()
	                             : opennova::controls::title_catalog(
	                                       ResourceRoot::session_title(Ref<ResourceRoot>()));
}

} // namespace

PlayerInputRouter::PlayerInputRouter() : device_(*this), session_(*this) {
	frame_.bind(&device_, &session_, &shell_);
}

void PlayerInputRouter::setup(Node *p_world, LocalPlayerPresenter *p_presenter,
		const Ref<ControlsModel> &p_controls) {
	world_id_ = p_world != nullptr ? ObjectID(p_world->get_instance_id()) : ObjectID();
	presenter_id_ = p_presenter != nullptr ? ObjectID(p_presenter->get_instance_id()) : ObjectID();
	controls_ = p_controls;
	// The frame's rows over the controls' catalog, the title's.
	frame_.set_catalog(catalog_of(controls_));
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

void PlayerInputRouter::teardown() {
	frame_.teardown();
	world_id_ = ObjectID();
	presenter_id_ = ObjectID();
	controls_.unref();
	input_override_.unref();
}

// The sim, re-resolved per use: mission reloads free the runtime and its sim, so a cached
// reference would go stale (the presenter follows the same rule).
Ref<Simulation> PlayerInputRouter::sim() const {
	return world_access::sim(ObjectDB::get_instance(world_id_));
}

LocalPlayerPresenter *PlayerInputRouter::presenter() const {
	return Object::cast_to<LocalPlayerPresenter>(ObjectDB::get_instance(presenter_id_));
}

void PlayerInputRouter::set_input_override(const Ref<PlayerMoveIntent> &p_intent) {
	input_override_ = p_intent;
}

Ref<MissionFrameInput> PlayerInputRouter::before_world_tick(double p_delta, bool p_capture_mouse,
		bool p_gameplay_input_active, bool p_menu_open) {
	Ref<MissionFrameInput> frame_input;
	frame_input.instantiate();
	frame_input->set_delta_seconds(p_delta);
	frame_input->set_hud_toggles(hud_toggles_);
	LocalPlayerPresenter *owner = presenter();
	const opennova::inmatch::PlayerInputFrame::FrameHead head = frame_.begin_frame(
			owner != nullptr, owner != nullptr && owner->has_player(), p_gameplay_input_active,
			p_menu_open);
	frame_input->set_sequence(static_cast<int64_t>(head.sequence));
	if (!head.sourced) return frame_input;
	// The engine's input frame is the session's source for every update of this frame.
	frame_input->set_tick_source(&frame_, ObjectID(owner->get_instance_id()));
	if (!head.player) {
		owner->set_fly_camera_locked(false);
		release_mouse_capture();
		owner->clear_models();
		return frame_input;
	}
	owner->set_fly_camera_locked(true);
	Input *input = Input::get_singleton();
	if (p_capture_mouse && input->get_mouse_mode() != Input::MOUSE_MODE_CAPTURED) {
		input->set_mouse_mode(Input::MOUSE_MODE_CAPTURED);
	}
	if (!head.spectating) owner->ensure_models();
	frame_input->set_cursor_delta(Vector2i(head.cursor_dx, head.cursor_dy));
	frame_input->set_menu_open(head.menu_open);
	return frame_input;
}

bool PlayerInputRouter::handle_input(const Ref<InputEvent> &p_event, bool p_active) {
	LocalPlayerPresenter *owner = presenter();
	if (!p_active || owner == nullptr || !owner->has_player()) return false;
	opennova::platform::InputEvent event;
	if (!input_device::to_platform(p_event, event)) return false;
	// With no binding table the device's buttons and wheel reach no row.
	if (controls_.is_null() && event.type != opennova::platform::InputEvent::Type::MouseMotion)
		return false;
	return frame_.device_event(event);
}

Ref<SpecialKeyOutcome> PlayerInputRouter::special_key(const Ref<Simulation> &p_session, int p_vk) {
	const Ref<Simulation> chain_sim = p_session.is_valid() ? p_session : sim();
	if (chain_sim.is_null()) return SpecialKeyOutcome::make({});
	Session session(*this, &chain_sim);
	opennova::inmatch::SpecialKeyInput input;
	input.vk = p_vk;
	const opennova::hud::HudKeyPressKeys keys =
			opennova::hud::hud_key_press_keys(game_text_lookup(gametext_));
	input.yes_vk = keys.yes;
	input.no_vk = keys.no;
	input.restart_vk = keys.restart;
	return SpecialKeyOutcome::make(frame_.special_key(session, input));
}

Ref<KeyEventQueue> PlayerInputRouter::key_events() {
	if (key_events_.is_null()) key_events_.instantiate();
	return key_events_;
}

void PlayerInputRouter::push_key_event(const Ref<InputEventKey> &p_event) {
	if (p_event.is_null()) return;
	// The key-down handler sets the key's state (the shell's key_down_event), then queues the
	// message, whose Ctrl and Shift bits read the key state (with no binding model, the
	// event's own; engine controls::queued_key carries the witness).
	const bool ctrl =
			controls_.is_valid() ? controls_->is_vk_down(0x11) : p_event->is_ctrl_pressed();
	const bool shift =
			controls_.is_valid() ? controls_->is_vk_down(0x10) : p_event->is_shift_pressed();
	key_events()->push_key(p_event, ctrl, shift);
}

void PlayerInputRouter::set_hud_toggles(const Ref<HudToggles> &p_toggles) {
	hud_toggles_ = p_toggles;
	frame_.set_hud_toggles(hud_toggles_.is_valid() ? &hud_toggles_->mutable_state() : nullptr);
}

PackedInt64Array PlayerInputRouter::take_hud_toggle_events() {
	PackedInt64Array out;
	for (const uint64_t events : frame_.take_hud_toggle_events())
		out.push_back(static_cast<int64_t>(events));
	return out;
}

bool PlayerInputRouter::poll_hud_toggles(const Ref<Simulation> &p_sim, int p_rows_fired,
		bool p_mouse_scale_negative, int32_t p_mouse_scale_param) {
	if (p_sim.is_null()) return false;
	Session session(*this, &p_sim);
	return frame_.dispatch_hud_rows(session, static_cast<uint32_t>(p_rows_fired),
			p_mouse_scale_negative, p_mouse_scale_param);
}

void PlayerInputRouter::set_hud_chat(const Ref<HudChatEntry> &p_chat,
		const Ref<RtxtStringFile> &p_gametext) {
	hud_chat_ = p_chat;
	gametext_ = p_gametext;
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

// --- the device ---------------------------------------------------------------------------

bool PlayerInputRouter::Device::pressed(const char *p_token) const {
	const Ref<ControlsModel> &controls = router_.controls_;
	return controls.is_valid() && controls->is_token_pressed(p_token);
}

int PlayerInputRouter::Device::pressed_key(const char *p_token) const {
	const Ref<ControlsModel> &controls = router_.controls_;
	return controls.is_valid() ? controls->pressed_key_for_token(p_token) : 0;
}

bool PlayerInputRouter::Device::keys_down(const char *p_token) const {
	const Ref<ControlsModel> &controls = router_.controls_;
	return pressed(p_token) || (controls.is_valid() && controls->is_token_key_down(p_token));
}

bool PlayerInputRouter::Device::key_slots_down(const char *p_token) const {
	const Ref<ControlsModel> &controls = router_.controls_;
	return controls.is_valid() && controls->is_token_key_down(p_token);
}

bool PlayerInputRouter::Device::binding_active(const char *p_token) const {
	const Ref<ControlsModel> &controls = router_.controls_;
	return controls.is_valid() && controls->is_token_binding_active(p_token);
}

// A keyboard key fires its row through the key queue's events (the shell's KeyboardPass); the
// mouse, joystick and scripted arms here.
bool PlayerInputRouter::Device::held_off_keyboard(const char *p_token) const {
	const Ref<ControlsModel> &controls = router_.controls_;
	return controls.is_valid() && controls->is_token_held_off_keyboard(p_token);
}

bool PlayerInputRouter::Device::scripted_left_button(const char *p_token) const {
	const Ref<ControlsModel> &controls = router_.controls_;
	return controls.is_valid() && controls->is_token_scripted_left_button(p_token);
}

// Shift's sign bit reads the key-state array, which a menu close's reset clears (engine
// controls::KeyStateReset carries the witness).
bool PlayerInputRouter::Device::fire_signed(const char *p_token) const {
	const Ref<ControlsModel> &controls = router_.controls_;
	return controls.is_valid() && controls->is_token_fire_signed(String(p_token));
}

opennova::platform::InputState PlayerInputRouter::Device::state() const {
	return input_device::state();
}

void PlayerInputRouter::Device::set_scan_gates(bool p_death_screen, bool p_scan_word) {
	if (router_.controls_.is_valid()) router_.controls_->set_scan_gates(p_death_screen, p_scan_word);
}

bool PlayerInputRouter::Device::keyboard_captured() const {
	return router_.controls_.is_valid() && router_.controls_->is_keyboard_captured();
}

opennova::inmatch::PlayerInputDevice::ClickRecord PlayerInputRouter::Device::mouse_click_record(
		uint32_t p_event) const {
	ClickRecord out;
	if (router_.controls_.is_null()) return out;
	const ControlsModel::MouseClickRecord record =
			router_.controls_->mouse_click_record(static_cast<int>(p_event));
	out.code = record.code;
	out.sign = record.sign;
	return out;
}

std::vector<opennova::controls::KeyRowEvent> PlayerInputRouter::Device::joystick_records(
		bool p_death_screen, bool p_scroll_lock) {
	if (router_.controls_.is_null()) return {};
	return router_.controls_->joystick_records(p_death_screen, p_scroll_lock);
}

std::vector<opennova::controls::KeyRowEvent> PlayerInputRouter::Device::key_release_pass() {
	const Ref<ControlsModel> &controls = router_.controls_;
	if (controls.is_null()) return {};
	const auto key_state = [&controls](int p_vk) { return controls->is_key_state_down(p_vk); };
	return controls->native_bindings().release_pass(router_.key_events()->press_latch(), key_state);
}

void PlayerInputRouter::Device::reset_key_queue() {
	router_.key_events()->reset();
}

void PlayerInputRouter::Device::reset_key_state() {
	if (router_.controls_.is_valid()) router_.controls_->reset_key_state();
	router_.key_events()->clear_press_latch();
}

bool PlayerInputRouter::Device::step_scripted(bool p_advance, int64_t p_logic_tick,
		float &r_look_x, float &r_look_y) {
	const Ref<ControlsModel> &controls = router_.controls_;
	const Ref<ScriptedInput> scripted =
			controls.is_valid() ? controls->get_scripted_input() : Ref<ScriptedInput>();
	if (scripted.is_null()) return false;
	if (p_advance) controls->advance_scripted_input(p_logic_tick);
	const Vector2 look = scripted->take_look();
	r_look_x = static_cast<float>(look.x);
	r_look_y = static_cast<float>(look.y);
	return true;
}

bool PlayerInputRouter::Device::movement_override(opennova::world::PlayerInput &r_movement) const {
	const Ref<PlayerMoveIntent> &intent = router_.input_override_;
	if (intent.is_null()) return false;
	r_movement.forward = intent->get_forward();
	r_movement.back = intent->get_back();
	r_movement.left = intent->get_left();
	r_movement.right = intent->get_right();
	r_movement.lean_left = intent->get_lean_left();
	r_movement.lean_right = intent->get_lean_right();
	r_movement.jump = intent->get_jump();
	return true;
}

// Retail's auto-run word and its turn-off over the keyboard-lock device (D-NET-563; engine
// controls::PlayerActionPoll::scroll_lock_on and PlayerActionFrame::scroll_lock_off carry the
// witness).
bool PlayerInputRouter::Device::scroll_lock_turn_off() {
	return key_lock_device::set(opennova::controls::kVkScroll, false);
}

// --- the session --------------------------------------------------------------------------

Ref<Simulation> PlayerInputRouter::Session::sim() const {
	return fixed_ != nullptr ? *fixed_ : router_.sim();
}

bool PlayerInputRouter::Session::available() const {
	return sim().is_valid();
}

opennova::inmatch::PlayerInputSession::Facts PlayerInputRouter::Session::facts() const {
	Facts out;
	const Ref<Simulation> s = sim();
	if (s.is_null()) return out;
	const int role = s->session_role();
	out.death_screen = s->local_death_screen_active();
	out.local_dead = s->local_player_dead();
	out.dedicated_host = role == Simulation::ROLE_DEDICATED_HOST;
	out.joiner = role == Simulation::ROLE_JOINER;
	out.mp_session = s->is_mp_session();
	out.game_type = static_cast<uint32_t>(s->get_session_game_type());
	out.logic_tick = s->get_logic_tick();
	out.title = &s->game_title();
	return out;
}

bool PlayerInputRouter::Session::action_head_admits(int p_code) {
	const Ref<Simulation> s = sim();
	return s.is_valid() && s->action_head_admits(p_code);
}

opennova::controls::UseItemPressArm PlayerInputRouter::Session::take_use_item_press() {
	using Arm = opennova::controls::UseItemPressArm;
	const Ref<Simulation> s = sim();
	if (s.is_null()) return Arm::kTaken;
	switch (s->take_use_item_press()) {
		case Simulation::USE_ITEM_ARM_MOUNT_CHAIN: return Arm::kMountChain;
		case Simulation::USE_ITEM_ARM_ARMORY: return Arm::kOpenArmory;
		// The vehicle-loadout arm's request: the shell closes a menu screen up, else opens
		// vehicle.mnu past the team test (engine world::local_player_use_item_arm and
		// local_player_vehicle_zone_team_matches carry the witness).
		case Simulation::USE_ITEM_ARM_VEHICLE_LOADOUT: return Arm::kOpenVehicleMenu;
		default: return Arm::kTaken;
	}
}

void PlayerInputRouter::Session::run_action(const opennova::controls::PlayerActionRequest &p_request) {
	using Action = opennova::controls::PlayerAction;
	const Ref<Simulation> s = sim();
	if (s.is_null()) return;
	Simulation &session = *s.ptr();
	switch (p_request.action) {
		case Action::ToggleMount: session.local_player_toggle_mount(); break;
		case Action::SelectSeat: session.local_player_select_seat(p_request.value); break;
		case Action::ToggleScope: session.request_local_player_scope_toggle(); break;
		case Action::WeaponCategory:
			session.request_local_player_weapon_category(
					static_cast<Simulation::WeaponCategory>(p_request.value));
			break;
		case Action::WeaponCycle: session.request_local_player_weapon_cycle(p_request.value); break;
		case Action::Stance:
			session.request_local_player_stance(static_cast<Simulation::Stance>(p_request.value));
			break;
		case Action::ScopeZero: session.request_local_player_scope_zero(p_request.value); break;
		case Action::RadarZoom: session.request_hud_radar_zoom(p_request.value); break;
		case Action::MapCycle: session.request_hud_map_cycle(); break;
		case Action::Binoculars: session.request_local_player_binoculars_toggle(); break;
		case Action::NightVision: session.request_local_player_nvg_toggle(); break;
		case Action::NvgGain: session.request_local_player_nvg_gain(p_request.value); break;
		case Action::WaypointCycle: session.request_waypoint_cycle(p_request.value); break;
		case Action::Spectate: session.request_spectate_action(p_request.value); break;
		// Sent over the session; the special-key chain closed the menu.
		case Action::EmotePick: session.send_voice_menu_pick(false, p_request.value); break;
		case Action::RadioPick: session.send_voice_menu_pick(true, p_request.value); break;
		// The shell opens the screen (the frame's take_armory_request /
		// take_vehicle_menu_request).
		case Action::OpenArmory: break;
		case Action::OpenVehicleMenu: break;
		// The role's arms (engine inmatch::Role::dispatch_record): never handed here.
		case Action::Respawn: break;
		case Action::MedicRequest: break;
		case Action::Reload: break;
		case Action::ToSpecial: break;
		case Action::Fire: break;
		case Action::UseItem: break;
		// The frame's own records (engine inmatch::PlayerInputFrame::dispatch_record).
		case Action::EmbedderRow: break;
		case Action::ViewToggle: break;
	}
}

bool PlayerInputRouter::Session::dispatch_action_record(int32_t p_code, int32_t p_param) {
	const Ref<Simulation> s = sim();
	return s.is_valid() && s->dispatch_action_record(p_code, p_param);
}

// The 1st Person Toggle over the session's local view (engine world/player_view.h
// player_view_apply_view_action carries the witness).
bool PlayerInputRouter::Session::view_toggle() {
	const Ref<Simulation> s = sim();
	return s.is_valid() && s->apply_local_player_view_action(Simulation::VIEW_ACTION_TOGGLE);
}

void PlayerInputRouter::Session::left_button_down() {
	const Ref<Simulation> s = sim();
	if (s.is_valid()) s->local_player_left_button_down();
}

void PlayerInputRouter::Session::clear_deferred_input() {
	const Ref<Simulation> s = sim();
	if (s.is_valid()) s->clear_deferred_input();
}

// The talk rows' dispatch gates, the capture, the talk debounce and the flood are the engine's
// chat entry (ChatEntryFacts::frame, inmatch/chat_flood.h); the HUD presenter draws the line at
// its frame.
void PlayerInputRouter::Session::dispatch_talk_rows(uint32_t p_rows_fired, bool p_active) {
	const Ref<Simulation> s = sim();
	if (router_.hud_chat_.is_null() || s.is_null()) return;
	router_.hud_chat_->dispatch_rows(static_cast<int>(p_rows_fired), p_active, s,
			router_.gametext_, s->get_main_frame_counter());
}

opennova::inmatch::SpecialKeyOutcome PlayerInputRouter::Session::special_key_chain(
		opennova::hud::HudToggleState &p_toggles, opennova::controls::PlayerActions &p_actions,
		const opennova::inmatch::SpecialKeyInput &p_input) {
	const Ref<Simulation> s = sim();
	if (s.is_null()) return {};
	return s->special_key_chain(p_toggles, p_actions, p_input);
}

// --- the shell ----------------------------------------------------------------------------

void PlayerInputRouter::Shell::keyboard_pass() {
	if (keyboard_pass_.is_valid()) keyboard_pass_.call();
}

bool PlayerInputRouter::Shell::menu_screen_open() {
	return frontend_.is_valid() && frontend_->screen_holds_input();
}

void PlayerInputRouter::Shell::record_leg(int32_t p_code, int32_t p_param) {
	if (record_leg_.is_valid()) record_leg_.call(p_code, p_param);
}
