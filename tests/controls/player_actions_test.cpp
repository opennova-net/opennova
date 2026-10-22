// Synthetic device streams through the public action poll, plus the real
// binding sampler for modifier claims and remapping. No production-source pins.
#include <runtime/controls/binding_set.h>
#include <runtime/controls/player_actions.h>
#include <runtime/controls/key_state_reset.h>

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
	void hold(const char *token, int vk = 0) { held[token] = vk; }
	void release(const char *token) { held.erase(token); }
	bool pressed(const char *token) const override { return held.count(token) != 0; }
	int pressed_key(const char *token) const override {
		const auto it = held.find(token);
		return it == held.end() ? 0 : it->second;
	}
	bool shift = false;
	bool fire_signed(const char *) const override { return shift; }
	// The key state alone, for a held key the scan or a text line hides.
	std::set<std::string> raw;
	bool keys_down(const char *token) const override {
		return raw.count(token) != 0 || pressed(token);
	}
};

// The Respawn, MedicReq, reload and ToSpecial rows' records, whose arms the role owns
// (inmatch::Role::dispatch_record); requests_are leaves them out, medic() / respawn() /
// reload() / to_special() count them.
bool role_record(const PlayerActionRequest &request) {
	return request.action == Action::Respawn || request.action == Action::MedicRequest ||
			request.action == Action::Reload || request.action == Action::ToSpecial;
}

int count_of(const PlayerActionFrame &frame, Action action) {
	int count = 0;
	for (const PlayerActionRequest &request : frame.requests) count += request.action == action;
	return count;
}

int medic(const PlayerActionFrame &frame) { return count_of(frame, Action::MedicRequest); }
int respawn(const PlayerActionFrame &frame) { return count_of(frame, Action::Respawn); }
int reload(const PlayerActionFrame &frame) { return count_of(frame, Action::Reload); }
int to_special(const PlayerActionFrame &frame) { return count_of(frame, Action::ToSpecial); }

bool requests_are(const PlayerActionFrame &frame, const std::vector<PlayerActionRequest> &expected) {
	std::vector<PlayerActionRequest> rows;
	for (const PlayerActionRequest &request : frame.requests)
		if (!role_record(request)) rows.push_back(request);
	if (rows.size() != expected.size()) return false;
	for (std::size_t i = 0; i < expected.size(); ++i)
		if (rows[i].action != expected[i].action || rows[i].value != expected[i].value)
			return false;
	return true;
}

// The special-key chain's take of a key-down names this pick (code -1: past no head).
bool picks(const ChainDigitTake &take, Action action, int value) {
	return take.taken && take.pick && take.pick->action == action && take.pick->value == value &&
			take.pick->code == -1;
}

// A source whose keyboard keys reach the rows only through the key queue's events
// (PlayerActionPoll::key_events): its off-keyboard arms (a mouse button, the
// joystick, the scripted device) are the tokens in `off`.
struct QueuedKeys : Keys {
	std::set<std::string> off;
	bool held_off_keyboard(const char *token) const override { return off.count(token) != 0; }
};

// The dispatch code of a catalog token's row [orig: word_8159A8 + 108 * row].
int code_of(const char *token) {
	std::size_t count = 0;
	const ActionDef *rows = catalog(&count);
	for (std::size_t i = 0; i < count; ++i)
		if (std::string(rows[i].token) == token) return action_code(rows[i].id);
	return -1;
}

// The keyboard pass's event for a token's row: its scanned key-down.
KeyRowEvent key_row(const char *token, int key, bool sign = false) {
	return KeyRowEvent{code_of(token), sign, key, false};
}

