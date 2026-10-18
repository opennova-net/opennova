// Synthetic device streams through the public action poll, plus the real
// binding sampler for modifier claims and remapping. No production-source pins.
#include <runtime/controls/binding_set.h>
#include <runtime/controls/player_actions.h>
#include <runtime/controls/key_state_reset.h>

#include <array>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace opennova::controls;
using Action = PlayerAction;

namespace {

#define CHECK(condition) do { \
	if (!(condition)) { \
		std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; \
		return false; \
	} \
} while (false)

const PlayerActionPoll kLive{true, true, true};

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

// An open text line takes the digits: a (mouse-bound) USE hold selects no
// seat while the keyboard is captured, so its release still mounts
// [orig: Input_HandleSpecialKeys @0x49c5c0 runs only with g_InputCaptureMode
// clear].
bool test_captured_keyboard_keeps_the_use_digits() {
	const PlayerActionPoll kTyping{true, true, true, true};
	PlayerActions actions;
	Keys keys;
	keys.hold("useitem");
	CHECK(actions.poll(keys, kTyping).requests.empty());
	keys.digits[3] = true;
	CHECK(actions.poll(keys, kTyping).requests.empty());
	keys.release("useitem");
	CHECK(requests_are(actions.poll(keys, kTyping), {{Action::ToggleMount}}));
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
	// One key event fires one row: Accessory (code 207) precedes radarout
	// (361) in the scan's walk, so the bare 7 takes the weapon alone
	// [orig: Input_ProcessKeyboardEvents @0x49d488 / @0x49d490].
	keys.down = {'7'};
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::WeaponCategory, 7}}));
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
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::WeaponCategory, 7}}));
	keys.down = {0xBD}; // radarout's own '-' still zooms
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::RadarZoom, 1}}));
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

