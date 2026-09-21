#pragma once

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
};

struct PlayerActionRequest {
	PlayerAction action = PlayerAction::ToggleMount;
	int value = 0;
};

struct PlayerActionPoll {
	bool active = false;
	bool captured = false;
	bool simulation_available = false;
};

struct PlayerActionFrame {
	bool fire_held = false;
	bool fire_edge = false;
	bool reload_edge = false;
	bool medic_edge = false;
	// Apply in order: the USE special-key/release arm precedes binding rows.
	std::vector<PlayerActionRequest> requests;
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

private:
	struct RowLatch { bool down = false; };
	std::vector<RowLatch> rows_;
	bool fire_was_held_ = false;
	bool reload_was_down_ = false;
	bool medic_was_down_ = false;
	bool use_latched_ = false;
	bool use_held_prev_ = false;
	bool use_hold_consumed_ = false;
	bool use_consume_pending_ = false;
	bool use_digit_was_down_[10] = {};

	void sample_use(const PlayerActionSource &source, const PlayerActionPoll &gate,
			PlayerActionFrame &frame);
};

// Only the seven currently routed wheel actions. The host admits a pressed
// wheel event with active gameplay, a player and a simulation; wheel events
// neither require mouse capture nor advance the polled key latches.
std::optional<PlayerActionRequest> player_wheel_action(std::string_view token);

} // namespace opennova::controls