// Each queued key event fires its row once, in the queue's order, an OS repeat
// again; the keyboard key's hold fires nothing more. An off-keyboard arm keeps
// its press edge, after the events. A row's gates hold for its event (a
// captured row's under a free cursor, every row's with no simulation), and the
// sign rides the event (NextWaypoint back).
// [orig: Input_ProcessKeyboardEvents @0x49d1f0 - the press scan's queue
//  @0x49d42f / @0x49d488 per dequeued event; Input_QueueEvent @0x4979C0 (no
//  merge); Input_ProcessFrame @0x49d520 - the drain's dispatch @0x49d5d8;
//  Input_ProcessPlayerFrame @0x49d4c0 - the keyboard pass @0x49d4c6 ahead of the
//  mouse and toggle passes @0x49d4d8 / @0x49d4de]
bool test_event_rows_fire_once_per_event() {
	PlayerActions actions;
	QueuedKeys keys;
	keys.hold("Knife", '1');
	PlayerActionPoll gate = kLive;
	gate.key_events = {key_row("Secondary", '2'), key_row("Knife", '1'), key_row("Knife", '1')};
	CHECK(requests_are(actions.poll(keys, gate), {{Action::WeaponCategory, 2},
			{Action::WeaponCategory, 1}, {Action::WeaponCategory, 1}}));
	CHECK(actions.poll(keys, kLive).requests.empty()); // the held key fires nothing
	keys.off.insert("binoculars");
	gate.key_events = {key_row("NVG", 'N')};
	CHECK(requests_are(actions.poll(keys, gate), {{Action::NightVision}, {Action::Binoculars}}));
	CHECK(actions.poll(keys, kLive).requests.empty()); // the arm held: no new edge
	keys.off.clear();
	gate.key_events = {key_row("NextWaypoint", 0x76, true), key_row("NextWaypoint", 0x76)};
	CHECK(requests_are(actions.poll(keys, gate),
			{{Action::WaypointCycle, -1}, {Action::WaypointCycle, 1}}));
	PlayerActionPoll uncaptured = kLive;
	uncaptured.captured = false;
	uncaptured.key_events = {key_row("Knife", '1'), key_row("NVG", 'N')};
	CHECK(requests_are(actions.poll(keys, uncaptured), {{Action::NightVision}}));
	PlayerActionPoll no_sim = kLive;
	no_sim.simulation_available = false;
	no_sim.key_events = {key_row("NVG", 'N')};
	CHECK(actions.poll(keys, no_sim).requests.empty());
	PlayerActionPoll menu = kLive;
	menu.menu_open = true;
	menu.key_events = {key_row("NVG", 'N')};
	CHECK(actions.poll(keys, menu).requests.empty());
	// Each request's event passes the dispatcher's head by its row's code.
	gate.key_events = {key_row("Prone", 'Z')};
	const auto frame = actions.poll(keys, gate);
	CHECK(frame.requests.size() == 1 && frame.requests[0].code == code_of("Prone"));
	return true;
}

// The special-key chain's digit arms, one key-down at a time, the first whose word holds
// deciding: under the USE hold (the hold the last poll latched) a digit picks its seat
// (keys 1..9 seats 0..8, key 0 seat 9) once per hold and is taken thereafter, and a
// non-digit passes on to the tail, the menus' arms skipped; under an open Emotes / Radio
// menu, the Emotes menu first, a digit picks its entry (0 as 10); with no arm holding no
// key is taken. A taken key queues no event, so the poll's events are the keys the chain
// left, a digit's included.
// [orig: Input_HandleSpecialKeys @0x49c5c0 - the held-USE arm @0x49c6d8 (the
//  (key - 48) <= 9 test @0x49c6e0, the seat index @0x49c6e6..0x49c6ed, the consumed test
//  @0x49c6f8 and store @0x49c70c), the Emotes arm's test @0x49c73f and 0 as 10 @0x49c74a,
//  the Radio arm's @0x49c78b / @0x49c796; Input_ProcessKeyboardEvents - a take's skip of
//  the scan @0x49d305; Input_ProcessFrame - the release edge's consumed test @0x49d6d4]
bool test_the_chain_takes_digit_keys() {
	PlayerActions actions;
	QueuedKeys keys;
	CHECK(!actions.chain_digit_key('1', false, false).taken); // no arm holds
	keys.hold("useitem", 0x10);
	actions.poll(keys, kLive); // the hold latches for the next frame's chain
	CHECK(!actions.chain_digit_key('N', false, false).taken);
	CHECK(!actions.chain_digit_key('N', true, false).taken); // the menu's arm never runs
	CHECK(picks(actions.chain_digit_key('0', true, false), Action::SelectSeat, 9));
	const ChainDigitTake again = actions.chain_digit_key('1', false, false);
	CHECK(again.taken && !again.pick); // one seat per hold
	// The keys the chain left reach their rows, a digit's among them.
	PlayerActionPoll gate = kLive;
	gate.key_events = {key_row("radarout", 0xBD), key_row("MedicReq", '9')};
	auto frame = actions.poll(keys, gate);
	CHECK(requests_are(frame, {{Action::RadarZoom, 1}}) && medic(frame) == 1);
	keys.release("useitem");
	CHECK(actions.poll(keys, kLive).requests.empty()); // the consumed hold toggles nothing
	CHECK(picks(actions.chain_digit_key('5', true, true), Action::EmotePick, 5));
	CHECK(picks(actions.chain_digit_key('0', false, true), Action::RadioPick, 10));
	CHECK(!actions.chain_digit_key(':', false, true).taken);
	CHECK(!actions.chain_digit_key('/', true, false).taken);
	return true;
}