// The Respawn row (catalog 68): Ctrl+R's modifier pass claims R, so the key
// edges the respawn and not the reload; a held key edges once; an inactive
// gameplay frame or the death screen (an alive-mode row) admits no edge, and
// the latch follows the keys behind them. [orig: Input_HandleActionBinding
// case 55 @0x49B20B; the row's code @0x817658]
bool test_respawn_edges_on_ctrl_r() {
	PlayerActions actions;
	BoundKeys keys;
	keys.down = {17, 'R'};
	auto frame = actions.poll(keys, kLive);
	CHECK(frame.respawn_edge && !frame.reload_edge);
	CHECK(!actions.poll(keys, kLive).respawn_edge);
	keys.down.clear(); actions.poll(keys, kLive);
	keys.down = {'R'};
	frame = actions.poll(keys, kLive);
	CHECK(!frame.respawn_edge && frame.reload_edge);
	keys.down.clear(); actions.poll(keys, kLive);
	keys.down = {17, 'R'};
	CHECK(!actions.poll(keys, {false, true, true}).respawn_edge);
	CHECK(!actions.poll(keys, kLive).respawn_edge); // the latch followed the hold
	keys.down.clear(); actions.poll(keys, kLive);
	PlayerActionPoll dead = kLive;
	dead.death_screen = true;
	keys.down = {17, 'R'};
	CHECK(!actions.poll(keys, dead).respawn_edge);
	keys.down.clear(); actions.poll(keys, kLive);
	keys.down = {17, 'R'};
	CHECK(actions.poll(keys, kLive).respawn_edge);
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

// The death screen's spectator rows: press edges while active, in catalog
// order, carrying the dispatch codes 500..502; an inactive frame advances the
// latch without firing [orig: rows 110..112 -> Input_HandleActionBinding
// cases 500..502 @0x49bd58..0x49bd9e].
bool test_spectator_rows_edge_to_their_codes() {
	PlayerActions actions;
	Keys keys;
	keys.hold("CycleSpectatorMode");
	keys.hold("DecSpectatorTarget");
	auto requests = actions.poll_spectator(keys, true);
	CHECK(requests.size() == 2 && requests[0].action == Action::Spectate &&
			requests[0].value == 500 && requests[1].value == 502);
	CHECK(actions.poll_spectator(keys, true).empty()); // held: no new edge
	keys.release("CycleSpectatorMode");
	keys.release("DecSpectatorTarget");
	keys.hold("IncSpectatorTarget");
	CHECK(actions.poll_spectator(keys, false).empty()); // inactive: latched, silent
	CHECK(actions.poll_spectator(keys, true).empty());  // still held since the latch
	keys.release("IncSpectatorTarget");
	CHECK(actions.poll_spectator(keys, true).empty());
	keys.hold("IncSpectatorTarget");
	requests = actions.poll_spectator(keys, true);
	CHECK(requests.size() == 1 && requests[0].value == 501);
	return true;
}

// While the Emotes or Radio menu is open a digit is the menu's pick (keys
// 1..9, and 0 as 10) and never reaches a digit-bound row, the Emotes menu
// first; the held-USE seat arm precedes both; inactive gameplay input and an
// open text line pick nothing.
// [orig: Input_HandleSpecialKeys @0x49c6d8 (seat), @0x49c731..0x49c77c
//  (emotes), @0x49c783..0x49c7c8 (radio); the consumed key skips the binding
//  scan, Input_ProcessKeyboardEvents @0x49d2fb]
bool test_voice_menu_digits_pick_and_swallow_the_rows() {
	PlayerActionPoll emotes = kLive;
	emotes.emotes_menu_open = true;
	PlayerActionPoll radio = kLive;
	radio.radio_menu_open = true;
	PlayerActionPoll both = emotes;
	both.radio_menu_open = true;
	PlayerActions actions;
	Keys keys;
	keys.hold("Knife", '1');
	keys.digits[1] = true;
	CHECK(requests_are(actions.poll(keys, emotes), {{Action::EmotePick, 1}}));
	// The embedder closed the menu; the held digit fires no row afterwards.
	CHECK(actions.poll(keys, kLive).requests.empty());
	keys.release("Knife");
	keys.digits[1] = false;
	actions.poll(keys, kLive);
	keys.digits[0] = true;
	CHECK(requests_are(actions.poll(keys, radio), {{Action::RadioPick, 10}}));
	keys.digits[0] = false;
	actions.poll(keys, kLive);
	keys.digits[5] = true;
	CHECK(requests_are(actions.poll(keys, both), {{Action::EmotePick, 5}}));
	keys.digits[5] = false;
	actions.poll(keys, kLive);
	// Inactive gameplay input: no pick, and the latched key cannot fire later.
	PlayerActionPoll idle = emotes;
	idle.active = false;
	keys.digits[2] = true;
	CHECK(actions.poll(keys, idle).requests.empty());
	CHECK(actions.poll(keys, emotes).requests.empty());
	keys.digits[2] = false;
	actions.poll(keys, kLive);
	// An open text line owns the digits.
	PlayerActionPoll typing = emotes;
	typing.keyboard_captured = true;
	keys.digits[4] = true;
	CHECK(actions.poll(keys, typing).requests.empty());
	keys.digits[4] = false;
	actions.poll(keys, kLive);
	// The held-USE seat arm comes first.
	keys.hold("useitem");
	actions.poll(keys, emotes);
	keys.digits[3] = true;
	CHECK(requests_are(actions.poll(keys, emotes), {{Action::SelectSeat, 2}}));
	return true;
}

// ToSpecial dispatches on both edges of its keys, press and release, with
// their live state; an edge without captured gameplay input or a simulation
// is not dispatched, and the latch follows the keys meanwhile.
// [orig: Input_ProcessKeyboardEvents @0x49D249..0x49D2B9 (the release pass) and
//  @0x49D42F (the press); ToSpecial's row flags 0x8C000801]
bool test_to_special_dispatches_both_edges() {
	PlayerActions actions;
	Keys keys;
	PlayerActionFrame frame = actions.poll(keys, kLive);
	CHECK(!frame.to_special_held && !frame.to_special_edge);
	keys.hold("ToSpecial", 0x46);
	frame = actions.poll(keys, kLive);
	CHECK(frame.to_special_held && frame.to_special_edge);
	frame = actions.poll(keys, kLive);
	CHECK(frame.to_special_held && !frame.to_special_edge);
	keys.release("ToSpecial");
	frame = actions.poll(keys, kLive);
	CHECK(!frame.to_special_held && frame.to_special_edge);
	// Uncaptured: the press latches, no dispatch; nor its release.
	PlayerActionPoll uncaptured = kLive;
	uncaptured.captured = false;
	keys.hold("ToSpecial", 0x46);
	frame = actions.poll(keys, uncaptured);
	CHECK(frame.to_special_held && !frame.to_special_edge);
	frame = actions.poll(keys, kLive);
	CHECK(!frame.to_special_edge);
	keys.release("ToSpecial");
	PlayerActionPoll no_sim = kLive;
	no_sim.simulation_available = false;
	frame = actions.poll(keys, no_sim);
	CHECK(!frame.to_special_held && !frame.to_special_edge);
	return true;
}

// The held USE row runs its arm on every frame a key is down, through the
// embedder's press latch, and only the mount chain holds the chain's latch:
// walking into an armory with USE held opens it, and the chain the earlier
// frames held ends there as a release does (the open first, then the
// toggle); a latched press takes the arm with nothing to open, and its
// release toggles nothing. The death screen admits no USE row (its mode word
// carries bit 1 alone).
// [orig: Input_ProcessAnalogBindings @0x4995F0 -- the keys @0x499635, the
//  mode word @0x49963E..0x499650; Input_HandleActionBinding_0 case 0xB1
//  @0x4E0A82, the mount chain @0x4E0B5E..0x4E0B71; Input_ProcessFrame's
//  release edge @0x49D6C1..0x49D6DC]
bool test_held_use_runs_the_arm_every_frame() {
	using Arm = UseItemPressArm;
	std::vector<Arm> arms;
	std::size_t asked = 0;
	PlayerActionPoll gate = kLive;
	gate.take_use_item_arm = [&]() { return asked < arms.size() ? arms[asked++] : Arm::kTaken; };
	PlayerActions actions;
	Keys keys;
	arms = {Arm::kMountChain, Arm::kMountChain, Arm::kOpenArmory, Arm::kTaken};
	keys.hold("useitem");
	CHECK(actions.poll(keys, gate).requests.empty());
	CHECK(actions.poll(keys, gate).requests.empty());
	CHECK(requests_are(actions.poll(keys, gate), {{Action::OpenArmory}, {Action::ToggleMount}}));
	CHECK(actions.poll(keys, gate).requests.empty());
	CHECK(asked == 4);
	keys.release("useitem");
	CHECK(actions.poll(keys, gate).requests.empty());
	CHECK(asked == 4);

	PlayerActions fresh;
	arms = {Arm::kOpenArmory};
	asked = 0;
	keys.hold("useitem");
	CHECK(requests_are(fresh.poll(keys, gate), {{Action::OpenArmory}}));
	keys.release("useitem");
	CHECK(fresh.poll(keys, gate).requests.empty());

	PlayerActions dead;
	PlayerActionPoll dead_gate = gate;
	dead_gate.death_screen = true;
	asked = 0;
	keys.hold("useitem");
	CHECK(dead.poll(keys, dead_gate).requests.empty());
	CHECK(asked == 0);
	keys.release("useitem");
	CHECK(dead.poll(keys, dead_gate).requests.empty());

	return true;
}

// Every menu close clears the key state: a key held through it (Shift as the
// WEAPON screen's ACCEPT, a held W) reads up until its next key-down message,
// a press or an OS repeat, or until its release; a key up at the reset is not
// touched, and keys never held are not either.
// [orig: UI_CloseMenuScreen @0x54E63F / WeaponLoadout_ApplyFromBuffer
//  @0x565D01 -> Input_ResetAllState -> Input_ClearKeyState @0x497ED5;
//  Input_HandleKeyPress @0x760CBB]
bool test_key_state_reset() {
	KeyStateReset reset;
	const auto down_at_reset = [](int vk) { return vk == 0x10 || vk == 'W'; };
	CHECK(reset.held(0x10, true)); // no reset yet: the device state
	reset.reset(down_at_reset);
	CHECK(!reset.held(0x10, true));
	CHECK(!reset.held(0x10, true)); // still held, still up
	CHECK(!reset.held('W', true));
	CHECK(reset.held('A', true)); // up at the reset, pressed since
	reset.key_down(0x10); // an OS repeat
	CHECK(reset.held(0x10, true));
	CHECK(!reset.held('W', true));
	// A release ends it too: the next press reads down.
	CHECK(!reset.held('W', false));
	CHECK(reset.held('W', true));
	// The extended keys are in the array: keypad Enter, VK 269.
	reset.reset([](int vk) { return vk == 269; });
	CHECK(!reset.held(269, true));
	reset.key_down(269);
	CHECK(reset.held(269, true));
	// Out-of-range codes pass the device state through.
	CHECK(reset.held(0, true));
	CHECK(reset.held(KeyStateReset::kKeyCount, true));
	return true;
}

// The reset clears the key-press latch: ToSpecial held through a close
// dispatches no release, not when its key reads up after the reset nor at its
// physical release; a fresh key-down presses it again.
// [orig: Input_ResetAllState @0x497F08; Input_ProcessKeyboardEvents
//  @0x49D249 / @0x49D2B9 / @0x49D2ED]
bool test_reset_drops_the_held_release() {
	Keys keys;
	PlayerActionPoll gate;
	gate.active = true;
	gate.captured = true;
	gate.simulation_available = true;
	PlayerActions actions;
	keys.hold("ToSpecial", 'F');
	CHECK(actions.poll(keys, gate).to_special_edge); // the press
	CHECK(!actions.poll(keys, gate).to_special_edge);
	actions.clear_key_press_latches();
	keys.release("ToSpecial"); // the key reads up after the reset
	CHECK(!actions.poll(keys, gate).to_special_edge);
	CHECK(!actions.poll(keys, gate).to_special_edge);
	keys.hold("ToSpecial", 'F'); // its next key-down
	CHECK(actions.poll(keys, gate).to_special_edge);
	keys.release("ToSpecial");
	CHECK(actions.poll(keys, gate).to_special_edge); // a latched release
	return true;
}

// No input frame runs under a menu screen: a USE hold latched on the mount
// chain when the menu opened is not aged under it, so the first frame after
// the close, its key read up by the reset, ends the hold and toggles the
// mount. A seat pick made in the hold consumes it as before.
// [orig: Game_ProcessMainFrame @0x52660D / @0x52661D; Input_ProcessFrame
//  @0x49D57F..0x49D585, @0x49D6C1..0x49D6DC]
bool test_menu_freezes_the_use_hold() {
	Keys keys;
	PlayerActionPoll gate;
	gate.active = true;
	gate.captured = true;
	gate.simulation_available = true;
	PlayerActions actions;
	keys.hold("useitem", 0x10);
	CHECK(actions.poll(keys, gate).requests.empty()); // the chain holds
	PlayerActionPoll menu = gate;
	menu.active = false;
	menu.captured = false;
	menu.menu_open = true;
	CHECK(actions.poll(keys, menu).requests.empty());
	keys.release("useitem"); // read up after the close's reset
	CHECK(actions.poll(keys, menu).requests.empty());
	CHECK(requests_are(actions.poll(keys, gate), {{Action::ToggleMount}}));
	CHECK(actions.poll(keys, gate).requests.empty());
	// An inactive frame that is no menu (the input frame runs) still cancels it.
	PlayerActions overlay;
	keys.hold("useitem", 0x10);
	overlay.poll(keys, gate);
	PlayerActionPoll inactive = gate;
	inactive.active = false;
	overlay.poll(keys, inactive);
	keys.release("useitem");
	CHECK(overlay.poll(keys, gate).requests.empty());
	return true;
}

int main() {
	int failed = 0;
	for (const auto test : {test_order_and_held_rows, test_capture_and_overlay_edges,
			test_missing_simulation_freezes_only_scope_zero, test_reset_preserves_switch_hud_medic_and_use_state,
			test_use_previous_frame_and_single_seat, test_captured_keyboard_keeps_the_use_digits,
			test_use_release_digit_priority_and_no_sim_consumption,
			test_shell_consume_and_overlay_cancel, test_use_swallows_only_digit_event_rows,
			test_live_binding_modifier_remap_and_use_stream,
			test_nvg_gain_needs_ctrl_and_shift_reverses_waypoint, test_wheel_subset_and_repeated_events,
			test_wheel_remainder_dispatches_whole_notches,
			test_spectator_rows_edge_to_their_codes, test_key_state_reset, test_reset_drops_the_held_release, test_menu_freezes_the_use_hold,
			test_voice_menu_digits_pick_and_swallow_the_rows,
			test_to_special_dispatches_both_edges, test_respawn_edges_on_ctrl_r,
			test_held_use_runs_the_arm_every_frame})
		if (!test()) ++failed;
	std::cout << "player_actions: " << failed << " failed\n";
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
