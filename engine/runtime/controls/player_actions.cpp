#include "player_actions.h"

#include "controls.h"
#include <runtime/world/player_present.h>

#include <iterator>

namespace opennova::controls {
namespace {

enum RowFlags {
	CaptureLatch = 1, // trigger latch sees the gated state, unlike event rows
	Reset = 2,
	NeedsSimulation = 4, // scope-zero latches freeze while no sim exists
};

using ActionRow = PlayerActionRow;
constexpr bool kCaptured = true; // the captured gate (gameplay input captured)
constexpr bool kActive = false;  // the active gate alone

using Action = PlayerAction;
// The rows ahead of the weapon rows, then the rows after them (action_rows).
const ActionRow kRowsHead[] = {
	{"scope", {Action::ToggleScope}, kCaptured, CaptureLatch | Reset},
	// Ctrl+1..Ctrl+0 and bare weapon digits are already separated by the
	// binding sampler's two passes. Mount admission stays in the simulation.
	// [orig: Input_HandleActionBinding_0 cases 0xB6..0xBF @0x4E0B81..0x4E0C22;
	//  Player_SwitchToWeaponByHandle parentSlot gate @0x4e0192]
	{"seat1", {Action::SelectSeat, 0}, kCaptured},
	{"seat2", {Action::SelectSeat, 1}, kCaptured},
	{"seat3", {Action::SelectSeat, 2}, kCaptured},
	{"seat4", {Action::SelectSeat, 3}, kCaptured},
	{"seat5", {Action::SelectSeat, 4}, kCaptured},
	{"seat6", {Action::SelectSeat, 5}, kCaptured},
	{"seat7", {Action::SelectSeat, 6}, kCaptured},
	{"seat8", {Action::SelectSeat, 7}, kCaptured},
	{"seat9", {Action::SelectSeat, 8}, kCaptured},
	{"seat10", {Action::SelectSeat, 9}, kCaptured},
};
// The catalog's weapon rows go here, each requesting its category, the code - 200 (Jointops.exe's
// Knife '1' .. medpack '9' categories 1..9; dfx2.exe's Powerup0 '0' 0 .. Detonator Backspace
// 10); rows 39/40 cycle '[' / ']'.
// [orig: input cases 200-210 @ 0x4e1144 ->
//  Player_SwitchToWeaponByHandle((action-200)*65); cases 212/214 ->
//  Player_CycleWeaponSlot @ 0x4dfe70] [orig: sub_4DD4C0 - cases 200-210 @0x4de239..0x4de24d
//  (dfx2.exe)]
const ActionRow kRowsTail[] = {
	// P is retail action 212 (+1: the next-higher weapon slot, or a +2 step of
	// a variable optic), N is 214 (-1 / -2), although the catalog names P
	// "Cycle Weapon Prev". The sign drives both infantry cycling and optical
	// magnification. [orig: Input_HandleActionBinding_0 @ 0x4E0420, calls
	// @ 0x4E1341 / @ 0x4E13A4; Player_CycleWeaponSlot `add edi,ebp` @ 0x4DFEF6]
	// docs/world/tank-parity-re.md (D-CTRL-5).
	{"cycleweaponP", {Action::WeaponCycle, 1}, kCaptured},
	{"cycleweaponN", {Action::WeaponCycle, -1}, kCaptured},
	// Three-key SELECT: the sim owns mutual exclusion and ForceCrouch refusal.
	// [orig: input cases 170/169/172 @0x4e0df3/@0x4e0d77/@0x4e0e3e ->
	//  NapiNPServerMsg_HandleStanceChange @0x501c60]
	{"Stand", {Action::Stance, 0}, kActive, Reset},
	{"Crouch", {Action::Stance, 1}, kActive, Reset},
	{"Prone", {Action::Stance, 2}, kActive, Reset},
	{"ScopeZeroDec", {Action::ScopeZero, -1}, kActive, Reset | NeedsSimulation},
	{"ScopeZeroInc", {Action::ScopeZero, 1}, kActive, Reset | NeedsSimulation},
	// Radarout grows the world extent (x1.15 toward 0x100000), radarin shrinks
	// it (x0.85 toward 4096). The simulation owns those steps. The HUD's code
	// 19 declutter row stays in its separate poll (D-CTRL-4).
	// [orig: Input_HandleActionBinding @0x49AD40 -- radarout row 48 = case 361
	//  @0x49beaf, radarin row 49 = case 360 @0x49bcb0; code 19 ->
	//  Input_HandleActionBinding_0 @0x4e060b..0x4e0624 ->
	//  CRenderState_SetLayerVisibility @0x59B0F0]
	{"radarout", {Action::RadarZoom, 1}, kActive},
	{"radarin", {Action::RadarZoom, -1}, kActive},
	// Off -> north-up window -> fullscreen -> off, in the in-game dispatcher.
	// [orig: row 98 code 28 -> the @0x4e0662 arm -> HUD_CycleMapMode
	//  @0x520bc0 (0->2->3->0)]
	{"map_toggle", {Action::MapCycle}, kActive},
	// B / N / Ctrl+= / Ctrl+-: the sim owns the witnessed refusals (fire
	// charge, the scoped seat-3 view) and the gain clamp 0..4.
	// [orig: rows 103/104/45/46 = dispatch 26 / 41 / 56 / 57 ->
	//  Input_HandleActionBinding_0 cases 0x1A, 0x29, 0x38, 0x39 @0x4e0420]
	{"binoculars", {Action::Binoculars}, kActive},
	{"NVG", {Action::NightVision}, kActive},
	{"nvggainup", {Action::NvgGain, 1}, kActive},
	{"nvggaindown", {Action::NvgGain, -1}, kActive},
	// F7; the value is replaced by the fire's sign (fire_signed) when the row fires.
	// [orig: row 51 = dispatch 23 @0x49b3de]
	{"NextWaypoint", {Action::WaypointCycle, 1}, kActive},
};

// The weapon row a catalog row is, if any: a code 200..210 requests its category.
bool weapon_row(const ActionDef &def, ActionRow &out) {
	if (def.code < kWeaponCategoryCodeFirst || def.code > kWeaponCategoryCodeLast) return false;
	out = ActionRow{def.token, {Action::WeaponCategory, def.code - kWeaponCategoryCodeFirst}, kCaptured};
	return true;
}

// The action table over `catalog`: the head rows, the catalog's weapon rows in its order, the
// tail rows.
std::vector<ActionRow> action_rows(const Catalog &catalog) {
	std::vector<ActionRow> rows(std::begin(kRowsHead), std::end(kRowsHead));
	for (std::size_t i = 0; i < catalog.count; ++i) {
		ActionRow row;
		if (weapon_row(catalog.rows[i], row)) rows.push_back(row);
	}
	rows.insert(rows.end(), std::begin(kRowsTail), std::end(kRowsTail));
	return rows;
}

// A catalog row's dispatch code by its config token; a token no row carries reads -1
// [orig: word_8159A8 + 108 * row].
int code_of(const Catalog &catalog, const char *token) {
	const ActionDef *def = catalog.for_token(token);
	return def != nullptr ? def->code : -1;
}

} // namespace

PlayerActions::PlayerActions(const Catalog &catalog)
		: catalog_(&catalog), table_(action_rows(catalog)), rows_(table_.size()) {}

int PlayerActions::row_code(const char *token) const { return code_of(*catalog_, token); }

// A catalog row's mode word by its config token (alive bit 1, death-screen bit 2); a token no
// row carries reads 0 [orig: the row's +8 word, Input_IsBindingActiveForMode @0x497ea0].
uint32_t PlayerActions::row_modes(const char *token) const {
	const ActionDef *def = catalog_->for_token(token);
	return def != nullptr ? def->modes : 0;
}

// USE ages before the action rows: LAST frame's hold governs the digit row swallow,
// including a digit arriving on the release frame, as it governs the special-key
// chain's digit arm (chain_digit_key, between the polls). A fresh press clears the
// consumed flag; a shell chord applies after that clear. Every input frame
// ages the latch and runs the release edge, one whose gameplay gate is down
// too; the death and stat screens run whole input frames while their word
// holds, and a menu screen with no pass-through word runs none
// (PlayerActionPoll::menu_open).
// [orig: Game_ProcessMainFrame @0x52661D and UI_ProcessMenuScreenFrame's tail
//  @0x54E695 (the DEATH / STAT word g_MenuInputPassThrough), the two callers of
//  Input_ProcessFrame]
// [orig: Input_ProcessFrame @0x49d520 -- the latch aging @0x49d57f..0x49d585,
//  the release edge @0x49d6c1..0x49d6dc -> Entity_ToggleVehicleMount
//  @0x436950; Input_HandleActionBinding_0 case 0xB1 @0x4e0a84, LABEL_121
//  @0x4e0b65..0x4e0b71]
void PlayerActions::sample_use(const PlayerActionSource &source, const PlayerActionPoll &gate) {
	use_held_prev_ = use_latched_;
	use_latched_ = false;
	use_frame_aged_ = true;
	// The held row queues its record (case 0xB1) on every frame a key is down, under the
	// row's mode word (bit 1 alive, bit 2 the death screen): the poll's last record, the
	// analog pass running last in the player frame; its arm runs at the record's dispatch
	// (use_item_record) and the release edge at the frame's tail (end_input_frame).
	// [orig: Input_ProcessAnalogBindings @0x4995F0 -- the keys @0x499635, the
	//  mode word @0x49963E..0x499650, the queue @0x499660; Input_ProcessPlayerFrame - the
	//  analog pass's call @0x49D50C]
	const bool use_admitted = !gate.death_screen || (row_modes("useitem") & 2u) != 0;
	// A key held through a menu close's reset reads up in the device's key
	// state (KeyStateReset), so the row reads it up too.
	const bool use_down = source.pressed("useitem");
	use_row_down_ = gate.active && use_admitted && use_down;
	menu_digits_ = gate.active && (gate.emotes_menu_open || gate.radio_menu_open);
}

PlayerActionFrame PlayerActions::use_item_record(UseItemPressArm arm) {
	PlayerActionFrame frame;
	// The case turns the auto-run's toggle off at its head (D-NET-563).
	// [orig: Input_HandleActionBinding_0 case 0xB1 - Input_DisableScrollLock @0x4E0A84]
	frame.scroll_lock_off.push_back(row_code("useitem"));
	// Only the mount chain sets the hold latch, so a frame whose arm leaves the chain ends
	// the hold as a release does (the armory opens first, then the tail's toggle).
	// [orig: case 0xB1 @0x4E0A82, the mount chain @0x4E0B5E..0x4E0B71]
	if (arm == UseItemPressArm::kMountChain) {
		if (!use_held_prev_) use_hold_consumed_ = false;
		use_latched_ = true;
	} else if (arm == UseItemPressArm::kOpenArmory) {
		frame.requests.push_back({Action::OpenArmory});
	} else if (arm == UseItemPressArm::kOpenVehicleMenu) {
		frame.requests.push_back({Action::OpenVehicleMenu});
	}
	return frame;
}

PlayerActionFrame PlayerActions::end_input_frame(bool simulation_available) {
	PlayerActionFrame frame;
	if (!use_frame_aged_) return frame;
	use_frame_aged_ = false;
	// A shell chord's consume lands after the frame's fresh-press reset.
	if (use_consume_pending_) {
		use_hold_consumed_ = true;
		use_consume_pending_ = false;
	}
	// The release tail turns the auto-run's toggle off ahead of its mount toggle, whether or
	// not a seat pick consumed the hold (D-NET-563).
	// [orig: Input_ProcessFrame - the release test @0x49D6AD..0x49D6C1, Input_DisableScrollLock
	//  @0x49D6C5, the consumed test @0x49D6CD]
	if (!use_latched_ && use_held_prev_) frame.scroll_lock_off.push_back(-1);
	if (!use_latched_ && use_held_prev_ && !use_hold_consumed_ && simulation_available)
		frame.requests.push_back({Action::ToggleMount});
	return frame;
}

PlayerActionFrame PlayerActions::poll(const PlayerActionSource &source, const PlayerActionPoll &gate) {
	PlayerActionFrame frame;
	// No input frame runs under a menu screen (PlayerActionPoll::menu_open).
	if (gate.menu_open) return frame;
	sample_use(source, gate);
	const bool captured = gate.active && gate.captured;
	// Fire held+edge, reload raw edge (refusal belongs to the sim), ADS toggle
	// request: their latches see the CAPTURE-GATED state, so UI clicks never
	// fire. Scope is the first request row above, with the same latch policy.
	// [orig: the binding dispatch cases 0x95 fire / 0xD3 reload / 6 scope,
	//  Input_HandleActionBinding_0 @0x4e0420 -- ported in engine/runtime/world
	//  weapon_fsm + Simulation]
	const auto admitted = [this, &gate](const char *token) {
		return !gate.death_screen || (row_modes(token) & 2u) != 0;
	};
	frame.fire_held = captured && admitted("attack_1") && source.binding_active("attack_1");
	// The fire row's records (PlayerActionFrame::fire_held carries the witness): a key's press
	// under the captured gate and the scan's mode word, the release pass's event under none,
	// as ToSpecial's. Its off-keyboard arm's edges follow the gated state, as the trigger's
	// latch always has (a capture regained over a held arm presses again), its release
	// unless an open text line holds the keyboard.
	const int fire_code = row_code("attack_1");
	const bool fire_gate = captured && admitted("attack_1");
	const bool fire_off = fire_gate && source.held_off_keyboard("attack_1");
	const bool fire_off_press = fire_off && !fire_off_down_;
	const bool fire_off_release = !fire_off && fire_off_down_ && !gate.keyboard_captured;
	fire_off_down_ = fire_off;
	frame.fire_keys_held = source.key_slots_down("attack_1");
	frame.fire_scripted_left_down = fire_off_press && source.scripted_left_button("attack_1");
	// The event rows fire once per queued key event (an OS repeat included), in
	// the queue's order; their mouse, joystick and scripted arms on their press
	// edges, after the keyboard pass's events (held_off_keyboard). A key the special-key
	// chain took queued no event (chain_digit_key).
	// [orig: Input_ProcessPlayerFrame @0x49D4C0 - Input_ProcessKeyboardEvents
	//  @0x49D4C6 ahead of the mouse and toggle passes @0x49D4D8 / @0x49D4DE]
	const auto key_fired = [&gate](int code) {
		for (const KeyRowEvent &event : gate.key_events)
			if (!event.release && event.code == code) return true;
		return false;
	};
	// The reload row: one record per key event under its gates, its off-keyboard arm's
	// press edge behind the keyboard pass's events (the role's arm, PlayerAction::Reload).
	const int reload_code = row_code("magazine");
	const bool reload_gate = captured && admitted("magazine");
	const bool reload_off = world::latched_key_edge(
			reload_gate && source.held_off_keyboard("magazine"), true, reload_was_down_);
	// The death screen releases capture. Medic still samples even with
	// inactive gameplay or no simulation; the sim owns death/cooldown gates.
	// [orig: Input_HandleActionBinding case 217 @0x49b4b4 (row 64 MedicReq)]
	const int medic_code = row_code("MedicReq");
	const bool medic_off = world::latched_key_edge(
			admitted("MedicReq") && source.held_off_keyboard("MedicReq"), true, medic_was_down_);
	// Respawn: the dispatcher's code 55 row (byte_49C384[54] = 37, jump-table
	// entry 37 -> the case) under the gameplay gate; its latch follows the
	// keys regardless. An alive-mode row, so the death screen's scan never admits it;
	// the live-player, round and session gates are the role's
	// (inmatch::Role::request_respawn). [orig: Input_HandleActionBinding @0x49AEE9..0x49AEF0
	//  (the index byte @0x49C3BA, the entry @0x49C32C) -> case 55 @0x49B20B; the row's code
	//  @0x817658]
	const int respawn_code = row_code("respawn");
	const bool respawn_off = world::latched_key_edge(
			admitted("respawn") && source.held_off_keyboard("respawn"), gate.active,
			respawn_was_down_);
	const bool respawn_fired =
			(gate.active && admitted("respawn") && key_fired(respawn_code)) || respawn_off;
	// Retail's auto-run (D-NET-563, PlayerActionFrame::auto_run_forward): the Scroll Lock word
	// and the held Last_move row queue the forward event; the forward, back and strafe keys'
	// own held events (the analog pass, skipped under a text line, where pressed() reads them
	// up), the Respawn press's input reset and the USE row (use_item_record) turn the toggle off.
	// [orig: Input_ProcessPlayerFrame @0x49D4C0 - @0x49D4E6..0x49D4FA, the analog pass
	//  Input_ProcessAnalogBindings (the call @0x49D50C; the held rows @0x499635, their mode word
	//  @0x49963E..0x499650, Input_QueueEvent(code, 0, 0, 0) @0x499660); Input_HandleActionBinding
	//  case 55 @0x49B20B -> Player_ResetLocalInput (the call @0x4B2B91)]
	const auto held = [&](const char *token) {
		return gate.active && admitted(token) && source.pressed(token);
	};
	frame.auto_run_forward = gate.active && (gate.scroll_lock_on || held("Last_move"));
	for (const char *token : {"move_forward", "move_back", "strafe_left", "strafe_right"})
		if (held(token)) frame.scroll_lock_off.push_back(row_code(token));
	if (respawn_fired) frame.scroll_lock_off.push_back(respawn_code);
	// ToSpecial: the handler reads its keys raw (keys_down, g_InputState.keyState)
	// to tell a press from a release. A press dispatches on its scanned key-down's
	// event, with captured gameplay input and under the death screen's binding
	// scan like the other weapon rows; the release on the release pass's event,
	// which has neither gate: the pass queues it whenever a key whose press latch
	// is set reads up, over a screen too, so a key held through the death (its row
	// scan-gated, @0x49D332..0x49D348) releases at its key-up, and a key held
	// through an open text line (its events the line's, the latch set all the
	// same) releases when it comes up after the line closes; under the line the
	// pass clears the latch with no event (BindingSet::release_pass). Its
	// off-keyboard arm dispatches on both edges, the release unless an open text
	// line holds the keyboard. A release goes to the key's first release row,
	// whichever row took the press (BindingSet::release_row).
	// [orig: Input_HandleActionBinding_0 case 0xDC @0x4E1161..0x4E1181;
	//  Input_ProcessKeyboardEvents, the latch set @0x49D2ED, the release pass
	//  @0x49D249..0x49D2B9 (the capture-mode skip of the queue @0x49D29F), the
	//  press @0x49D42F]
	frame.to_special_held = source.keys_down("ToSpecial");
	const int to_special_code = row_code("ToSpecial");
	const auto to_special_admits = [&](bool release) {
		return gate.simulation_available && (release || (captured && admitted("ToSpecial")));
	};
	const bool to_special_off = source.held_off_keyboard("ToSpecial");
	const bool to_special_off_press = to_special_off && !to_special_off_down_;
	const bool to_special_off_release =
			!to_special_off && to_special_off_down_ && !gate.keyboard_captured;
	to_special_off_down_ = to_special_off;
	const auto request_of = [this, &source](const ActionRow &row, bool sign_known, bool sign) {
		PlayerActionRequest request = row.request;
		// The row's key event reaches its arm through the dispatcher's head.
		request.code = row_code(row.token);
		request.sign = sign_known ? sign : source.fire_signed(row.token);
		// NextWaypoint (code 23) reads the fire's sign: back when signed
		// [orig: Input_HandleActionBinding case 23 @0x49B3DE..0x49B44D].
		if (request.action == Action::WaypointCycle && request.sign) request.value = -1;
		return request;
	};
	// The keyboard pass's events, one record each in the queue's order: the action rows,
	// the medic call and the Respawn key alike (Input_QueueEvent per scanned key-down), so
	// a Respawn ahead of a weapon row ends the drain before it (its reset), and one behind
	// it runs after it.
	for (const KeyRowEvent &event : gate.key_events) {
		// The fire row's press and its release pass's event, each a record in key order.
		if (event.code == fire_code) {
			if (event.release || fire_gate)
				frame.requests.push_back({Action::Fire, 0, fire_code, event.sign});
			continue;
		}
		// ToSpecial's press and its release pass's event, each a record in key order.
		if (event.code == to_special_code) {
			if (to_special_admits(event.release))
				frame.requests.push_back({Action::ToSpecial, 0, to_special_code, event.sign});
			continue;
		}
		if (event.release) continue;
		if (event.code == reload_code) {
			if (reload_gate) frame.requests.push_back({Action::Reload, 0, reload_code, event.sign});
			continue;
		}
		if (gate.embedder_row && gate.embedder_row(event.code)) {
			frame.requests.push_back({Action::EmbedderRow, event.param, event.code, event.sign});
			continue;
		}
		// The death screen's spectator rows (codes 500..502), whose scan admits them only
		// there [orig: Input_HandleActionBinding @0x49AD40 - cases 500 @0x49bd58, 501
		// @0x49bd67, 502 @0x49bd89].
		if (event.code >= 500 && event.code <= 502) {
			if (gate.death_screen && gate.active && gate.simulation_available)
				frame.requests.push_back({Action::Spectate, event.code, event.code, event.sign});
			continue;
		}
		if (event.code == medic_code) {
			if (admitted("MedicReq"))
				frame.requests.push_back({Action::MedicRequest, 0, medic_code, event.sign});
			continue;
		}
		if (event.code == respawn_code) {
			if (gate.active && admitted("respawn"))
				frame.requests.push_back({Action::Respawn, 0, respawn_code, event.sign});
			continue;
		}
		bool owned = false;
		for (const ActionRow &row : table_) {
			if (row_code(row.token) != event.code) continue;
			owned = true;
			const bool active = row.captured ? captured : gate.active;
			if (active && admitted(row.token) && gate.simulation_available)
				frame.requests.push_back(request_of(row, true, event.sign));
			break;
		}
		// A row no arm here owns: the scan queued its event all the same, so it is a record for
		// the drain's dispatcher (player_actions_own_code carries the witness).
		if (!owned)
			frame.requests.push_back({Action::EmbedderRow, event.param, event.code, event.sign});
	}
	// The off-keyboard arms' press edges behind the keyboard pass's events (ToSpecial's
	// arm both edges): the joystick's and the scripted hold's, whose toggle pass runs behind
	// the keyboard pass. A mouse arm is no edge here: its record is the message pump's click
	// record, queued ahead of the key scan as the wheel's is (the embedder's;
	// BindingSet::mouse_click_action) [orig: Input_ProcessPlayerFrame @0x49D4C0
	//  (Input_ProcessKeyboardEvents @0x49D4C6 ahead of Input_ProcessToggleBindings
	//  @0x49D4DE); Game_WindowProc @0x7624C0 (Input_DispatchMouseEvent @0x762574) ->
	//  Input_ProcessBindings @0x4DDA50 -> Input_TryDispatchBindingByWeaponType @0x499180
	//  (Input_QueueEvent @0x499251)].
	if (fire_off_press || fire_off_release) frame.requests.push_back({Action::Fire, 0, fire_code});
	if (reload_off) frame.requests.push_back({Action::Reload, 0, reload_code});
	if ((to_special_off_press || to_special_off_release) &&
			to_special_admits(to_special_off_release))
		frame.requests.push_back({Action::ToSpecial, 0, to_special_code});
	if (medic_off) frame.requests.push_back({Action::MedicRequest, 0, medic_code});
	if (respawn_off) frame.requests.push_back({Action::Respawn, 0, respawn_code});
	for (std::size_t i = 0; i < table_.size(); ++i) {
		const auto &row = table_[i];
		if ((row.flags & NeedsSimulation) && !gate.simulation_available) continue;
		const bool active = row.captured ? captured : gate.active;
		const bool capture_latch = (row.flags & CaptureLatch) != 0;
		const bool down = (!capture_latch || active) && admitted(row.token) &&
				source.held_off_keyboard(row.token);
		bool swallowed = false;
		if (!capture_latch && down && (use_held_prev_ || menu_digits_)) {
			// VK digits only: a rebound digit is swallowed, a mouse/joystick
			// match (VK 0) is not [orig: the (key - 48) <= 9 tests @0x49c6e0 /
			// @0x49c73f / @0x49c78b].
			const int vk = source.pressed_key(row.token);
			swallowed = vk >= 0x30 && vk <= 0x39;
		}
		// The off-keyboard arms' latches advance behind overlays, without
		// capture, during a USE hold and without a sim. Only scope-zero freezes
		// above.
		if (world::latched_key_edge(down, active && !swallowed, rows_[i].down) &&
				gate.simulation_available)
			frame.requests.push_back(request_of(row, false, false));
	}
	// The held USE row's record, the analog pass's, behind every other (sample_use).
	if (use_row_down_) frame.requests.push_back({Action::UseItem, 0, row_code("useitem")});
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
		const bool down = source.held_off_keyboard(kSpectatorRows[i].token);
		if (world::latched_key_edge(down, active, spectator_was_down_[i]))
			out.push_back({Action::Spectate, kSpectatorRows[i].code, kSpectatorRows[i].code});
	}
	return out;
}