// Reload, MedicReq and Respawn fire on their key events under their gates
// (reload captured, respawn with gameplay input live, medic always), their held
// keyboard keys firing nothing more; their off-keyboard arms keep their edges.
// [orig: Input_HandleActionBinding_0 @0x4E0420 (case 0xD3, reload);
//  Input_HandleActionBinding @0x49AD40 (case 217 @0x49b4b4 MedicReq, case 55 @0x49B20B
//  respawn); the press scan's queue @0x49d42f / @0x49d488]
bool test_reload_medic_and_respawn_fire_on_their_events() {
	PlayerActions actions;
	QueuedKeys keys;
	keys.hold("magazine", 'R');
	PlayerActionPoll gate = kLive;
	gate.key_events = {key_row("magazine", 'R'), key_row("MedicReq", '9'),
			key_row("respawn", 'R')};
	auto frame = actions.poll(keys, gate);
	CHECK(reload(frame) && medic(frame) == 1 && respawn(frame) == 1);
	CHECK(frame.requests.size() == 3 && frame.requests[0].action == Action::Reload &&
			frame.requests[0].code == 211 && frame.requests[1].action == Action::MedicRequest &&
			frame.requests[1].code == 217 && frame.requests[2].action == Action::Respawn &&
			frame.requests[2].code == 55);
	frame = actions.poll(keys, kLive);
	CHECK(!reload(frame) && !medic(frame) && !respawn(frame));
	PlayerActionPoll inactive = kLive;
	inactive.active = false;
	inactive.key_events = gate.key_events;
	frame = actions.poll(keys, inactive);
	CHECK(!reload(frame) && medic(frame) && !respawn(frame));
	keys.off.insert("magazine");
	frame = actions.poll(keys, kLive);
	CHECK(reload(frame)); // the off-keyboard arm's press edge
	CHECK(!reload(actions.poll(keys, kLive)));
	// The records keep the key order across the action rows and the role's rows: Knife,
	// Respawn, Primary queue as 201, 55, 203; the medic call's off-keyboard arm queues behind
	// the keyboard pass's events.
	keys.off = {"MedicReq"};
	PlayerActionPoll ordered = kLive;
	ordered.key_events = {key_row("Knife", '1'), key_row("respawn", 'R'), key_row("Primary", '3')};
	frame = actions.poll(keys, ordered);
	CHECK(frame.requests.size() == 4 && frame.requests[0].code == code_of("Knife") &&
			frame.requests[1].action == Action::Respawn && frame.requests[1].code == 55 &&
			frame.requests[2].code == code_of("Primary") &&
			frame.requests[3].action == Action::MedicRequest && frame.requests[3].code == 217);
	return true;
}

// The talk, HUD and pause rows' key events are the embedder's records in key order with
// the action rows' (D-CTRL-6, LI-s15): huddetail, Knife, pause, Primary queue as 19, 201,
// 25, 203, so a pause key ahead of a weapon row flips the word the weapon row's head then
// reads. The table's gates are the action rows' alone: with gameplay input down the
// embedder's rows still queue.
// [orig: Input_ProcessKeyboardEvents @0x49D1F0 (Input_QueueEvent per scanned key-down
//  @0x49D42F / @0x49D488); Input_ProcessFrame @0x49D520 (the drain @0x49D599..0x49D5E7);
//  Input_HandleActionBinding @0x49AD40 (case 25 @0x49B520)]
bool test_embedder_rows_keep_the_key_order() {
	PlayerActions actions;
	QueuedKeys keys;
	const int huddetail = code_of("huddetail");
	const int pause = code_of("pause");
	CHECK(huddetail == 19 && pause == 25);
	PlayerActionPoll gate = kLive;
	gate.embedder_row = [&](int code) { return code == huddetail || code == pause; };
	gate.key_events = {key_row("huddetail", 0x75), key_row("Knife", '1'), key_row("pause", 0x13),
			key_row("Primary", '3')};
	auto frame = actions.poll(keys, gate);
	CHECK(frame.requests.size() == 4 && frame.requests[0].action == Action::EmbedderRow &&
			frame.requests[0].code == 19 && frame.requests[1].code == code_of("Knife") &&
			frame.requests[2].action == Action::EmbedderRow && frame.requests[2].code == 25 &&
			frame.requests[3].code == code_of("Primary"));
	PlayerActionPoll inactive = gate;
	inactive.active = false;
	inactive.captured = false;
	frame = actions.poll(keys, inactive);
	CHECK(frame.requests.size() == 2 && frame.requests[0].code == 19 &&
			frame.requests[1].code == 25);
	return true;
}

