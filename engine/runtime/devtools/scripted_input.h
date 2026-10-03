// The scenario driver's scripted input device: a test instrument, not a port.
// A network-parity scenario (drive a vehicle, capture a zone, shoot a player,
// throw a grenade at your own feet) is one shared script that OpenNova and
// retail both play -- retail through the onHook virtual keyboard keyed on
// retail action codes, OpenNova through this device. The script names the
// codes the catalog's rows dispatch (controls::action_for_code: forward 152,
// fire 149, USE 177, ...), so the device holds the row's config token and
// the embedder's binding seam reports that token held like any physical key:
// the binding scan, PlayerActions' edges and gates, the simulation and the
// wire run unchanged. Look steps are raw mouse pixels the embedder feeds
// through its mouse-motion path.
//
// Timing is keyed on the session's logic tick, quantized to the display
// frame (one frame runs 0..n ticks): start() arms the device, the first
// advance() latches its logic tick as the start S, and a step at relative
// tick t applies at the first sample whose logic tick L has L - S >= t; its
// record is that L. A token changes state at most once per sample: a step
// that would flip a token this sample already flipped waits, with every later
// step in order, for the next sample, so a press reaches at least one device
// sample even when a frame banked several ticks. End (and cancel) releases
// everything; a logic clock that runs backwards (a reloaded mission) cancels.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::devtools {

// One step of the shared script, as its JSON carries it: {"tick": t, then one
// of "down": code | "up": code | "press": code | "look_px": [dx, dy] |
// "end": true}. A press is down at t and up at t + 1.
struct ScriptStep {
	enum class Kind : uint8_t { Down, Up, Press, Look, End };
	Kind kind = Kind::End;
	int64_t tick = 0;   // relative logic tick, >= 0
	int code = 0;       // retail action code (Down / Up / Press)
	double dx_px = 0.0; // Look: raw mouse pixels, +x right, +y down
	double dy_px = 0.0;
};

// One source step's outcome: the logic tick it applied at (the sample's L;
// -1 while pending) and, for a press, the tick its release applied at.
struct ScriptStepRecord {
	int64_t applied_logic_tick = -1;
	int64_t release_logic_tick = -1;
};

// The script's setup pose in the onHook bridge's ApplyPose words: the
// position in 16.16 fixed point and the heading/pitch in 32-bit BAM. The
// conversion is the bridge's own (onhook/src/mcp/snapshot_parse.cpp:
// truncating Q16 within int32; heading from the wrapped 90 - yaw; pitch
// truncated within +-90 degrees), so both drivers land the same words.
struct ScriptPose {
	int32_t position_q16[3] = {};
	int32_t heading_bam = 0;
	int32_t pitch_bam = 0;
};
bool script_pose_from_mission(double x, double y, double z, double yaw_deg, double pitch_deg,
		ScriptPose &out, std::string &error);

class ScriptedInput {
public:
	enum class State : uint8_t { Idle, Armed, Running, Finished, Cancelled };

	// Validate and schedule a script, replacing (and releasing) any previous
	// one. Refused with `error` set, leaving the device as it was: a negative
	// tick, a code no catalog row dispatches, a non-finite look, no end step
	// or a second one, or a step (a press's release included) after the end.
	bool load(const std::vector<ScriptStep> &steps, std::string &error);
	// Arm a loaded script; the next advance() latches the start. False when
	// nothing is loaded or the script already started.
	bool start();
	// Release everything; a pending or running script stops for good.
	void cancel();
	// The frame's device sample at the session's logic tick.
	void advance(int64_t logic_tick);

	bool token_held(std::string_view token) const;
	// The look pixels the applied steps fed since the last take.
	void take_look(double &r_dx_px, double &r_dy_px);

	State state() const { return state_; }
	bool loaded() const { return !events_.empty(); }
	int64_t start_logic_tick() const { return start_tick_; }
	int64_t end_logic_tick() const { return end_tick_; }
	const std::vector<ScriptStepRecord> &records() const { return records_; }
	std::vector<int> held_codes() const;
	// Why a script stopped short ("" after a clean end).
	const std::string &cancel_reason() const { return cancel_reason_; }

private:
	enum class Op : uint8_t { Down, Up, Look, End };
	struct Event {
		int64_t tick = 0;
		uint32_t step = 0;
		Op op = Op::End;
		bool press_release = false; // the up half of a press
		const char *token = nullptr;
		int code = 0;
		double dx_px = 0.0;
		double dy_px = 0.0;
	};
	struct Held {
		const char *token = nullptr;
		int code = 0;
	};

	void reset();
	void stop(State state, std::string reason);
	bool apply(const Event &e, std::vector<const char *> &changed);

	std::vector<Event> events_;
	std::vector<ScriptStepRecord> records_;
	std::vector<Held> held_;
	size_t next_ = 0;
	State state_ = State::Idle;
	int64_t start_tick_ = -1;
	int64_t end_tick_ = -1;
	int64_t last_tick_ = -1;
	double look_dx_ = 0.0;
	double look_dy_ = 0.0;
	std::string cancel_reason_;
};

} // namespace opennova::devtools
