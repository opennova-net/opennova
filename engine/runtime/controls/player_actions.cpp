#include "player_actions.h"

#include "controls.h"
#include <runtime/world/player_present.h>

#include <iterator>

namespace opennova::controls {
namespace {

enum class Gate { Captured, Active };
enum RowFlags {
	CaptureLatch = 1, // trigger latch sees the gated state, unlike event rows
	Reset = 2,
	NeedsSimulation = 4, // scope-zero latches freeze while no sim exists
	Wheel = 8,
};

struct ActionRow {
	const char *token;
	PlayerActionRequest request;
	Gate gate;
	int flags = 0;
};

using Action = PlayerAction;
const ActionRow kRows[] = {
	{"scope", {Action::ToggleScope}, Gate::Captured, CaptureLatch | Reset},
	// Ctrl+1..Ctrl+0 and bare weapon digits are already separated by the
	// binding sampler's two passes. Mount admission stays in the simulation.
	// [orig: Input_HandleActionBinding_0 cases 0xB6..0xBF @0x4E0B81..0x4E0C22;
	//  Player_SwitchToWeaponByHandle parentSlot gate @0x4e0192]
	{"seat1", {Action::SelectSeat, 0}, Gate::Captured},
	{"seat2", {Action::SelectSeat, 1}, Gate::Captured},
	{"seat3", {Action::SelectSeat, 2}, Gate::Captured},
	{"seat4", {Action::SelectSeat, 3}, Gate::Captured},
	{"seat5", {Action::SelectSeat, 4}, Gate::Captured},
	{"seat6", {Action::SelectSeat, 5}, Gate::Captured},
	{"seat7", {Action::SelectSeat, 6}, Gate::Captured},
	{"seat8", {Action::SelectSeat, 7}, Gate::Captured},
	{"seat9", {Action::SelectSeat, 8}, Gate::Captured},
	{"seat10", {Action::SelectSeat, 9}, Gate::Captured},
	// Catalog rows 28..36: Knife '1', Secondary '2', Primary '3', Flashbang
	// '4', FragGrenade '5', SmokeGrenade '6', Accessory '7', Detonator '8',
	// medpack '9' request categories 1..9; rows 39/40 cycle '[' / ']'.
	// [orig: input cases 200-210 @ 0x4e1144 ->
	//  Player_SwitchToWeaponByHandle((action-200)*65); cases 212/214 ->
	//  Player_CycleWeaponSlot @ 0x4dfe70; engine/runtime/controls k_catalog rows]
	{weapon_category_token(0), {Action::WeaponCategory, 1}, Gate::Captured},
	{weapon_category_token(1), {Action::WeaponCategory, 2}, Gate::Captured},
	{weapon_category_token(2), {Action::WeaponCategory, 3}, Gate::Captured},
	{weapon_category_token(3), {Action::WeaponCategory, 4}, Gate::Captured},
	{weapon_category_token(4), {Action::WeaponCategory, 5}, Gate::Captured},
	{weapon_category_token(5), {Action::WeaponCategory, 6}, Gate::Captured},
	{weapon_category_token(6), {Action::WeaponCategory, 7}, Gate::Captured},
	{weapon_category_token(7), {Action::WeaponCategory, 8}, Gate::Captured},
	{weapon_category_token(8), {Action::WeaponCategory, 9}, Gate::Captured},
	// P is retail action 212 (+1: the next-higher weapon slot, or a +2 step of
	// a variable optic), N is 214 (-1 / -2), although the catalog names P
	// "Cycle Weapon Prev". The sign drives both infantry cycling and optical
	// magnification. [orig: Input_HandleActionBinding_0 @ 0x4E0420, calls
	// @ 0x4E1341 / @ 0x4E13A4; Player_CycleWeaponSlot `add edi,ebp` @ 0x4DFEF6]
	// docs/world/tank-parity-re.md (D-CTRL-5).
	{"cycleweaponP", {Action::WeaponCycle, 1}, Gate::Captured, Wheel},
	{"cycleweaponN", {Action::WeaponCycle, -1}, Gate::Captured, Wheel},
	// Three-key SELECT: the sim owns mutual exclusion and ForceCrouch refusal.
	// [orig: input cases 170/169/172 @0x4e0df3/@0x4e0d77/@0x4e0e3e ->
	//  NapiNPServerMsg_HandleStanceChange @0x501c60]
	{"Stand", {Action::Stance, 0}, Gate::Active, Reset | Wheel},
	{"Crouch", {Action::Stance, 1}, Gate::Active, Reset | Wheel},
	{"Prone", {Action::Stance, 2}, Gate::Active, Reset | Wheel},
	{"ScopeZeroDec", {Action::ScopeZero, -1}, Gate::Active, Reset | NeedsSimulation | Wheel},
	{"ScopeZeroInc", {Action::ScopeZero, 1}, Gate::Active, Reset | NeedsSimulation | Wheel},
	// Radarout grows the world extent (x1.15 toward 0x100000), radarin shrinks
	// it (x0.85 toward 4096). The simulation owns those steps. The HUD's code
	// 19 declutter row stays in its separate poll (D-CTRL-4).
	// [orig: Input_HandleActionBinding @0x49AD40 -- radarout row 48 = case 361
	//  @0x49beaf, radarin row 49 = case 360 @0x49bcb0; code 19 ->
	//  Input_HandleActionBinding_0 @0x4e060b..0x4e0624 ->
	//  CRenderState_SetLayerVisibility @0x59B0F0]
	{"radarout", {Action::RadarZoom, 1}, Gate::Active},
	{"radarin", {Action::RadarZoom, -1}, Gate::Active},
	// Off -> north-up window -> fullscreen -> off, in the in-game dispatcher.
	// [orig: row 98 code 28 -> the @0x4e0662 arm -> HUD_CycleMapMode
	//  @0x520bc0 (0->2->3->0)]
	{"map_toggle", {Action::MapCycle}, Gate::Active},
	// B / N / Ctrl+= / Ctrl+-: the sim owns the witnessed refusals (fire
	// charge, the scoped seat-3 view) and the gain clamp 0..4.
	// [orig: rows 103/104/45/46 = dispatch 26 / 41 / 56 / 57 ->
	//  Input_HandleActionBinding_0 cases 0x1A, 0x29, 0x38, 0x39 @0x4e0420]
	{"binoculars", {Action::Binoculars}, Gate::Active},
	{"NVG", {Action::NightVision}, Gate::Active},
	{"nvggainup", {Action::NvgGain, 1}, Gate::Active},
	{"nvggaindown", {Action::NvgGain, -1}, Gate::Active},
	// F7; the value is replaced by the Shift direction when the row fires.
	// [orig: row 51 = dispatch 23 @0x49b3de]
	{"NextWaypoint", {Action::WaypointCycle, 1}, Gate::Active},
};

} // namespace

PlayerActions::PlayerActions() : rows_(std::size(kRows)) {}

// USE ages before the action rows: LAST frame's hold governs digit swallowing,
// including a digit arriving on the release frame. A fresh press clears the
// consumed flag; a shell chord applies after that clear. An inactive gameplay
// frame cancels the chain, so closing an overlay cannot toggle a mount.
// The special-key chain's digit arms, first open wins: the held-USE seat pick,
// then the Emotes menu, then the Radio menu; a digit an arm takes never
// reaches the binding rows (the swallow in poll()).
// [orig: Input_ProcessFrame @0x49d520 -- the latch aging @0x49d57f..0x49d585,
//  the release edge @0x49d6c1..0x49d6dc -> Entity_ToggleVehicleMount
//  @0x436950; Input_HandleActionBinding_0 case 0xB1 @0x4e0a84, LABEL_121
//  @0x4e0b65..0x4e0b71; Input_HandleSpecialKeys @0x49c5c0, the held-USE digit
//  arm @0x49c6d8..0x49c730 -> Entity_FindAvailableSeat @0x436790, the Emotes
//  arm @0x49c731..0x49c77c, the Radio arm @0x49c783..0x49c7c8]
void PlayerActions::sample_use(const PlayerActionSource &source, const PlayerActionPoll &gate,
		PlayerActionFrame &frame) {
	use_held_prev_ = use_latched_;
	use_latched_ = false;
	if (!gate.active) use_held_prev_ = false;
	if (gate.active && source.pressed("useitem")) {
		if (!use_held_prev_) use_hold_consumed_ = false;
		use_latched_ = true;
	}
	if (use_consume_pending_) {
		use_hold_consumed_ = true;
		use_consume_pending_ = false;
	}
	menu_digits_ = gate.active && (gate.emotes_menu_open || gate.radio_menu_open);
	for (int digit = 0; digit < 10; ++digit) {
		// The digit arm is a key-press path, so an open text line takes the
		// digits [orig: Input_HandleSpecialKeys @0x49c5c0 runs only with
		// g_InputCaptureMode clear, Input_ProcessKeyboardEvents @0x49d2e3].
		const bool digit_down = !gate.keyboard_captured && source.digit_down(digit);
		if (!world::latched_key_edge(digit_down, use_held_prev_ || menu_digits_,
				use_digit_was_down_[digit])) continue;
		if (!use_held_prev_) {
			// The menu arms: keys 1..9 pick 1..9 and key 0 picks 10, sent and
			// the menu closed; the Emotes menu wins when both are open.
			// [orig: @0x49c745..0x49c75c (emotes, C2S 0x14) and
			//  @0x49c791..0x49c7a8 (radio, C2S 0x13)]
			const int pick = digit == 0 ? 10 : digit;
			frame.requests.push_back(
					{gate.emotes_menu_open ? Action::EmotePick : Action::RadioPick, pick});
			continue;
		}
		// Keys 1..9 select seats 0..8, key 0 seat 9 [orig: @0x49c6e6..0x49c6ed].
		const int seat = digit == 0 ? 9 : digit - 1;
		if (!use_hold_consumed_ && gate.simulation_available)
			frame.requests.push_back({Action::SelectSeat, seat});
		use_hold_consumed_ = true;
	}
	if (!use_latched_ && use_held_prev_ && !use_hold_consumed_ && gate.simulation_available)
		frame.requests.push_back({Action::ToggleMount});
}

PlayerActionFrame PlayerActions::poll(const PlayerActionSource &source, const PlayerActionPoll &gate) {
	PlayerActionFrame frame;
	sample_use(source, gate, frame);
	const bool captured = gate.active && gate.captured;
	// Fire held+edge, reload raw edge (refusal belongs to the sim), ADS toggle
	// request: their latches see the CAPTURE-GATED state, so UI clicks never
	// fire. Scope is the first request row above, with the same latch policy.
	// [orig: the binding dispatch cases 0x95 fire / 0xD3 reload / 6 scope,
	//  Input_HandleActionBinding_0 @0x4e0420 -- ported in engine/runtime/world
	//  weapon_fsm + Simulation]
	frame.fire_held = captured && source.pressed("attack_1");
	frame.fire_edge = world::latched_key_edge(frame.fire_held, true, fire_was_held_);
	frame.reload_edge = world::latched_key_edge(captured && source.pressed("magazine"),
			true, reload_was_down_);
	// The death screen releases capture. Medic still samples even with
	// inactive gameplay or no simulation; the sim owns death/cooldown gates.
	// [orig: Input_HandleActionBinding case 217 @0x49b4b4 (row 64 MedicReq)]
	frame.medic_edge = world::latched_key_edge(source.pressed("MedicReq"), true, medic_was_down_);
	for (std::size_t i = 0; i < std::size(kRows); ++i) {
		const auto &row = kRows[i];
		if ((row.flags & NeedsSimulation) && !gate.simulation_available) continue;
		const bool active = row.gate == Gate::Captured ? captured : gate.active;
		const bool capture_latch = (row.flags & CaptureLatch) != 0;
		const bool down = (!capture_latch || active) && source.pressed(row.token);
		bool swallowed = false;
		if (!capture_latch && down && (use_held_prev_ || menu_digits_)) {
			// VK digits only: a rebound digit is swallowed, a mouse/joystick
			// match (VK 0) is not [orig: the (key - 48) <= 9 tests @0x49c6e0 /
			// @0x49c73f / @0x49c78b].
			const int vk = source.pressed_key(row.token);
			swallowed = vk >= 0x30 && vk <= 0x39;
		}
		// Raw event-row latches advance behind overlays, without capture,
		// during a USE hold and without a sim. Only scope-zero freezes above.
		if (world::latched_key_edge(down, active && !swallowed, rows_[i].down) &&
				gate.simulation_available) {
			PlayerActionRequest request = row.request;
			if (request.action == Action::WaypointCycle && source.shift_down()) request.value = -1;
			frame.requests.push_back(request);
		}
	}
	return frame;
}

std::vector<PlayerActionRequest> PlayerActions::poll_spectator(const PlayerActionSource &source,
		bool active) {
	// [orig: rows 110 / 111 / 112 -> Input_HandleActionBinding cases 500
	//  @0x49bd58, 501 @0x49bd67, 502 @0x49bd89]
	static constexpr struct {
		const char *token;
		int code;
	} kSpectatorRows[3] = {
		{"CycleSpectatorMode", 500},
		{"IncSpectatorTarget", 501},
		{"DecSpectatorTarget", 502},
	};
	std::vector<PlayerActionRequest> out;
	for (std::size_t i = 0; i < std::size(kSpectatorRows); ++i) {
		const bool down = source.pressed(kSpectatorRows[i].token);
		if (world::latched_key_edge(down, active, spectator_was_down_[i]))
			out.push_back({Action::Spectate, kSpectatorRows[i].code});
	}
	return out;
}

void PlayerActions::reset() {
	fire_was_held_ = false;
	reload_was_down_ = false;
	for (std::size_t i = 0; i < std::size(kRows); ++i)
		if (kRows[i].flags & Reset) rows_[i].down = false;
}

void PlayerActions::consume_use_hold() {
	use_consume_pending_ = true;
}

int WheelRemainder::feed(int32_t delta) {
	// [orig: Input_DispatchMouseEvent @ 0x7614A9..0x7615DB]
	remainder_ += delta;
	int notches = 0;
	for (; remainder_ >= kWheelDelta; remainder_ -= kWheelDelta) ++notches;  // @ 0x761575..0x7615AA
	for (; remainder_ <= -kWheelDelta; remainder_ += kWheelDelta) --notches; // @ 0x7615AC..0x7615DB
	return notches;
}

std::optional<PlayerActionRequest> player_wheel_action(std::string_view token) {
	for (const auto &row : kRows)
		if ((row.flags & Wheel) && token == row.token) return row.request;
	return std::nullopt;
}

} // namespace opennova::controls