// ToSpecial's press dispatches on its key-down's event (an OS repeat again), with
// captured gameplay input; its release on the release pass's event, which has no
// capture or mode gate (over a screen too); neither with no simulation. The
// handler reads the keys' state to tell the two apart.
// [orig: Input_ProcessKeyboardEvents @0x49d1f0 - the press @0x49d42f, the release
//  pass's queue @0x49d2b1; Input_HandleActionBinding_0 case 0xDC
//  @0x4E1161..0x4E1181]
bool test_to_special_dispatches_its_key_events() {
	PlayerActions actions;
	QueuedKeys keys;
	const int code = code_of("ToSpecial");
	keys.raw.insert("ToSpecial");
	PlayerActionPoll gate = kLive;
	gate.key_events = {KeyRowEvent{code, false, 'F', false}};
	auto frame = actions.poll(keys, gate);
	CHECK(frame.to_special_held && to_special(frame));
	CHECK(!to_special(actions.poll(keys, kLive))); // the held key: no event
	CHECK(to_special(actions.poll(keys, gate)));   // an OS repeat's event
	PlayerActionPoll uncaptured = gate;
	uncaptured.captured = false;
	CHECK(!to_special(actions.poll(keys, uncaptured)));
	keys.raw.clear();
	PlayerActionPoll screen = kLive;
	screen.active = false;
	screen.captured = false;
	screen.key_events = {KeyRowEvent{code, false, 'F', true}};
	frame = actions.poll(keys, screen);
	CHECK(!frame.to_special_held && to_special(frame));
	PlayerActionPoll no_sim = screen;
	no_sim.simulation_available = false;
	CHECK(!to_special(actions.poll(keys, no_sim)));
	return true;
}

// The spectator rows' key events request their codes in the queue's order while
// active, and their held keys fire nothing more.
// [orig: rows 110..112 -> Input_HandleActionBinding cases 500..502
//  @0x49bd58..0x49bd9e; the press scan's queue @0x49d42f / @0x49d488]
bool test_spectator_rows_fire_on_their_events() {
	PlayerActions actions;
	QueuedKeys keys;
	PlayerActionPoll dead = kLive;
	dead.death_screen = true;
	dead.key_events = {key_row("DecSpectatorTarget", 0), key_row("CycleSpectatorMode", 0),
			key_row("DecSpectatorTarget", 0)};
	const auto requests = actions.poll(keys, dead).requests;
	CHECK(requests.size() == 3 && requests[0].action == Action::Spectate &&
			requests[0].value == 502 && requests[1].value == 500 && requests[2].value == 502);
	// Each is a record of the update's drain by its code, which maps back to its request
	// (LI-s15).
	CHECK(requests[0].code == 502 && requests[1].code == 500 && requests[2].code == 502);
	const auto back = player_action_request(InputEventRecord{501, 0, 0, 0, false,
			InputEventOrigin::Device});
	CHECK(back && back->action == Action::Spectate && back->value == 501 && back->code == 501);
	CHECK(actions.poll_spectator(keys, true).empty()); // no off-keyboard arm held
	PlayerActionPoll idle = dead;
	idle.active = false;
	CHECK(actions.poll(keys, idle).requests.empty());
	return true;
}

// The special-key chain's deploy pick (input event 12, its pick the event's first argument)
// and the escape row's event (18) are the embedder's records in key order with the action
// rows', the pick's argument on its record; on the death screen the spectator rows' events
// (500..502) are records in the same order, and off it (or with gameplay input down) they
// fire nothing.
// [orig: Input_HandleSpecialKeys @0x49c5c0 - Input_QueueEvent(12, key - 65 + 1, 0, 0)
//  @0x49ca73; Input_ProcessKeyboardEvents @0x49d1f0 - the scan's queue @0x49d42f / @0x49d488;
//  Input_HandleActionBinding @0x49AD40 - cases 500 @0x49bd58, 502 @0x49bd89]
bool test_picks_escape_and_spectator_rows_keep_the_key_order() {
	PlayerActions actions;
	QueuedKeys keys;
	PlayerActionPoll gate = kLive;
	gate.embedder_row = [](int code) { return code == 12 || code == 18; };
	const KeyRowEvent pick{12, false, 0, false, 3};
	gate.key_events = {key_row("Knife", '1'), pick, KeyRowEvent{18, false, 0x1B, false},
			key_row("Primary", '3')};
	auto frame = actions.poll(keys, gate);
	CHECK(frame.requests.size() == 4 && frame.requests[0].code == code_of("Knife") &&
			frame.requests[1].action == Action::EmbedderRow && frame.requests[1].code == 12 &&
			frame.requests[1].value == 3 && frame.requests[2].action == Action::EmbedderRow &&
			frame.requests[2].code == 18 && frame.requests[2].value == 0 &&
			frame.requests[3].code == code_of("Primary"));
	PlayerActionPoll dead = gate;
	dead.death_screen = true;
	dead.key_events = {key_row("DecSpectatorTarget", 0), pick, key_row("CycleSpectatorMode", 0)};
	frame = actions.poll(keys, dead);
	CHECK(frame.requests.size() == 3 && frame.requests[0].action == Action::Spectate &&
			frame.requests[0].code == 502 && frame.requests[1].code == 12 &&
			frame.requests[2].action == Action::Spectate && frame.requests[2].value == 500);
	PlayerActionPoll alive = gate;
	alive.key_events = {key_row("CycleSpectatorMode", 0)};
	CHECK(actions.poll(keys, alive).requests.empty());
	PlayerActionPoll idle = dead;
	idle.active = false;
	idle.key_events = {key_row("CycleSpectatorMode", 0)};
	CHECK(actions.poll(keys, idle).requests.empty());
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
	CHECK(frame.fire_held && frame.fire_edge && reload(frame) && medic(frame));
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
	CHECK(frame.fire_held && !frame.fire_edge && !reload(frame) && !medic(frame));
	CHECK(frame.requests.empty());
	return true;
}

