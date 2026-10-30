// The local player's input frame (inmatch::PlayerInputFrame) over fake seams: the record queue's
// order (the message pump's click and wheel records, the talk and HUD rows' off-keyboard edges,
// then the sample's), the rows' press edges across frames that run no update, each drained
// record's arm (the talk and HUD rows, the shell's records, the held USE row, the movement rows'
// Scroll Lock turn-off, an action request past its head, the session's own records), the mouse
// events and the wheel's notches, the PowerThrow left-down ahead of the drain, the HUD rows' head
// drops and role bits, and the cursor's whole-pixel carry.
#include <runtime/controls/controls.h>
#include <runtime/controls/device_messages.h>
#include <runtime/hud/hud_chat_entry.h>
#include <runtime/hud/hud_toggles.h>
#include <runtime/inmatch/player_input_frame.h>

#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace opennova;
using inmatch::PlayerInputFrame;

int g_failures = 0;

#define CHECK(cond, message)                                                          \
	do {                                                                              \
		if (!(cond)) {                                                                \
			std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, message, #cond); \
			++g_failures;                                                             \
		}                                                                             \
	} while (0)

int code_of(const char *token) {
	std::size_t count = 0;
	const controls::ActionDef *rows = controls::catalog(&count);
	for (std::size_t i = 0; i < count; ++i)
		if (std::string(token) == rows[i].token) return controls::action_code(rows[i].id);
	return -1;
}

struct FakeDevice final : inmatch::PlayerInputDevice {
	std::set<std::string> held;           // tokens held through an off-keyboard arm
	std::vector<std::pair<uint32_t, int>> clicks; // mouse event -> code
	platform::InputState snapshot;
	int turn_offs = 0;
	int queue_resets = 0;
	int state_resets = 0;
	bool pressed(const char *) const override { return false; }
	int pressed_key(const char *) const override { return 0; }
	bool fire_signed(const char *) const override { return false; }
	bool held_off_keyboard(const char *token) const override { return held.count(token) != 0; }
	platform::InputState state() const override { return snapshot; }
	void set_scan_gates(bool, bool) override {}
	bool keyboard_captured() const override { return false; }
	ClickRecord mouse_click_record(uint32_t event) const override {
		for (const auto &click : clicks)
			if (click.first == event) return {click.second, false};
		return {};
	}
	std::vector<controls::KeyRowEvent> joystick_records(bool, bool) override { return {}; }
	std::vector<controls::KeyRowEvent> key_release_pass() override { return {}; }
	void reset_key_queue() override { ++queue_resets; }
	void reset_key_state() override { ++state_resets; }
	bool step_scripted(bool, int64_t, float &, float &) override { return false; }
	bool movement_override(world::PlayerInput &) const override { return false; }
	bool scroll_lock_turn_off() override {
		++turn_offs;
		return true;
	}
};

struct FakeSession final : inmatch::PlayerInputSession {
	Facts session_facts;
	std::set<int> dropped; // codes the head drops
	std::vector<std::string> log;
	bool available() const override { return true; }
	Facts facts() const override { return session_facts; }
	bool action_head_admits(int code) override { return dropped.count(code) == 0; }
	controls::UseItemPressArm take_use_item_press() override {
		log.push_back("use");
		return controls::UseItemPressArm::kOpenArmory;
	}
	void run_action(const controls::PlayerActionRequest &request) override {
		log.push_back("action " + std::to_string(request.code));
	}
	bool dispatch_action_record(int32_t code, int32_t param) override {
		log.push_back("record " + std::to_string(code) + " " + std::to_string(param));
		return true;
	}
	void left_button_down() override { log.push_back("left_down"); }
	void clear_deferred_input() override { log.push_back("clear_deferred"); }
	void dispatch_talk_rows(uint32_t rows, bool) override {
		log.push_back("talk " + std::to_string(rows));
	}
	inmatch::SpecialKeyOutcome special_key_chain(hud::HudToggleState &, controls::PlayerActions &,
			const inmatch::SpecialKeyInput &) override {
		return {};
	}
};

struct FakeShell final : inmatch::PlayerInputShell {
	int passes = 0;
	bool menu = false;
	std::vector<std::pair<int32_t, int32_t>> legs;
	void keyboard_pass() override { ++passes; }
	bool menu_screen_open() override { return menu; }
	void record_leg(int32_t code, int32_t param) override { legs.emplace_back(code, param); }
};

