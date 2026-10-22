// The device's key queue and the binding scan per dequeued key event (the first slice
// of D-CTRL-6's event-queue port): every key-down message, an OS auto-repeat included,
// is one queued event, and the keyboard pass fires one row per event it takes.
#include <runtime/controls/binding_set.h>
#include <runtime/controls/key_event_queue.h>

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <set>

using namespace opennova::controls;
namespace flag = opennova::controls::key_event_flag;

namespace {

#define CHECK(condition) do { \
	if (!(condition)) { \
		std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; \
		return false; \
	} \
} while (false)

// The queue keeps each press's VK and flag word in order, the character byte 0, and
// reads empty once the read index meets the write index.
// [orig: Input_QueueKeyEvent @0x760c10 (@0x760c3c..0x760c7b); Input_DequeueKeyEvent
//  @0x760d60 (@0x760d6b, @0x760d7a)]
bool test_queue_keeps_each_press_in_order() {
	KeyEventQueue queue;
	KeyEvent event;
	CHECK(queue.empty() && !queue.pop(event));
	queue.push(0x75, 0);
	queue.push(0x75, flag::kRepeat);
	queue.push('1', flag::kCtrl);
	queue.push(0x0D, flag::kExtended | flag::kShift);
	CHECK(queue.pop(event) && event.vk == 0x75 && event.flags == 0 && event.character == 0);
	CHECK(queue.pop(event) && event.vk == 0x75 && event.flags == flag::kRepeat);
	CHECK(queue.pop(event) && event.vk == '1' && event.flags == flag::kCtrl);
	CHECK(queue.pop(event) && event.vk == 0x0D &&
			event.flags == (flag::kExtended | flag::kShift));
	CHECK(queue.empty() && !queue.pop(event));
	queue.push('A', 0);
	queue.reset();
	CHECK(queue.empty());
	return true;
}

// No full test: the write index wraps over unread slots, so 128 unread presses read as
// an empty queue, and two more leave those two readable.
// [orig: Input_QueueKeyEvent - `& 0x7F` @0x760c2d; Input_DequeueKeyEvent @0x760d6b]
bool test_queue_wraps_without_a_full_test() {
	KeyEventQueue queue;
	KeyEvent event;
	for (int i = 0; i < KeyEventQueue::kCapacity; ++i) queue.push(0x41 + i % 26, 0);
	CHECK(queue.empty() && !queue.pop(event));
	queue.push('X', 0);
	queue.push('Y', 0);
	CHECK(queue.pop(event) && event.vk == 'X');
	CHECK(queue.pop(event) && event.vk == 'Y');
	CHECK(!queue.pop(event));
	return true;
}

// Keypad Enter (VK_RETURN with the extended bit) scans as 269; the main Enter and any
// other extended key scan as their VK.
// [orig: Input_IsExtendedEnterKey @0x760f40; Input_ProcessKeyboardEvents @0x49d310 /
//  @0x49d31c]
bool test_keypad_enter_scans_as_269() {
	CHECK(key_event_scan_code({0x0D, 0, flag::kExtended}) == 269);
	CHECK(key_event_scan_code({0x0D, 0, flag::kExtended | flag::kRepeat}) == 269);
	CHECK(key_event_scan_code({0x0D, 0, 0}) == 0x0D);
	CHECK(key_event_scan_code({0x2D, 0, flag::kExtended}) == 0x2D);
	return true;
}