bool test_capture_and_overlay_edges() {
	PlayerActions actions;
	Keys keys;
	for (const char *token : {"attack_1", "magazine", "scope", "MedicReq", "Knife", "seat1",
			"cycleweaponN", "Crouch", "radarout", "map_toggle"}) keys.hold(token);
	auto frame = actions.poll(keys, {false, true, true});
	CHECK(!frame.fire_held && !frame.fire_edge && !reload(frame) && medic(frame) == 1);
	CHECK(requests_are(frame, {}));
	frame = actions.poll(keys, kLive);
	CHECK(frame.fire_held && frame.fire_edge && reload(frame) && !medic(frame));
	CHECK(requests_are(frame, {{Action::ToggleScope}})); // raw held rows do not re-fire
	frame = actions.poll(keys, {true, false, true});
	CHECK(!frame.fire_held && frame.requests.empty());
	frame = actions.poll(keys, kLive);
	CHECK(frame.fire_edge && reload(frame));
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
	CHECK(frame.fire_edge && reload(frame) && medic(frame) == 1 && requests_are(frame, {}));
	frame = actions.poll(keys, kLive);
	CHECK(!frame.fire_edge && !reload(frame) && !medic(frame));
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
	CHECK(frame.fire_edge && reload(frame) && !medic(frame));
	CHECK(requests_are(frame, {{Action::ToggleScope}, {Action::Stance, 0}, {Action::Stance, 1},
			{Action::Stance, 2}, {Action::ScopeZero, -1}, {Action::ScopeZero, 1}}));
	CHECK(picks(actions.chain_digit_key('8', false, false), Action::SelectSeat, 7));
	actions.reset();
	keys.held.clear(); // the consumed hold survives the reset: its release toggles nothing
	CHECK(actions.poll(keys, kLive).requests.empty());
	return true;
}

// The chain reads the hold the LAST poll latched: a digit in the USE press's own frame is
// the scan's; one seat per hold; the digit's row held under the hold stays swallowed.
// [orig: Input_ProcessFrame @0x49d520 - the aging @0x49d57f ahead of the keyboard pass;
//  Input_HandleSpecialKeys @0x49c6d8..0x49c730]
bool test_use_previous_frame_and_single_seat() {
	PlayerActions actions;
	Keys keys;
	keys.hold("useitem"); keys.hold("Knife", '1');
	CHECK(!actions.chain_digit_key('1', false, false).taken); // USE was not held before the 1
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::WeaponCategory, 1}}));
	CHECK(actions.poll(keys, kLive).requests.empty());
	keys.release("Knife");
	actions.poll(keys, kLive);
	keys.hold("radarout", '9');
	CHECK(picks(actions.chain_digit_key('9', false, false), Action::SelectSeat, 8));
	CHECK(actions.poll(keys, kLive).requests.empty()); // the digit's row is swallowed
	const ChainDigitTake again = actions.chain_digit_key('0', false, false);
	CHECK(again.taken && !again.pick); // one seat per hold
	keys.release("useitem");
	CHECK(actions.poll(keys, kLive).requests.empty()); // consumed release never mounts
	CHECK(actions.poll(keys, kLive).requests.empty()); // swallowed held row cannot re-fire
	keys.release("radarout");
	actions.poll(keys, kLive);
	keys.hold("radarout", '9');
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::RadarZoom, 1}}));
	return true;
}

