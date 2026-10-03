#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace opennova::controls {

// The embedder resolves bindings against its devices. A keyboard match returns
// its VK; a mouse/joystick match has no keyboard VK. Digits are physical keys,
// independent of remapping and modifier claims.
class PlayerActionSource {
public:
	virtual ~PlayerActionSource() = default;
	virtual bool pressed(const char *token) const = 0;
	virtual int pressed_key(const char *token) const = 0;
	virtual bool digit_down(int digit) const = 0;
	// Shift held: a fallback-pass row fires with the direction bit
	// [orig: Input_ProcessKeyboardEvents @0x49d452..0x49d470 — Shift or row
	//  flag 0x200 ORs 0x80000000 into the queued action].
	virtual bool shift_down() const = 0;
};

enum class PlayerAction {
	ToggleMount,
	SelectSeat,
	ToggleScope,
	WeaponCategory,
	WeaponCycle,
	Stance,
	ScopeZero,
	RadarZoom,
	MapCycle,
	Binoculars,
	NightVision,
	NvgGain,
	WaypointCycle, // value -1 = backward (the Shift direction bit)
	// The death screen's spectator rows (value = the dispatch code 500 / 501
	// / 502; replication::ClientReplicaPipeline::spectate_action).
	Spectate,
	// A digit while the Emotes / Radio menu is open: value 1..10 (the 0 key
	// is 10). The embedder sends it (C2S 0x14 / C2S 0x13) and closes that
	// menu (hud::hud_toggles_close_voice_menu).
	EmotePick,
	RadioPick,
};

struct PlayerActionRequest {
	PlayerAction action = PlayerAction::ToggleMount;
	int value = 0;
};

struct PlayerActionPoll {
	bool active = false;
	bool captured = false;
	bool simulation_available = false;
	// An open text line owns the keyboard (BindingSet::set_keyboard_captured):
	// the held-USE digits go to the line, not the seats.
	bool keyboard_captured = false;
	// The F9 Emotes / F10 Radio menus' open words as the HUD toggles hold them
	// this frame (hud::HudToggleState): a digit goes to the open menu.
	bool emotes_menu_open = false;
	bool radio_menu_open = false;
	// The death screen is up (g_DeathScreenActive): the binding scan admits
	// only the rows whose mode word carries bit 2, so the fire, reload and
	// medic rows and the weapon/seat/scope rows stay silent
	// [orig: Input_IsBindingActiveForMode @0x497ea0].
	bool death_screen = false;
};

struct PlayerActionFrame {
	bool fire_held = false;
	bool fire_edge = false;
	bool reload_edge = false;
	bool medic_edge = false;
	// ToSpecial (catalog row 37, default F, dispatch 220): its keys' live state
	// and a dispatch on either edge, press or release — the row's flags carry
	// the release bit (0x80000000) beside the press dispatch, and the handler
	// tells the two apart by the keys' state [orig: Input_ProcessKeyboardEvents
	// @0x49D249..0x49D2B9 (release) / @0x49D42F (press); row flags 0x8C000801].
	bool to_special_held = false;
	bool to_special_edge = false;
	// Apply in order: the USE special-key/release arm precedes binding rows.
	std::vector<PlayerActionRequest> requests;
};

// The WM_MOUSEWHEEL accumulator in front of the wheel binding rows: each
// message's signed delta (WHEEL_DELTA 120 per notch, positive away from the
// user) joins a persistent remainder, and every whole +120 dispatches one
// wheel-up event (the 0x400 binding mask) while every whole -120 dispatches one
// wheel-down event (0x800); the rest carries into the next message.
// [orig: Input_DispatchMouseEvent @ 0x761470 -- `sar eax,10h` / `add eax,
//  g_MouseState.wheelRemainder` @ 0x7614AB..0x7614AE, the +120 walk firing
//  event 0x100 @ 0x761575..0x7615AA, the -120 walk firing 0x200
//  @ 0x7615AC..0x7615DB; the events map to masks 0x400 / 0x800 in
//  Input_TryDispatchBindingByWeaponType @ 0x4992FC / @ 0x499311]
inline constexpr int32_t kWheelDelta = 120;
class WheelRemainder {
public:
	// Returns the signed whole-notch count this delta completes.
	int feed(int32_t delta);

private:
	int32_t remainder_ = 0;
};

// Owns the action table's gates and latches across frames and mission rebuilds.
// A host without a player (including spectator mode) does not poll this state.
// Request refusal and all gameplay effects belong to the simulation.
class PlayerActions {
public:
	PlayerActions();
	PlayerActionFrame poll(const PlayerActionSource &source, const PlayerActionPoll &gate);
	// The presenter reset clears trigger, stance and scope-zero edges only;
	// switch/HUD/medic/USE state survives it, just as it survives host teardown.
	void reset();
	// Deferred until after the next poll's fresh-USE-press reset.
	void consume_use_hold();
	// The death screen's spectator rows, the catalog's mode-2 rows 110..112
	// (CycleSpectatorMode, IncSpectatorTarget, DecSpectatorTarget): the binding
	// scan admits them only while the death screen is up, so the embedder
	// polls them there, on their press edges, while gameplay input is active.
	// [orig: Input_IsBindingActiveForMode @0x497ea0 — row +8 bit 2 on the
	//  death screen; rows @0x818810 / @0x81887C / @0x8188E8 -> codes 500..502]
	std::vector<PlayerActionRequest> poll_spectator(const PlayerActionSource &source, bool active);

private:
	struct RowLatch { bool down = false; };
	std::vector<RowLatch> rows_;
	bool fire_was_held_ = false;
	bool reload_was_down_ = false;
	bool medic_was_down_ = false;
	bool to_special_was_down_ = false;
	bool use_latched_ = false;
	bool use_held_prev_ = false;
	bool use_hold_consumed_ = false;
	bool use_consume_pending_ = false;
	bool use_digit_was_down_[10] = {};
	// This frame's menu digit arm: a menu was open while gameplay input ran.
	bool menu_digits_ = false;
	bool spectator_was_down_[3] = {};

	void sample_use(const PlayerActionSource &source, const PlayerActionPoll &gate,
			PlayerActionFrame &frame);
};

// Only the seven currently routed wheel actions. The host admits a pressed
// wheel event with active gameplay, a player and a simulation; wheel events
// neither require mouse capture nor advance the polled key latches.
std::optional<PlayerActionRequest> player_wheel_action(std::string_view token);

} // namespace opennova::controls
