// Synthetic device streams through the public action poll, plus the real
// binding sampler for modifier claims and remapping. No production-source pins.
#include <runtime/controls/binding_set.h>
#include <runtime/controls/player_actions.h>

#include <array>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <map>
#include <set>
#include <string>

using namespace opennova::controls;
using Action = PlayerAction;

namespace {

#define CHECK(condition) do { \
	if (!(condition)) { \
		std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; \
		return false; \
	} \
} while (false)

constexpr PlayerActionPoll kLive{true, true, true};

struct Keys : PlayerActionSource {
	std::map<std::string, int> held;
	std::array<bool, 10> digits{};
	void hold(const char *token, int vk = 0) { held[token] = vk; }
	void release(const char *token) { held.erase(token); }
	bool pressed(const char *token) const override { return held.count(token) != 0; }
	int pressed_key(const char *token) const override {
		const auto it = held.find(token);
		return it == held.end() ? 0 : it->second;
	}
	bool digit_down(int digit) const override { return digits[digit]; }
	bool shift = false;
	bool shift_down() const override { return shift; }
};

bool requests_are(const PlayerActionFrame &frame, const std::vector<PlayerActionRequest> &expected) {
	if (frame.requests.size() != expected.size()) return false;
	for (std::size_t i = 0; i < expected.size(); ++i)
		if (frame.requests[i].action != expected[i].action ||
				frame.requests[i].value != expected[i].value) return false;
	return true;
}

bool test_order_and_held_rows() {
	PlayerActions actions;
	Keys keys;
	keys.hold("useitem");
	CHECK(requests_are(actions.poll(keys, kLive), {}));
	keys.release("useitem");
	// A USE release and every binding arrive together. Request order affects
	// mount/weapon/stance admission, so assert the emitted stream in full.
	for (const char *token : {"attack_1", "magazine", "scope", "MedicReq", "seat1", "seat2",
			"seat3", "seat4", "seat5", "seat6", "seat7", "seat8", "seat9", "seat10", "Knife",
			"Secondary", "Primary", "Flashbang", "FragGrenade", "SmokeGrenade", "Accessory",
			"Detonator", "medpack", "cycleweaponP", "cycleweaponN", "Stand", "Crouch", "Prone",
			"ScopeZeroDec", "ScopeZeroInc", "radarout", "radarin", "map_toggle", "binoculars",
			"NVG", "nvggainup", "nvggaindown", "NextWaypoint"}) keys.hold(token);
	auto frame = actions.poll(keys, kLive);
	CHECK(frame.fire_held && frame.fire_edge && frame.reload_edge && frame.medic_edge);
	std::vector<PlayerActionRequest> expected{{Action::ToggleMount}, {Action::ToggleScope}};
	for (int seat = 0; seat < 10; ++seat) expected.push_back({Action::SelectSeat, seat});
	for (int category = 1; category <= 9; ++category)
		expected.push_back({Action::WeaponCategory, category});
	for (const auto request : {PlayerActionRequest{Action::WeaponCycle, 1}, {Action::WeaponCycle, -1},
			{Action::Stance, 0}, {Action::Stance, 1}, {Action::Stance, 2}, {Action::ScopeZero, -1},
			{Action::ScopeZero, 1}, {Action::RadarZoom, 1}, {Action::RadarZoom, -1}, {Action::MapCycle},
			{Action::Binoculars}, {Action::NightVision}, {Action::NvgGain, 1}, {Action::NvgGain, -1},
			{Action::WaypointCycle, 1}})
		expected.push_back(request);
	CHECK(requests_are(frame, expected));
	frame = actions.poll(keys, kLive);
	CHECK(frame.fire_held && !frame.fire_edge && !frame.reload_edge && !frame.medic_edge);
	CHECK(frame.requests.empty());
	return true;
}