// The release frame's keyboard pass still meets the hold the last frame latched: its digit
// picks the seat, a second digit is taken with no pick, the digit rows held then stay
// swallowed, and the release edge toggles no mount. The pick consumes the hold whether or
// not the embedder has a simulation to run it on, so a later release never mounts.
// [orig: Input_ProcessFrame @0x49d520 - the aging @0x49d57f..0x49d585 ahead of the keyboard
//  pass, the release edge @0x49d6c1..0x49d6dc; Input_HandleSpecialKeys @0x49c6f8..0x49c70c]
bool test_use_release_digit_priority_and_no_sim_consumption() {
	PlayerActions actions;
	Keys keys;
	keys.hold("useitem");
	actions.poll(keys, kLive);
	keys.release("useitem");
	keys.hold("Knife", '1'); keys.hold("seat10", '0'); keys.hold("scope");
	CHECK(picks(actions.chain_digit_key('0', false, false), Action::SelectSeat, 9));
	const ChainDigitTake second = actions.chain_digit_key('1', false, false);
	CHECK(second.taken && !second.pick);
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::ToggleScope}}));
	PlayerActions no_sim;
	keys = Keys{}; keys.hold("useitem");
	no_sim.poll(keys, kLive);
	CHECK(no_sim.chain_digit_key('4', false, false).taken);
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
	// An inactive frame runs the input frame too: the held USE's digit picks a
	// seat (the hold is consumed), and the arm is not asked.
	CHECK(picks(actions.chain_digit_key('2', false, false), Action::SelectSeat, 1));
	CHECK(actions.poll(keys, {false, false, true}).requests.empty());
	CHECK(actions.poll(keys, kLive).requests.empty()); // a fresh hold
	keys.release("useitem");
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::ToggleMount}})); // fresh hold
	keys.hold("useitem"); actions.poll(keys, kLive);
	keys.release("useitem");
	// The release edge runs on an inactive frame as on any input frame.
	CHECK(requests_are(actions.poll(keys, {false, false, true}), {{Action::ToggleMount}}));
	CHECK(actions.poll(keys, kLive).requests.empty());
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
		CHECK(frame.fire_edge && reload(frame) && medic(frame));
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
	bool fire_signed(const char *token) const override {
		return bindings.fire_signed(bindings.index_of_token(token), 0,
				[this](int vk) { return down.count(vk) != 0; });
	}
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
	CHECK(picks(actions.chain_digit_key('7', false, false), Action::SelectSeat, 6));
	CHECK(actions.poll(keys, kLive).requests.empty()); // the 7's rows are swallowed
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
	// Rebound to Ctrl+F7 the row fires through the modifier pass, unsigned:
	// Shift held still cycles forward [orig: @0x49d41e..0x49d42f].
	const int waypoint = keys.bindings.index_of_token("NextWaypoint");
	CHECK(keys.bindings.assign_key(waypoint, 0x76, true, false, false, false));
	keys.down = {16, 17, 0x76};
	CHECK(requests_are(actions.poll(keys, kLive), {{Action::WaypointCycle, 1}}));
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
	CHECK(respawn(frame) && !reload(frame));
	CHECK(!respawn(actions.poll(keys, kLive)));
	keys.down.clear(); actions.poll(keys, kLive);
	keys.down = {'R'};
	frame = actions.poll(keys, kLive);
	CHECK(!respawn(frame) && reload(frame));
	keys.down.clear(); actions.poll(keys, kLive);
	keys.down = {17, 'R'};
	CHECK(!respawn(actions.poll(keys, {false, true, true})));
	CHECK(!respawn(actions.poll(keys, kLive))); // the latch followed the hold
	keys.down.clear(); actions.poll(keys, kLive);
	PlayerActionPoll dead = kLive;
	dead.death_screen = true;
	keys.down = {17, 'R'};
	CHECK(!respawn(actions.poll(keys, dead)));
	keys.down.clear(); actions.poll(keys, kLive);
	keys.down = {17, 'R'};
	CHECK(respawn(actions.poll(keys, kLive)));
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