struct Rig {
	FakeDevice device;
	FakeSession session;
	FakeShell shell;
	hud::HudToggleState toggles;
	PlayerInputFrame frame;
	Rig() {
		device.snapshot.cursor_captured = true;
		frame.bind(&device, &session, &shell);
		frame.set_hud_toggles(&toggles);
	}
	// One display frame running `updates` updates, each: rows, sample, records, drain, tail.
	std::vector<controls::InputEventRecord> run_frame(int updates, bool drain = true) {
		std::vector<controls::InputEventRecord> drained;
		frame.begin_frame(true, true, true, false);
		for (int update = 0; update < updates; ++update) {
			frame.run_update_rows(500 + update, update > 0);
			(void)frame.sample_tick(500 + update);
			controls::InputEventQueue queue;
			frame.queue_records(queue);
			controls::InputEventRecord record;
			while (queue.pop(record)) {
				drained.push_back(record);
				if (drain) frame.dispatch_record(record);
			}
			frame.end_input_frame();
		}
		return drained;
	}
};

platform::InputEvent button(platform::MouseButton which, bool down, uint8_t clicks = 1) {
	platform::InputEvent event;
	event.type = platform::InputEvent::Type::MouseButton;
	event.button.button = which;
	event.button.down = down;
	event.button.clicks = clicks;
	return event;
}

platform::InputEvent wheel(float notches) {
	platform::InputEvent event;
	event.type = platform::InputEvent::Type::MouseWheel;
	event.wheel.y = notches;
	return event;
}

void test_device_messages() {
	using platform::MouseButton;
	CHECK(controls::mouse_button_event(button(MouseButton::Left, true).button) == 2, "left down");
	CHECK(controls::mouse_button_event(button(MouseButton::Left, false).button) == 4, "left up");
	CHECK(controls::mouse_button_event(button(MouseButton::Left, true, 2).button) == 0x400,
			"left double click");
	CHECK(controls::mouse_button_event(button(MouseButton::Right, true).button) == 8, "right down");
	CHECK(controls::mouse_button_event(button(MouseButton::Right, false).button) == 0x10,
			"right up");
	CHECK(controls::mouse_button_event(button(MouseButton::Right, true, 2).button) == 0x800,
			"right double click");
	CHECK(controls::mouse_button_event(button(MouseButton::Middle, true).button) == 0x20,
			"middle down");
	CHECK(controls::mouse_button_event(button(MouseButton::Middle, false).button) == 0x40,
			"middle up");
	CHECK(controls::mouse_button_event(button(MouseButton::Middle, true, 2).button) == 0x1000,
			"middle double click");
	CHECK(controls::mouse_button_event(button(MouseButton::X1, true).button) == 0,
			"retail's window procedure sees no X button");
	CHECK(controls::wheel_delta_units(wheel(1.0f).wheel) == 120, "one notch");
	CHECK(controls::wheel_delta_units(wheel(0.5f).wheel) == 60, "half a notch");
	CHECK(controls::wheel_delta_units(wheel(-0.4f).wheel) == -48, "a partial reverse notch");

	platform::KeyEvent key;
	key.key = platform::KeyCode::Return;
	key.scancode = platform::Scancode::KeypadEnter;
	controls::QueuedKey queued = controls::queued_key(key, false, false);
	controls::KeyEvent entry;
	entry.vk = static_cast<uint8_t>(queued.vk);
	entry.flags = queued.flags;
	CHECK(queued.vk == 0x0D && controls::key_event_scan_code(entry) == 269,
			"keypad Enter is VK_RETURN on the extended bit, scanned as 269");
	key.scancode = platform::Scancode::Return;
	key.repeat = true;
	queued = controls::queued_key(key, true, true);
	namespace flag = controls::key_event_flag;
	CHECK(queued.flags == (flag::kCtrl | flag::kShift | flag::kRepeat),
			"the main Enter: Ctrl, Shift and the repeat bit, no extended bit");
	key = platform::KeyEvent{};
	key.key = static_cast<platform::KeyCode>(0x24); // Home, reported with no scancode
	CHECK((controls::queued_key(key, false, false).flags & flag::kExtended) != 0,
			"a navigation key carries the extended bit by its VK");
	CHECK(controls::queued_key(platform::KeyEvent{}, false, false).vk == 0, "no VK queues nothing");
}

