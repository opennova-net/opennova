// The Game window's status readout (ADR 0042 d6: records in): the session
// the world runs under, its logic clock and the display frame rate, pushed by
// the embedder on a short cadence while the tools are open. The role and
// state mirror inmatch::RoleKind / inmatch::State by value (the embedder
// static_asserts the pairing; this group never includes inmatch).
#pragma once

#include <cstdint>

namespace opennova::devtools {

enum class StatusRole : uint8_t {
	SinglePlayer = 0,
	ListenServer,
	Joiner,
	DedicatedServer,
};

enum class StatusState : uint8_t {
	Unloaded = 0,
	Connecting,
	Loading,
	Running,
	Paused,
	Stopping,
	Failed,
};

const char *status_role_label(StatusRole role);
const char *status_state_label(StatusState state);

struct GameStatusSnapshot {
	bool world = false;            // a mission is loaded
	uint64_t logic_tick = 0;
	StatusRole role = StatusRole::SinglePlayer;
	StatusState state = StatusState::Unloaded;
	bool playing = false;          // the runtime ticks (not paused)
	bool transport_locked = false; // a network role: pause and step are refused
	int32_t peers = 0;             // remote peers on the listen server's table
	double fps = 0.0;              // the engine's frames per second
	double frame_ms = 0.0;         // mean display frame over the push interval
	double frame_ms_peak = 0.0;    // worst display frame over the push interval
};

}  // namespace opennova::devtools