ChainDigitTake PlayerActions::chain_digit_key(int vk, bool emotes_menu_open,
		bool radio_menu_open) {
	// [orig: Input_HandleSpecialKeys @0x49c5c0 - the `(key - 48) <= 9` tests @0x49c6e0 /
	//  @0x49c73f / @0x49c78b; an arm whose word holds sends a non-digit to the tail]
	const bool digit = static_cast<unsigned>(vk - 0x30) <= 9u;
	ChainDigitTake take;
	if (use_latched_) {
		if (!digit) return take;
		take.taken = true;
		// Keys 1..9 select seats 0..8, key 0 seat 9 [orig: @0x49c6e6..0x49c6ed]; one pick per
		// hold [orig: the consumed test @0x49c6f8, the store @0x49c70c].
		int seat = vk - 0x31;
		if (seat < 0) seat = 9;
		if (!use_hold_consumed_) take.pick = PlayerActionRequest{Action::SelectSeat, seat};
		use_hold_consumed_ = true;
		return take;
	}
	if (!emotes_menu_open && !radio_menu_open) return take;
	if (!digit) return take;
	// The open menu's pick: keys 1..9 pick 1..9 and key 0 picks 10, the Emotes menu ahead
	// of the Radio menu [orig: @0x49c745..0x49c74c (emotes, C2S 0x14 @0x49c752) and
	// @0x49c791..0x49c798 (radio, C2S 0x13 @0x49c79e)].
	take.taken = true;
	take.pick = PlayerActionRequest{emotes_menu_open ? Action::EmotePick : Action::RadioPick,
			vk == 0x30 ? 10 : vk - 0x30};
	return take;
}