void test_record_order_and_mouse() {
	Rig rig;
	const int fire = code_of("attack_1");
	const int wheel_row = code_of("scope");
	const int hud_detail = code_of(hud::hud_toggle_row_token(hud::kRowHudDetail));
	const int escape = controls::action_code(67);
	rig.device.clicks = {{2u, fire}, {controls::kMouseWheelUpEvent, wheel_row}};
	// The message pump's records: a click, then half a notch twice (one whole notch).
	(void)rig.frame.device_event(button(platform::MouseButton::Left, true));
	CHECK(rig.frame.device_event(wheel(0.5f)), "a sub-notch message is the frame's");
	CHECK(rig.frame.device_event(wheel(0.5f)), "the second half completes a notch");
	// A HUD row held through an off-keyboard arm, and a key event for the escape row.
	rig.device.held.insert(hud::hud_toggle_row_token(hud::kRowHudDetail));
	rig.frame.queue_row_event(escape, false, 0x1B);
	const auto drained = rig.run_frame(1, false);
	CHECK(drained.size() == 4, "four records: the click, the notch, the HUD edge, the escape");
	if (drained.size() == 4) {
		CHECK(drained[0].code == fire, "the click record leads (the message pump)");
		CHECK(drained[1].code == wheel_row, "then the wheel notch");
		CHECK(drained[2].code == hud_detail, "then the HUD row's off-keyboard edge");
		CHECK(drained[3].code == escape, "then the sample's key events");
	}
	CHECK(!rig.session.log.empty() && rig.session.log.front() == "left_down",
			"the left button's down runs its PowerThrow test at the pump, ahead of the drain");
	// A double click owes no left-down.
	rig.session.log.clear();
	(void)rig.frame.device_event(button(platform::MouseButton::Left, true, 2));
	(void)rig.run_frame(1, false);
	CHECK(rig.session.log.empty(), "a double click owes no PowerThrow test");
	// The reverse deltas: a partial notch dispatches nothing, its completion one wheel-down.
	const int stand = code_of("Stand");
	rig.device.clicks.emplace_back(controls::kMouseWheelDownEvent, stand);
	(void)rig.frame.device_event(wheel(-0.4f));
	CHECK(rig.run_frame(1, false).empty(), "a partial reverse notch dispatches nothing");
	(void)rig.frame.device_event(wheel(-0.6f));
	const auto reverse = rig.run_frame(1, false);
	CHECK(reverse.size() == 1 && reverse[0].code == stand, "the reverse deltas complete a notch");
}

void test_row_edges_across_frames() {
	Rig rig;
	const char *token = hud::hud_toggle_row_token(hud::kRowHudColor);
	const int code = code_of(token);
	// Three frames that run no update see the row down, up, down: two press edges wait.
	rig.device.held.insert(token);
	rig.frame.begin_frame(true, true, true, false);
	rig.device.held.clear();
	rig.frame.begin_frame(true, true, true, false);
	rig.device.held.insert(token);
	rig.frame.begin_frame(true, true, true, false);
	rig.frame.run_update_rows(500, false);
	(void)rig.frame.sample_tick(500);
	controls::InputEventQueue queue;
	rig.frame.queue_records(queue);
	int edges = 0;
	for (std::size_t i = 0; i < queue.size(); ++i)
		if (queue.at(i).code == code) ++edges;
	CHECK(edges == 2, "a press, its release and a new press each reach the next update");
	// Held on, the next update fires nothing more.
	const auto drained = rig.run_frame(1, false);
	CHECK(drained.empty(), "a held arm fires no second edge");
}

void test_dispatch_arms() {
	Rig rig;
	const int talk = code_of(hud::chat_talk_row_token(0));
	const int escape = controls::action_code(67);
	const int scope = code_of("scope");
	// A talk row's record opens its line.
	rig.frame.dispatch_record({talk, 0, 0, 0, false});
	CHECK(rig.session.log.size() == 1 && rig.session.log[0] == "talk 1", "the talk row's arm");
	// The shell's records reach its leg with their first argument.
	rig.frame.dispatch_record({escape, 0, 0, 0, false});
	rig.frame.dispatch_record({12, 7, 0, 0, false});
	CHECK(rig.shell.legs.size() == 2 && rig.shell.legs[1] == std::make_pair(12, 7),
			"the escape row's and the deploy pick's records are the shell's");
	// The held USE row: the press latch's armory arm.
	rig.session.log.clear();
	rig.frame.dispatch_record({controls::action_code(44), 0, 0, 0, false});
	CHECK(rig.frame.take_armory_request(), "the USE row's armory arm takes its open");
	CHECK(rig.device.turn_offs == 1, "case 0xB1 turns the auto-run off");
	CHECK(rig.session.log.size() == 1 && rig.session.log[0] == "use",
			"the USE row's record passes its head to the press latch");
	// The movement rows: one turn-off a frame, none for the forward key's auto-run event.
	rig.session.log.clear();
	rig.device.turn_offs = 0;
	rig.frame.begin_frame(true, true, true, false);
	rig.frame.dispatch_record({152, 0, 0, 1, false});
	CHECK(rig.device.turn_offs == 0, "the auto-run's own forward event keeps the toggle");
	rig.frame.dispatch_record({147, 0, 0, 0, false});
	CHECK(rig.device.turn_offs == 0, "case 147 turns nothing off");
	rig.frame.dispatch_record({151, 0, 0, 0, false});
	rig.frame.dispatch_record({156, 0, 0, 0, false});
	CHECK(rig.device.turn_offs == 1, "the back and strafe cases turn it off once a frame");
	CHECK(rig.session.log.empty(), "a movement row's record arms no request");
	// An action row past its head, and one the head drops.
	rig.frame.dispatch_record({scope, 0, 0, 0, false});
	rig.session.dropped.insert(scope);
	rig.frame.dispatch_record({scope, 0, 0, 0, false});
	CHECK(rig.session.log.size() == 1 && rig.session.log[0] == "action " + std::to_string(scope),
			"an action request runs past its head only");
	// The session's own record (the exit).
	rig.frame.dispatch_record({3, 0, 0, 0, false});
	CHECK(rig.session.log.back() == "record 3 0", "the exit is the session's record");
}

