#pragma once

// A running game's mission logic as the editor holds it (ADR 0046 S24, Live): what the game played from the
// runtime's debug report (runtime/mission/script_debug_report.h, one engine function per fact) reported last,
// each event's latch, countdowns and its triggers' last truth, and the mission variables. The session fills it
// while Play runs the runtime (session/live_logic, lane C) and the board draws it on its cards
// (ui/mission_board_live); empty (no mission) otherwise. What a count means is the game's: an event's
// countdowns are in 64-tick steps and count down a quarter pass at a time [docs/mission/bms-event-runtime-re.md
// 1.6].

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::editor {

// One event of the mission as the game reported it.
struct LiveEvent {
	// Its index in the event table (0 the first), the board's card's.
	size_t index = 0;
	// Whether its triggers held and it waits on its delay (active), and whether it acted (fired: active and its
	// delay run out, what an Event trigger reads).
	bool active = false;
	bool fired = false;
	// Its countdowns and their reloads, in steps as the game holds them.
	int64_t activate_countdown = 0;
	int64_t activate_reload = 0;
	int64_t repeat_countdown = 0;
	int64_t repeat_reload = 0;
	// Each trigger's last result as the chain ran, by its place in the event: 1 true, 0 false, -1 not checked yet
	// (recorded as the chain runs, never by evaluating again).
	std::vector<int8_t> truth;
	// The tick the game last checked its triggers at (-1: never), and what the chain came to then (1 true, 0
	// false, -1 not run yet).
	int64_t last_checked_tick = -1;
	int8_t chain = -1;
	// A fire asked of it (play_debug) waits for its next check.
	bool fire_pending = false;
};

// The mission logic of the game Play runs, as last reported.
struct LiveLogic {
	// The mission played, as the project spells its file ("" none: no game, or one the editor reads no logic
	// of), and the document's revision it was played at (a document moved on since reads `stale`).
	std::string mission;
	uint64_t played_revision = 0;
	// The game's clock at the report, in ticks.
	int64_t tick = 0;
	std::vector<LiveEvent> events;
	// The mission variables V0.. as the game holds them, raw 32-bit.
	std::vector<int32_t> variables;
	// Whether the document has changed since it was played (its events may no longer be the game's).
	bool stale = false;
	// Moves with each report taken (a window draws a new one once).
	uint64_t serial = 0;

	bool empty() const { return mission.empty(); }
	// The event of an index the report holds, or null.
	const LiveEvent *event(size_t index) const {
		for (const LiveEvent &event : events)
			if (event.index == index) return &event;
		return nullptr;
	}
};

} // namespace opennova::editor