// A binding row's key or wheel event names its row's dispatch code, which the dispatcher's
// head gates by that row's flags (V-L2-s8, D-NET-710): seat1 (row 17) 182, scope (row 105) 6,
// cycleweaponP (row 39) 212; the held-USE digit's seat pick and the USE release's mount toggle
// call their handlers directly, past no head.
// [orig: word_8159A8 + 108 * row (the catalog's codes); Input_HandleSpecialKeys @0x49c5c0 -
//  the seat index @0x49c6e6..0x49c6ed, Entity_FindAvailableSeat (the call @0x49c702);
//  Input_ProcessFrame @0x49d520 - the release
//  edge @0x49d6c1..0x49d6dc]
bool test_key_rows_carry_their_dispatch_codes() {
	{
		PlayerActions actions;
		Keys keys;
		keys.hold("seat1", '1');
		const PlayerActionFrame frame = actions.poll(keys, kLive);
		CHECK(requests_are(frame, {{Action::SelectSeat, 0}}) && frame.requests[0].code == 182);
	}
	{
		PlayerActions actions;
		Keys keys;
		keys.hold("scope");
		const PlayerActionFrame frame = actions.poll(keys, kLive);
		CHECK(requests_are(frame, {{Action::ToggleScope}}) && frame.requests[0].code == 6);
	}
	const auto wheel = player_wheel_action("cycleweaponP");
	CHECK(wheel && wheel->code == 212);
	{
		PlayerActions actions;
		Keys keys;
		keys.hold("useitem");
		actions.poll(keys, kLive);
		CHECK(picks(actions.chain_digit_key('1', false, false), Action::SelectSeat, 0));
	}
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

// While the Emotes or Radio menu is open a digit's key-down is the menu's pick (the chain)
// and a digit row's off-keyboard arm fires nothing either; the embedder's send closes the
// menu, so the next digit is the scan's again. The held-USE seat arm precedes the menus.
// [orig: Input_HandleSpecialKeys @0x49c6d8 (seat), @0x49c731..0x49c77c (emotes),
//  @0x49c783..0x49c7c8 (radio); the (key - 48) <= 9 tests @0x49c6e0 / @0x49c73f /
//  @0x49c78b; a take's skip of the scan, Input_ProcessKeyboardEvents @0x49d305]
bool test_voice_menu_digits_pick_and_swallow_the_rows() {
	PlayerActionPoll emotes = kLive;
	emotes.emotes_menu_open = true;
	PlayerActions actions;
	Keys keys;
	keys.hold("Knife", '1');
	CHECK(picks(actions.chain_digit_key('1', true, false), Action::EmotePick, 1));
	CHECK(actions.poll(keys, emotes).requests.empty()); // the digit's row is swallowed
	// The embedder closed the menu; the held digit fires no row afterwards.
	CHECK(actions.poll(keys, kLive).requests.empty());
	keys.release("Knife");
	actions.poll(keys, kLive);
	CHECK(!actions.chain_digit_key('1', false, false).taken); // closed: the scan's key
	// The held-USE seat arm comes first.
	keys.hold("useitem");
	actions.poll(keys, emotes);
	CHECK(picks(actions.chain_digit_key('3', true, false), Action::SelectSeat, 2));
	return true;
}

// ToSpecial's off-keyboard arm (a mouse button, the joystick, the scripted
// device) dispatches on both edges, press and release, with the keys' live
// state, under the keyboard events' gates; an edge without captured gameplay
// input or a simulation is not dispatched, and the latch follows the arm
// meanwhile. (Keys' arms are all off-keyboard: it feeds no key events.)
// [orig: Input_ProcessKeyboardEvents @0x49D249..0x49D2B9 (the release pass) and
//  @0x49D42F (the press); ToSpecial's row flags 0x8C000801]
bool test_to_special_dispatches_both_edges() {
	PlayerActions actions;
	Keys keys;
	PlayerActionFrame frame = actions.poll(keys, kLive);
	CHECK(!frame.to_special_held && !to_special(frame));
	keys.hold("ToSpecial", 0x46);
	frame = actions.poll(keys, kLive);
	CHECK(frame.to_special_held && to_special(frame));
	frame = actions.poll(keys, kLive);
	CHECK(frame.to_special_held && !to_special(frame));
	keys.release("ToSpecial");
	frame = actions.poll(keys, kLive);
	CHECK(!frame.to_special_held && to_special(frame));
	// Uncaptured: the press latches, no dispatch; nor its release.
	PlayerActionPoll uncaptured = kLive;
	uncaptured.captured = false;
	keys.hold("ToSpecial", 0x46);
	frame = actions.poll(keys, uncaptured);
	CHECK(frame.to_special_held && !to_special(frame));
	frame = actions.poll(keys, kLive);
	CHECK(!to_special(frame));
	keys.release("ToSpecial");
	PlayerActionPoll no_sim = kLive;
	no_sim.simulation_available = false;
	frame = actions.poll(keys, no_sim);
	CHECK(!frame.to_special_held && !to_special(frame));
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

// The special-key chain's held-USE word: the hold the last poll's USE row latched (the
// mount chain alone), which the next input frame's head ages into the word the chain
// reads. A frame whose arm leaves the chain (the armory's open, a press the head or the
// latch takes) latches no hold, a seat pick that consumed the hold keeps the word up
// while the key is held, and the release frame clears it.
// [orig: Input_ProcessFrame @0x49d520 - g_UseItemHoldPrev = g_UseItemHoldLatch
//  @0x49d57f, the latch cleared @0x49d585; Input_HandleActionBinding_0 case 0xB1's
//  latch @0x4e0b71; Input_HandleSpecialKeys @0x49c5c0 - the held-USE arm's test
//  @0x49c6d8, a non-digit's jump to the tail @0x49c6e0]
bool test_use_held_is_the_hold_the_chain_reads() {
	using Arm = UseItemPressArm;
	std::vector<Arm> arms;
	std::size_t asked = 0;
	PlayerActionPoll gate = kLive;
	gate.take_use_item_arm = [&]() { return asked < arms.size() ? arms[asked++] : Arm::kTaken; };
	PlayerActions actions;
	Keys keys;
	CHECK(!actions.use_held());
	arms = {Arm::kMountChain, Arm::kMountChain, Arm::kMountChain, Arm::kTaken};
	keys.hold("useitem");
	actions.poll(keys, gate);
	CHECK(actions.use_held()); // the first held frame's latch: the next frame's word
	CHECK(picks(actions.chain_digit_key('2', false, false), Action::SelectSeat, 1));
	CHECK(actions.use_held()); // the seat pick consumes the hold, not the word
	CHECK(actions.poll(keys, gate).requests.empty());
	CHECK(actions.use_held());
	actions.poll(keys, gate);
	CHECK(actions.use_held());
	// A taken press latches no hold: the word drops while the key is down, and the
	// consumed hold's release edge toggles nothing.
	CHECK(actions.poll(keys, gate).requests.empty());
	CHECK(!actions.use_held());
	keys.release("useitem");
	actions.poll(keys, gate);
	CHECK(!actions.use_held());

	PlayerActions armory;
	arms = {Arm::kOpenArmory};
	asked = 0;
	keys.hold("useitem");
	CHECK(requests_are(armory.poll(keys, gate), {{Action::OpenArmory}}));
	CHECK(!armory.use_held());
	keys.release("useitem");

	PlayerActions released;
	arms = {Arm::kMountChain};
	asked = 0;
	keys.hold("useitem");
	released.poll(keys, gate);
	CHECK(released.use_held());
	keys.release("useitem");
	CHECK(requests_are(released.poll(keys, gate), {{Action::ToggleMount}}));
	CHECK(!released.use_held());
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
	// An inactive frame that is no menu runs the input frame: the arm is not
	// asked, so the hold ends there and the mount toggles.
	PlayerActions overlay;
	keys.hold("useitem", 0x10);
	overlay.poll(keys, gate);
	PlayerActionPoll inactive = gate;
	inactive.active = false;
	CHECK(requests_are(overlay.poll(keys, inactive), {{Action::ToggleMount}}));
	keys.release("useitem");
	CHECK(overlay.poll(keys, gate).requests.empty());
	return true;
}

// ToSpecial's off-keyboard arm keeps the release pass's gates: a release made
// over a screen that holds the mouse still dispatches (the session's
// dispatcher head then drops it for a dead player, under the deploy hold and
// once the round is over); an open text line drops it. The press keeps its
// gates. [orig: Input_ProcessKeyboardEvents @0x49D249..0x49D2B9, @0x49D29F]
bool test_to_special_release_over_a_screen() {
	Keys keys;
	PlayerActionPoll gate;
	gate.active = true;
	gate.captured = true;
	gate.simulation_available = true;
	PlayerActions actions;
	keys.hold("ToSpecial", 'F');
	CHECK(to_special(actions.poll(keys, gate)));
	PlayerActionPoll screen = gate;
	screen.active = false;
	screen.captured = false;
	CHECK(!to_special(actions.poll(keys, screen)));
	keys.release("ToSpecial");
	CHECK(to_special(actions.poll(keys, screen))); // the release, over the screen
	keys.hold("ToSpecial", 'F');
	CHECK(!to_special(actions.poll(keys, screen))); // no press there
	keys.release("ToSpecial");
	PlayerActionPoll typing = gate;
	typing.keyboard_captured = true;
	CHECK(!to_special(actions.poll(keys, typing))); // the text line drops it
	return true;
}

int main() {
	int failed = 0;
	for (const auto test : {test_order_and_held_rows, test_capture_and_overlay_edges,
			test_missing_simulation_freezes_only_scope_zero, test_reset_preserves_switch_hud_medic_and_use_state,
			test_use_previous_frame_and_single_seat,
			test_use_release_digit_priority_and_no_sim_consumption,
			test_shell_consume_and_overlay_cancel, test_use_swallows_only_digit_event_rows,
			test_live_binding_modifier_remap_and_use_stream,
			test_nvg_gain_needs_ctrl_and_shift_reverses_waypoint, test_wheel_subset_and_repeated_events,
			test_wheel_remainder_dispatches_whole_notches,
			test_spectator_rows_edge_to_their_codes, test_key_state_reset,
			test_menu_freezes_the_use_hold, test_to_special_release_over_a_screen,
			test_event_rows_fire_once_per_event, test_the_chain_takes_digit_keys,
			test_reload_medic_and_respawn_fire_on_their_events, test_embedder_rows_keep_the_key_order,
			test_to_special_dispatches_its_key_events, test_spectator_rows_fire_on_their_events,
			test_picks_escape_and_spectator_rows_keep_the_key_order,
			test_voice_menu_digits_pick_and_swallow_the_rows,
			test_to_special_dispatches_both_edges, test_respawn_edges_on_ctrl_r,
			test_held_use_runs_the_arm_every_frame, test_use_held_is_the_hold_the_chain_reads,
			test_key_rows_carry_their_dispatch_codes})
		if (!test()) ++failed;
	std::cout << "player_actions: " << failed << " failed\n";
	return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