bool test_capture_and_overlay_edges() {
	PlayerActions actions;
	Keys keys;
	for (const char *token : {"attack_1", "magazine", "scope", "MedicReq", "Knife", "seat1",
			"cycleweaponN", "Crouch", "radarout", "map_toggle"}) keys.hold(token);
	auto frame = actions.poll(keys, {false, true, true});
	CHECK(!frame.fire_held && !frame.fire_edge && !frame.reload_edge && frame.medic_edge);
	CHECK(frame.requests.empty());
	frame = actions.poll(keys, kLive);
	CHECK(frame.fire_held && frame.fire_edge && frame.reload_edge && !frame.medic_edge);
	CHECK(requests_are(frame, {{Action::ToggleScope}})); // raw held rows do not re-fire
	frame = actions.poll(keys, {true, false, true});
	CHECK(!frame.fire_held && frame.requests.empty());
	frame = actions.poll(keys, kLive);
	CHECK(frame.fire_edge && frame.reload_edge);
	CHECK(requests_are(frame, {{Action::ToggleScope}}));
	keys = Keys{};
	actions.poll(keys, kLive);
	keys.hold("Knife"); keys.hold("Crouch"); keys.hold("radarout");
	frame = actions.poll(keys, {true, false, true});
	CHECK(requests_are(frame, {{Action::Stance, 1}, {Action::RadarZoom, 1}}));
	CHECK(actions.poll(keys, kLive).requests.empty()); // category consumed without capture
	return true;
}

bool test_missing_simulation_freezes_only_scope_zero() {
	PlayerActions actions;
	Keys keys;
	for (const char *token : {"attack_1", "magazine", "scope", "MedicReq", "seat1", "Knife",
			"cycleweaponP", "Prone", "ScopeZeroDec", "ScopeZeroInc", "radarin", "map_toggle"})
		keys.hold(token);
	auto frame = actions.poll(keys, {true, true, false});
	CHECK(frame.fire_edge && frame.reload_edge && frame.medic_edge && frame.requests.empty());
	frame = actions.poll(keys, kLive);
	CHECK(!frame.fire_edge && !frame.reload_edge && !frame.medic_edge);
	CHECK(requests_are(frame, {{Action::ScopeZero, -1}, {Action::ScopeZero, 1}}));
	keys = Keys{};
	actions.poll(keys, {true, true, false}); // zero's held latch does not see this release
	keys.hold("ScopeZeroInc"); keys.hold("map_toggle");
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::MapCycle}}));
	keys.release("ScopeZeroInc");
	actions.poll(keys, kLive);
	keys.hold("ScopeZeroInc");
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::ScopeZero, 1}}));
	return true;
}

bool test_reset_preserves_switch_hud_medic_and_use_state() {
	PlayerActions actions;
	Keys keys;
	for (const char *token : {"attack_1", "magazine", "scope", "MedicReq", "seat1", "Knife",
			"cycleweaponP", "Stand", "Crouch", "Prone", "ScopeZeroDec", "ScopeZeroInc",
			"radarout", "radarin", "map_toggle", "useitem"}) keys.hold(token);
	actions.poll(keys, kLive);
	actions.reset();
	auto frame = actions.poll(keys, kLive);
	CHECK(frame.fire_edge && frame.reload_edge && !frame.medic_edge);
	CHECK(requests_are(frame, {{Action::ToggleScope}, {Action::Stance, 0}, {Action::Stance, 1},
			{Action::Stance, 2}, {Action::ScopeZero, -1}, {Action::ScopeZero, 1}}));
	keys.digits[8] = true;
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::SelectSeat, 7}}));
	actions.reset();
	keys.held.clear(); // consumed USE and the raw digit latch both survive reset
	CHECK(actions.poll(keys, kLive).requests.empty());
	return true;
}

bool test_use_previous_frame_and_single_seat() {
	PlayerActions actions;
	Keys keys;
	keys.hold("useitem"); keys.hold("Knife", '1'); keys.digits[1] = true;
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::WeaponCategory, 1}}));
	CHECK(actions.poll(keys, kLive).requests.empty()); // USE was not held before initial 1
	keys.release("Knife"); keys.digits[1] = false;
	actions.poll(keys, kLive);
	keys.hold("radarout", '9'); keys.digits[9] = true;
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::SelectSeat, 8}}));
	keys.digits[0] = true;
	CHECK(actions.poll(keys, kLive).requests.empty()); // one seat per hold
	keys.release("useitem");
	CHECK(actions.poll(keys, kLive).requests.empty()); // consumed release never mounts
	CHECK(actions.poll(keys, kLive).requests.empty()); // swallowed held row cannot re-fire
	keys.release("radarout");
	actions.poll(keys, kLive);
	keys.hold("radarout", '9');
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::RadarZoom, 1}}));
	return true;
}