void test_hud_rows_head_and_roles() {
	Rig rig;
	const uint32_t detail = 1u << hud::kRowHudDetail;
	const uint32_t dotsize = 1u << hud::kRowDotsize;
	// The head drops the row by its catalog row's code: no event.
	rig.session.dropped.insert(controls::action_code(hud::hud_toggle_row_catalog_row(
			hud::kRowHudDetail)));
	rig.frame.begin_frame(true, true, true, false);
	CHECK(!rig.frame.dispatch_hud_rows(rig.session, detail, false, 0), "no pause");
	CHECK(rig.frame.take_hud_toggle_events().empty(), "a dropped row fires nothing");
	rig.session.dropped.clear();
	(void)rig.frame.dispatch_hud_rows(rig.session, detail, false, 0);
	auto events = rig.frame.take_hud_toggle_events();
	CHECK(events.size() == 1 && (events[0] & hud::hud_toggle_event::kHudDetailCycled) != 0,
			"an admitted row cycles");
	// The role bits: a dedicated host in a session is an authority with no peer bit, whose
	// dotsize row's operand gate holds; a listen host carries both bits.
	rig.session.session_facts.mp_session = true;
	rig.session.session_facts.dedicated_host = true;
	(void)rig.frame.dispatch_hud_rows(rig.session, dotsize, false, 0);
	CHECK(rig.frame.take_hud_toggle_events().empty(), "the dedicated host's dotsize holds");
	rig.session.session_facts.dedicated_host = false;
	(void)rig.frame.dispatch_hud_rows(rig.session, dotsize, false, 0);
	events = rig.frame.take_hud_toggle_events();
	CHECK(events.size() == 1 && (events[0] & hud::hud_toggle_event::kDotsizeCycled) != 0,
			"a session peer's dotsize cycles");
}

void test_cursor_and_menu() {
	Rig rig;
	platform::InputEvent motion;
	motion.type = platform::InputEvent::Type::MouseMotion;
	motion.motion.dx = 1.5f;
	motion.motion.dy = 2.25f;
	CHECK(rig.frame.device_event(motion), "motion is the frame's");
	auto head = rig.frame.begin_frame(true, true, true, false);
	CHECK(head.cursor_dx == 1 && head.cursor_dy == 2, "whole pixels at the head");
	motion.motion.dx = 0.6f;
	motion.motion.dy = 0.0f;
	(void)rig.frame.device_event(motion);
	head = rig.frame.begin_frame(true, true, true, false);
	CHECK(head.cursor_dx == 1 && head.cursor_dy == 0, "the sub-pixel rest carries");
	// A menu screen an update opened gates the frame's later updates: no keyboard pass.
	rig.shell.passes = 0;
	rig.shell.menu = true;
	(void)rig.run_frame(2);
	CHECK(rig.shell.passes == 0, "no input frame runs under a menu screen");
	rig.shell.menu = false;
	(void)rig.run_frame(2);
	CHECK(rig.shell.passes == 2, "each update runs its keyboard pass");
	// The Escape key's reset owes the next frame's head the turn-off, once.
	rig.device.turn_offs = 0;
	rig.frame.owe_scroll_lock_turn_off();
	rig.frame.begin_frame(true, true, true, false);
	rig.frame.dispatch_record({151, 0, 0, 0, false});
	CHECK(rig.device.turn_offs == 1, "the owed turn-off is the frame's one");
	// A menu close's input reset: the queue, the key state and the session's deferred queue.
	rig.session.log.clear();
	rig.frame.reset_input_state();
	CHECK(rig.device.queue_resets == 1 && rig.device.state_resets == 1, "the device's reset");
	CHECK(rig.session.log.size() == 1 && rig.session.log[0] == "clear_deferred",
			"the deferred queue's clear");
}

} // namespace

int main() {
	test_device_messages();
	test_record_order_and_mouse();
	test_row_edges_across_frames();
	test_dispatch_arms();
	test_hud_rows_head_and_roles();
	test_cursor_and_menu();
	if (g_failures != 0) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("player_input_frame: ok\n");
	return 0;
}
