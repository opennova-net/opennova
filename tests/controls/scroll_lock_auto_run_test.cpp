// Scroll Lock is retail's auto-run (D-NET-563): every input frame the Scroll Lock word reads on
// queues the forward event with its fourth argument 1, as the held Last_move row does, and the
// forward event a key queues itself (fourth argument 0), the back and strafe keys, the held USE
// row, its release tail and the Respawn press turn the toggle off, each by its dispatch code.
// Synthetic device streams through the public action poll; the codes are retail's case labels.
// [orig: Input_ProcessPlayerFrame @0x49D4C0 - Input_IsScrollLockOn (the call @0x49D4E6) ->
//  Input_QueueEvent(152, 0, 1, 1) @0x49D4FA; Input_HandleActionBinding_0 @0x4E0420 - code 425
//  @0x4E046F -> Input_QueueEvent(152, 0, 0, 1) @0x4E13ED, case 151 @0x4E0C66, case 152 (the
//  fourth argument's test @0x4E0C92, Input_DisableScrollLock @0x4E0C9E), case 156 @0x4E1021,
//  case 157 @0x4E105E, case 0xB1 @0x4E0A84; Input_HandleActionBinding case 55 @0x49B20B ->
//  Player_ResetLocalInput (the call @0x4B2B91); Input_ProcessFrame - the USE release tail
//  @0x49D6C5, ahead of its consumed test @0x49D6CD]
#include <runtime/controls/player_actions.h>

#include <algorithm>
#include <functional>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace opennova::controls;

namespace {

#define CHECK(condition) do { \
	if (!(condition)) { \
		std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; \
		return false; \
	} \
} while (false)

struct Keys : PlayerActionSource {
	std::set<std::string> held;
	bool pressed(const char *token) const override { return held.count(token) != 0; }
	int pressed_key(const char *) const override { return 0; }
	bool fire_signed(const char *) const override { return false; }
	bool keys_down(const char *token) const override { return pressed(token); }
};

// One input frame as the embedder runs it: the poll, then the drain's held USE record through
// `arm` (the press latch's arm; the mount chain when unset), then the frame's tail; the
// frame's requests and Scroll Lock turn-offs in that order, the USE record replaced by its
// arm's (PlayerActions::use_item_record, end_input_frame).
// [orig: Input_ProcessFrame @0x49D520 - the drain @0x49D599..0x49D5E7, the release edge
//  @0x49D6C1..0x49D6DC; Input_HandleActionBinding_0 case 0xB1 @0x4E0A84]
PlayerActionFrame poll_frame(PlayerActions &actions, const PlayerActionSource &source,
		const PlayerActionPoll &gate, const std::function<UseItemPressArm()> &arm = {}) {
	PlayerActionFrame frame = actions.poll(source, gate);
	std::vector<PlayerActionRequest> requests;
	for (const PlayerActionRequest &request : frame.requests) {
		if (request.action != PlayerAction::UseItem) {
			requests.push_back(request);
			continue;
		}
		const PlayerActionFrame use =
				actions.use_item_record(arm ? arm() : UseItemPressArm::kMountChain);
		requests.insert(requests.end(), use.requests.begin(), use.requests.end());
		frame.scroll_lock_off.insert(frame.scroll_lock_off.end(), use.scroll_lock_off.begin(),
				use.scroll_lock_off.end());
	}
	const PlayerActionFrame tail = actions.end_input_frame(gate.simulation_available);
	requests.insert(requests.end(), tail.requests.begin(), tail.requests.end());
	frame.scroll_lock_off.insert(frame.scroll_lock_off.end(), tail.scroll_lock_off.begin(),
			tail.scroll_lock_off.end());
	frame.requests = std::move(requests);
	return frame;
}

PlayerActionPoll live(bool scroll_lock_on) {
	PlayerActionPoll gate;
	gate.active = true;
	gate.captured = true;
	gate.simulation_available = true;
	gate.scroll_lock_on = scroll_lock_on;
	return gate;
}

bool offs_are(const PlayerActionFrame &frame, std::vector<int> expected) {
	std::vector<int> got = frame.scroll_lock_off;
	std::sort(got.begin(), got.end());
	std::sort(expected.begin(), expected.end());
	return got == expected;
}