bool test_use_release_digit_priority_and_no_sim_consumption() {
	PlayerActions actions;
	Keys keys;
	keys.hold("useitem");
	actions.poll(keys, kLive);
	keys.release("useitem"); keys.digits[0] = true; keys.digits[1] = true;
	keys.hold("Knife", '1'); keys.hold("seat10", '0'); keys.hold("scope");
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::SelectSeat, 9}, {Action::ToggleScope}}));
	// Even an absent sim consumes the special digit, and prevents a later
	// release from unexpectedly mounting when a sim becomes available.
	PlayerActions no_sim;
	keys = Keys{}; keys.hold("useitem");
	no_sim.poll(keys, kLive);
	keys.digits[4] = true;
	CHECK(no_sim.poll(keys, {true, true, false}).requests.empty());
	keys.release("useitem");
	CHECK(no_sim.poll(keys, kLive).requests.empty());
	return true;
}

bool test_shell_consume_and_overlay_cancel() {
	PlayerActions actions;
	Keys keys;
	actions.consume_use_hold(); // a same-frame shell chord survives fresh-press reset
	actions.reset();
	keys.hold("useitem");
	actions.poll(keys, kLive);
	keys.release("useitem");
	CHECK(actions.poll(keys, kLive).requests.empty());
	keys.hold("useitem");
	actions.poll(keys, kLive);
	keys.digits[2] = true;
	CHECK(actions.poll(keys, {false, false, true}).requests.empty());
	CHECK(actions.poll(keys, kLive).requests.empty()); // inactive frame aged raw digits
	keys.release("useitem");
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::ToggleMount}})); // fresh hold
	keys.hold("useitem"); actions.poll(keys, kLive);
	keys.release("useitem");
	CHECK(actions.poll(keys, {false, false, true}).requests.empty());
	CHECK(actions.poll(keys, kLive).requests.empty()); // overlay canceled the release
	return true;
}

bool test_use_swallows_only_digit_event_rows() {
	for (const int vk : {0, 0x2f, 0x30, 0x39, 0x3a, 0x60}) {
		PlayerActions actions;
		Keys keys;
		keys.hold("useitem"); actions.poll(keys, kLive);
		for (const char *token : {"attack_1", "magazine", "scope", "MedicReq", "Primary",
				"seat5", "cycleweaponN", "Prone", "ScopeZeroInc", "radarout", "map_toggle"})
			keys.hold(token, vk);
		const auto frame = actions.poll(keys, kLive);
		CHECK(frame.fire_edge && frame.reload_edge && frame.medic_edge);
		if (vk == 0x30 || vk == 0x39) {
			CHECK(requests_are(frame, {{Action::ToggleScope}}));
		} else {
			CHECK(requests_are(frame, {{Action::ToggleScope}, {Action::SelectSeat, 4},
					{Action::WeaponCategory, 3}, {Action::WeaponCycle, -1}, {Action::Stance, 2},
					{Action::ScopeZero, 1}, {Action::RadarZoom, 1}, {Action::MapCycle}}));
		}
	}
	return true;
}

struct BoundKeys : PlayerActionSource {
	BindingSet bindings;
	std::set<int> down;
	bool pressed(const char *token) const override { return pressed_key(token) != 0; }
	int pressed_key(const char *token) const override {
		return bindings.pressed_key(bindings.index_of_token(token),
				[this](int vk) { return down.count(vk) != 0; });
	}
	bool digit_down(int digit) const override { return down.count('0' + digit) != 0; }
	bool shift_down() const override { return down.count(16) != 0; }
};

bool test_live_binding_modifier_remap_and_use_stream() {
	PlayerActions actions;
	BoundKeys keys;
	const int radar = keys.bindings.index_of_token("radarout");
	CHECK(keys.bindings.assign_key(radar, '7', false, false, false, false));
	keys.down = {'7'};
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::WeaponCategory, 7}, {Action::RadarZoom, 1}}));
	keys.down.clear(); actions.poll(keys, kLive);
	keys.down = {17, '7'}; // Ctrl's modified seat row claims the digit from both bare rows
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::SelectSeat, 6}}));
	keys.down.clear(); actions.poll(keys, kLive);
	keys.down = {16}; actions.poll(keys, kLive); // Shift is USE; special keys run first
	keys.down.insert('7');
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::SelectSeat, 6}}));
	keys.down.clear();
	CHECK(actions.poll(keys, kLive).requests.empty());
	keys.down = {'7'};
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::WeaponCategory, 7}, {Action::RadarZoom, 1}}));
	return true;
}