void PlayerActions::reset() {
	fire_off_down_ = false;
	reload_was_down_ = false;
	for (std::size_t i = 0; i < table_.size(); ++i)
		if (table_[i].flags & Reset) rows_[i].down = false;
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

bool player_actions_own_code(const Catalog &catalog, int code) {
	const auto is = [&catalog, code](const char *token) { return code_of(catalog, token) == code; };
	if (is("attack_1") || is("ToSpecial") || is("magazine") || is("MedicReq") || is("respawn") ||
			(code >= 500 && code <= 502))
		return true;
	for (const ActionRow &row : action_rows(catalog))
		if (code_of(catalog, row.token) == code) return true;
	return false;
}

std::optional<PlayerActionRequest> player_action_request(const Catalog &catalog,
		const InputEventRecord &record) {
	for (const ActionRow &row : action_rows(catalog)) {
		if (code_of(catalog, row.token) != record.code) continue;
		PlayerActionRequest request = row.request;
		request.code = record.code;
		request.sign = record.sign;
		// [orig: Input_HandleActionBinding case 23 @0x49B3DE..0x49B44D (the sign)]
		if (request.action == Action::WaypointCycle && record.sign) request.value = -1;
		return request;
	}
	// The death screen's spectator rows (codes 500..502; a mouse button's click record
	// among them) [orig: Input_HandleActionBinding @0x49AD40 - cases 500 @0x49bd58, 501
	//  @0x49bd67, 502 @0x49bd89].
	if (record.code >= 500 && record.code <= 502)
		return PlayerActionRequest{Action::Spectate, record.code, record.code, record.sign};
	return std::nullopt;
}

} // namespace opennova::controls