// The word queues the forward event every input frame it reads on and turns nothing off.
bool test_the_word_walks_forward() {
	PlayerActions actions(opennova::controls::default_catalog());
	Keys keys;
	for (int frame = 0; frame < 3; ++frame) {
		const PlayerActionFrame out = poll_frame(actions, keys, live(true));
		CHECK(out.auto_run_forward && out.scroll_lock_off.empty() && out.requests.empty());
	}
	const PlayerActionFrame off = poll_frame(actions, keys, live(false));
	CHECK(!off.auto_run_forward && off.scroll_lock_off.empty());
	return true;
}

// No input frame runs under a menu screen; an inactive frame (an overlay) takes no forward key.
bool test_menu_and_inactive_frames() {
	PlayerActions actions(opennova::controls::default_catalog());
	Keys keys;
	PlayerActionPoll menu = live(true);
	menu.menu_open = true;
	CHECK(!poll_frame(actions, keys, menu).auto_run_forward);
	PlayerActionPoll inactive = live(true);
	inactive.active = false;
	CHECK(!poll_frame(actions, keys, inactive).auto_run_forward);
	return true;
}

// The held Last_move row queues the same forward event, with its fourth argument 1: no
// turn-off.
bool test_last_move_row() {
	PlayerActions actions(opennova::controls::default_catalog());
	Keys keys;
	keys.held.insert("Last_move");
	const PlayerActionFrame out = poll_frame(actions, keys, live(false));
	CHECK(out.auto_run_forward && out.scroll_lock_off.empty());
	return true;
}

// A key's own forward, back and strafe events turn the toggle off every frame they are held,
// by their case codes 152, 151, 156 and 157; the word still queues its forward event.
bool test_movement_keys_turn_it_off() {
	PlayerActions actions(opennova::controls::default_catalog());
	Keys keys;
	keys.held.insert("move_forward");
	CHECK(offs_are(poll_frame(actions, keys, live(true)), {152}));
	CHECK(offs_are(poll_frame(actions, keys, live(true)), {152}));
	keys.held = {"move_back", "strafe_left", "strafe_right"};
	const PlayerActionFrame out = poll_frame(actions, keys, live(true));
	CHECK(out.auto_run_forward && offs_are(out, {151, 156, 157}));
	// The death screen admits the movement rows (mode word 3).
	PlayerActionPoll dead = live(true);
	dead.death_screen = true;
	keys.held = {"move_forward"};
	CHECK(offs_are(poll_frame(actions, keys, dead), {152}));
	return true;
}

// The held USE row's case (0xB1) turns it off every frame it runs; its release tail turns it
// off past no head (-1), whether or not a seat pick consumed the hold.
bool test_use_row_and_release_tail() {
	PlayerActions actions(opennova::controls::default_catalog());
	Keys keys;
	keys.held.insert("useitem");
	CHECK(offs_are(poll_frame(actions, keys, live(false)), {177}));
	keys.held.clear();
	const PlayerActionFrame released = poll_frame(actions, keys, live(false));
	CHECK(offs_are(released, {-1}) && released.requests.size() == 1 &&
			released.requests[0].action == PlayerAction::ToggleMount);

	PlayerActions consumed(opennova::controls::default_catalog());
	keys.held = {"useitem"};
	(void)poll_frame(consumed, keys, live(false));
	const ChainDigitTake picked = consumed.chain_digit_key('1', false, false);
	CHECK(picked.pick && picked.pick->action == PlayerAction::SelectSeat);
	CHECK(poll_frame(consumed, keys, live(false)).requests.empty());
	keys.held.clear();
	const PlayerActionFrame tail = poll_frame(consumed, keys, live(false));
	CHECK(offs_are(tail, {-1}) && tail.requests.empty());
	return true;
}

// The Respawn press's input reset turns it off by code 55, on its press edge only.
bool test_respawn_press() {
	PlayerActions actions(opennova::controls::default_catalog());
	Keys keys;
	keys.held.insert("respawn");
	const PlayerActionFrame pressed = poll_frame(actions, keys, live(false));
	CHECK(pressed.requests.size() == 1 && pressed.requests[0].action == PlayerAction::Respawn &&
			offs_are(pressed, {55}));
	CHECK(poll_frame(actions, keys, live(false)).scroll_lock_off.empty());
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = test_the_word_walks_forward() && ok;
	ok = test_menu_and_inactive_frames() && ok;
	ok = test_last_move_row() && ok;
	ok = test_movement_keys_turn_it_off() && ok;
	ok = test_use_row_and_release_tail() && ok;
	ok = test_respawn_press() && ok;
	if (!ok) return 1;
	std::cout << "controls_scroll_lock_auto_run: ok\n";
	return 0;
}