// Each dequeued event fires the row its scan finds, an OS repeat like a press: a held
// bare F6 fires huddetail once per event (a press and two repeats, three fires) where a
// held-state edge fires once. Ctrl+F6 is hudcolor's (the modifier pass), Ctrl+1 seat1's,
// a bare 1 the Knife's (the fallback), signed with Shift held. Ctrl's own key-down event
// (VK 0x11) fires no row: no catalog row carries it, so a key already held fires nothing
// new as Ctrl goes down.
// [orig: Input_ProcessKeyboardEvents @0x49d1f0 - the dequeue loop @0x49d2d2..0x49d4a0, the
//  modifier pass's fire @0x49d42f, the fallback's sign @0x49d452..0x49d470 and fire
//  @0x49d488; Input_QueueKeyEvent's repeat bit @0x760c7b]
bool test_each_event_fires_its_row() {
	BindingSet set;
	std::set<int> down;
	const auto key_down = [&](int vk) { return down.count(vk) != 0; };
	const int huddetail = set.index_of_token("huddetail");
	const int hudcolor = set.index_of_token("hudcolor");
	const int seat1 = set.index_of_token("seat1");
	const int knife = set.index_of_token("Knife");
	CHECK(huddetail >= 0 && hudcolor >= 0 && seat1 >= 0 && knife >= 0);

	KeyEventQueue queue;
	down = {0x75};
	queue.push(0x75, 0);
	queue.push(0x75, flag::kRepeat);
	queue.push(0x75, flag::kRepeat);
	int fires = 0;
	KeyEvent event;
	while (queue.pop(event))
		if (set.event_row(key_event_scan_code(event), key_down) == huddetail) ++fires;
	CHECK(fires == 3);

	down = {0x11, 0x75};
	CHECK(set.event_row(0x75, key_down) == hudcolor);
	down = {0x11, '1'};
	CHECK(set.event_row('1', key_down) == seat1);
	CHECK(set.event_row(0x11, key_down) == -1);
	bool signed_fire = true;
	down = {'1'};
	CHECK(set.event_row('1', key_down, &signed_fire) == knife && !signed_fire);
	down = {0x10, '1'};
	CHECK(set.event_row('1', key_down, &signed_fire) == knife && signed_fire);
	down = {0x11, '1'};
	CHECK(set.event_row('1', key_down, &signed_fire) == seat1 && !signed_fire);
	return true;
}

// The keyboard pass's release pass over its press latch. Every dequeued event sets
// its key's latch; a latched key that reads up queues its release row's event and
// clears the latch: ToSpecial's (code 220), the only keyboard-bound row whose flags
// carry the release bit, signed while Shift is down; a held key queues nothing; an open
// text line clears the latch with no event; a key no release row holds keeps its latch;
// the whole input reset's clear drops a held key's release; a rebound ToSpecial
// releases from its new key.
// [orig: Input_ProcessKeyboardEvents @0x49d1f0 - the latch set @0x49d2ed, the release
//  pass @0x49d240..0x49d2b9 (the walk @0x49d252..0x49d27f, Shift @0x49d283, the capture
//  word @0x49d29f, the queue @0x49d2b1, the clear @0x49d2b9, no row @0x49d281);
//  Input_ResetAllState @0x497ED0 - the memset @0x497F08]
bool test_release_pass_follows_the_press_latch() {
	BindingSet set;
	KeyPressLatch latch;
	std::set<int> down;
	const auto key_state = [&](int vk) { return down.count(vk) != 0; };
	const int to_special = set.index_of_token("ToSpecial");
	CHECK(to_special >= 0 && set.release_row('F') == to_special);
	CHECK(set.release_row('W') == -1); // move_forward carries no release bit
	KeyEvent f;
	f.vk = 'F';
	latch.set(f);
	down = {'F'};
	CHECK(set.release_pass(latch, key_state).empty() && latch.is_set('F')); // held
	down.clear();
	auto events = set.release_pass(latch, key_state);
	CHECK(events.size() == 1 && events[0].code == 220 && events[0].release &&
			!events[0].sign && events[0].key == 'F');
	CHECK(!latch.is_set('F') && set.release_pass(latch, key_state).empty());
	latch.set(f);
	down = {0x10};
	events = set.release_pass(latch, key_state);
	CHECK(events.size() == 1 && events[0].code == 220 && events[0].sign);
	latch.set(f);
	down.clear();
	set.set_keyboard_captured(true);
	CHECK(set.release_pass(latch, key_state).empty() && !latch.is_set('F'));
	set.set_keyboard_captured(false);
	KeyEvent w;
	w.vk = 'W';
	latch.set(w);
	CHECK(set.release_pass(latch, key_state).empty() && latch.is_set('W'));
	latch.set(f);
	down = {'F'};
	latch.clear();
	down.clear();
	CHECK(set.release_pass(latch, key_state).empty() && !latch.is_set('W'));
	CHECK(set.assign_key(to_special, 'G', false, false, false, false));
	CHECK(set.release_row('G') == to_special);
	KeyEvent g;
	g.vk = 'G';
	latch.set(g);
	events = set.release_pass(latch, key_state);
	CHECK(events.size() == 1 && events[0].code == 220 && events[0].key == 'G');
	return true;
}

} // namespace

int main() {
	int failed = 0;
	for (const auto test : {test_queue_keeps_each_press_in_order,
				 test_queue_wraps_without_a_full_test, test_keypad_enter_scans_as_269,
				 test_each_event_fires_its_row, test_release_pass_follows_the_press_latch})
		if (!test()) ++failed;
	std::cout << "controls_key_event_queue: " << failed << " failed\n";
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
