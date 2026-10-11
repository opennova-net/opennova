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

#include <editor/model/value.h>

namespace opennova::editor {

// One event of the mission as the game reported it.
struct LiveEvent {
	// Its index in the event table (0 the first), the board's card's.
	size_t index = 0;
	// Whether its triggers held and it waits on its delay (active), and whether it acted (fired: active and its
	// delay run out, what an Event trigger reads).
	bool active = false;
	bool fired = false;
	// Its countdowns and their reloads, in ticks as the game holds them (64 a step, one step taken a quarter pass
	// of 64 ticks: about the seconds they last at 62.5 ticks a second).
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

// One event of the mission as it was played: its row, and its triggers' records in their order, in the document
// that held what the game loaded (at Play's start, or a document of the played bytes read since).
struct PlayedEvent {
	NodeId row = 0;
	std::vector<NodeId> triggers;
};

// The mission logic of the game Play runs, as last reported.
struct LiveLogic {
	// The mission played, by its project-relative path, its document's ("" none: no game, or one the editor
	// reads no logic of), and the document's revision it was played at (a document moved on since reads `stale`).
	std::string mission;
	uint64_t played_revision = 0;
	// The played events by the game's index (empty: no document of the played bytes was open to say which rows
	// they are). A card is the game's event only where its row is the played one at the same place: an event
	// removed, added before or moved since shows none of the game's state.
	std::vector<PlayedEvent> played;
	// The game's clock at the report, in ticks.
	int64_t tick = 0;
	std::vector<LiveEvent> events;
	// The mission variables V0.. as the game holds them, raw 32-bit.
	std::vector<int32_t> variables;
	// Whether the document has changed since it was played (its events may no longer be the game's).
	bool stale = false;
	// How long the game has not answered the editor's asks, in milliseconds (0: it answers): the report is the
	// last it gave.
	int64_t quiet_ms = 0;
	// Moves with each report taken, never back, across Plays (a window draws a new one once).
	uint64_t serial = 0;

	bool empty() const { return mission.empty(); }
	// The event of an index the report holds, or null.
	const LiveEvent *event(size_t index) const {
		for (const LiveEvent &event : events)
			if (event.index == index) return &event;
		return nullptr;
	}
	// The game's report of the event at place `index` now whose row is `row`: its event of that index where the
	// played event there is that row, else null (the played rows unknown, or the event moved or removed since).
	const LiveEvent *event_of(NodeId row, size_t index) const {
		return index < played.size() && played[index].row == row ? event(index) : nullptr;
	}
	// The game's last truth of the trigger at place `place` of that event whose record is `trigger`: 1, 0, -1 not
	// checked yet; -2 where it is not the played trigger at that place (added, moved or removed since).
	int trigger_truth(NodeId row, size_t index, NodeId trigger, size_t place) const {
		const LiveEvent *reported = event_of(row, index);
		if (!reported || place >= played[index].triggers.size() || played[index].triggers[place] != trigger) return -2;
		return place < reported->truth.size() ? reported->truth[place] : -1;
	}
};

} // namespace opennova::editor
