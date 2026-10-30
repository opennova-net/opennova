#include "local_input.h"

#include "local_mission.h"

#include <runtime/controls/key_event_queue.h>

namespace opennova::game {

LocalInput::LocalInput(LocalMission &mission, platform::InputDevice &input) :
		mission_(mission),
		input_(input),
		device_(bindings_, &input),
		session_(mission),
		shell_(device_, frame_) {
	frame_.bind(&device_, &session_, &shell_);
}

void LocalInput::pump() {
	const bool live = mission_.session().state() == inmatch::State::Running && mission_.has_player();
	platform::InputEvent event;
	while (input_.poll_event(event)) {
		device_.event(event);
		if (live) (void)frame_.device_event(event);
	}
	device_.set_state(input_.state());
}

void LocalInput::begin_frame(inmatch::FrameInput &frame) {
	const bool player = mission_.has_player();
	const inmatch::PlayerInputFrame::FrameHead head =
			frame_.begin_frame(/*presenter=*/true, player, /*gameplay_input_active=*/true,
					/*menu_open=*/false);
	frame.player.sequence = head.sequence;
	if (!head.sourced) return;
	frame.tick_source = &frame_;
	frame.player.cursor_dx = head.cursor_dx;
	frame.player.cursor_dy = head.cursor_dy;
}

// --- the session ----------------------------------------------------------------------------

bool LocalInput::Session::available() const {
	return mission_.session().state() == inmatch::State::Running ||
			mission_.session().state() == inmatch::State::Paused;
}

inmatch::PlayerInputSession::Facts LocalInput::Session::facts() const {
	Facts out;
	const mission::MissionKernel &kernel = mission_.kernel();
	out.death_screen = kernel.world.spectator.death_screen;
	out.game_type = mission_.game_type();
	out.logic_tick = static_cast<int64_t>(kernel.world.logic_tick);
	return out;
}

bool LocalInput::Session::action_head_admits(int code) {
	return mission_.role().action_head_admits(code);
}

// The USE row's arms (the mount chain, the armory, the vehicle loadout) are the frontend's.
controls::UseItemPressArm LocalInput::Session::take_use_item_press() {
	return controls::UseItemPressArm::kTaken;
}

void LocalInput::Session::run_action(const controls::PlayerActionRequest &request) {
	(void)request;
}

bool LocalInput::Session::dispatch_action_record(int32_t code, int32_t param) {
	(void)code;
	(void)param;
	return false;
}

void LocalInput::Session::dispatch_talk_rows(uint32_t rows_fired, bool active) {
	(void)rows_fired;
	(void)active;
}

inmatch::SpecialKeyOutcome LocalInput::Session::special_key_chain(hud::HudToggleState &toggles,
		controls::PlayerActions &actions, const inmatch::SpecialKeyInput &input) {
	(void)toggles;
	(void)actions;
	(void)input;
	return {};
}

// --- the shell ------------------------------------------------------------------------------

void LocalInput::Shell::keyboard_pass() {
	controls::run_keyboard_pass(device_.key_queue(), device_.press_latch(), *this);
}

void LocalInput::Shell::record_leg(int32_t code, int32_t param) {
	(void)code;
	(void)param;
}

void LocalInput::Shell::release_pass() {
	frame_.run_key_release_pass();
}

void LocalInput::Shell::dequeued(const controls::KeyEvent &event, int slot) {
	(void)event;
	(void)slot;
}

bool LocalInput::Shell::chat_key(const controls::KeyEvent &event) {
	(void)event;
	return false;
}

bool LocalInput::Shell::special_key(const controls::KeyEvent &event) {
	(void)event;
	return false;
}

int LocalInput::Shell::binding_code(int scan_code) {
	return device_.event_code(scan_code, sign_);
}

void LocalInput::Shell::row_event(const controls::KeyEvent &event, int code, int scan_code) {
	(void)event;
	frame_.queue_row_event(code, sign_, scan_code);
}

} // namespace opennova::game