// The live table keeps a bare '=' on radarin alone: NVG gain needs its Ctrl
// modifier (rows 45/46 carry VK_CONTROL), so Ctrl+'=' fires the gain row and
// the modifier pass claims the key from radarin. Shift+F7 cycles the waypoint
// backward. [orig: Input_ProcessKeyboardEvents @0x49d327..0x49d3ac (modifier
// pass), @0x49d3ba..0x49d488 (fallback), @0x49d452 (the Shift direction bit)]
bool test_nvg_gain_needs_ctrl_and_shift_reverses_waypoint() {
	PlayerActions actions;
	BoundKeys keys;
	keys.down = {0xBB};
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::RadarZoom, -1}}));
	keys.down.clear(); actions.poll(keys, kLive);
	keys.down = {17, 0xBB};
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::NvgGain, 1}}));
	keys.down.clear(); actions.poll(keys, kLive);
	keys.down = {17, 0xBD};
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::NvgGain, -1}}));
	keys.down.clear(); actions.poll(keys, kLive);
	keys.down = {0x76};
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::WaypointCycle, 1}}));
	keys.down.clear(); actions.poll(keys, kLive);
	keys.down = {16}; actions.poll(keys, kLive);
	keys.down.insert(0x76);
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::WaypointCycle, -1}}));
	keys.down.clear(); actions.poll(keys, kLive);
	keys.down = {'B'};
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::Binoculars}}));
	keys.down = {'N'};
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::NightVision}}));
	return true;
}

bool test_wheel_subset_and_repeated_events() {
	struct WheelCase { const char *token; Action action; int value; };
	for (const auto &row : {WheelCase{"cycleweaponP", Action::WeaponCycle, 1},
			{"cycleweaponN", Action::WeaponCycle, -1}, {"ScopeZeroDec", Action::ScopeZero, -1},
			{"ScopeZeroInc", Action::ScopeZero, 1}, {"Prone", Action::Stance, 2},
			{"Crouch", Action::Stance, 1}, {"Stand", Action::Stance, 0}}) {
		for (int event = 0; event < 2; ++event) {
			const auto request = player_wheel_action(row.token);
			CHECK(request && request->action == row.action && request->value == row.value);
		}
	}
	for (const char *token : {"", "unknown", "scope", "attack_1", "Knife", "seat1", "radarout",
			"map_toggle", "MedicReq", "useitem", "stand"}) CHECK(!player_wheel_action(token));
	return true;
}

} // namespace

// The WM_MOUSEWHEEL accumulator: whole +/-120 notches dispatch, one event per
// notch, and the remainder (either sign) carries into the next message, so a
// high-resolution wheel's sub-notch messages add up before anything fires.
// [orig: Input_DispatchMouseEvent @ 0x7614A9..0x7615DB]
bool test_wheel_remainder_dispatches_whole_notches() {
	WheelRemainder wheel;
	CHECK(wheel.feed(kWheelDelta) == 1);
	CHECK(wheel.feed(-kWheelDelta) == -1);
	CHECK(wheel.feed(60) == 0);
	CHECK(wheel.feed(60) == 1);
	CHECK(wheel.feed(240) == 2);      // a coalesced message fires two events
	CHECK(wheel.feed(-50) == 0);
	CHECK(wheel.feed(-70) == -1);
	CHECK(wheel.feed(100) == 0);      // +100 left over...
	CHECK(wheel.feed(-40) == 0);      // ...opposite deltas cancel inside one notch
	CHECK(wheel.feed(-179) == 0);     // 60 - 179 = -119: still short of a notch
	CHECK(wheel.feed(-1) == -1);
	CHECK(wheel.feed(359) == 2);      // 359 - 240 = 119 carried
	CHECK(wheel.feed(1) == 1);
	return true;
}

int main() {
	int failed = 0;
	for (const auto test : {test_order_and_held_rows, test_capture_and_overlay_edges,
			test_missing_simulation_freezes_only_scope_zero, test_reset_preserves_switch_hud_medic_and_use_state,
			test_use_previous_frame_and_single_seat, test_use_release_digit_priority_and_no_sim_consumption,
			test_shell_consume_and_overlay_cancel, test_use_swallows_only_digit_event_rows,
			test_live_binding_modifier_remap_and_use_stream,
			test_nvg_gain_needs_ctrl_and_shift_reverses_waypoint, test_wheel_subset_and_repeated_events,
			test_wheel_remainder_dispatches_whole_notches})
		if (!test()) ++failed;
	std::cout << "player_actions: " << failed << " failed\n";
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
