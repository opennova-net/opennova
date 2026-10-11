#pragma once

// The running game's mission logic in the session (ADR 0046 S24, Live): what the game Play runs reports of its
// mission's events and variables (game_debug's mission_logic_snapshot row, runtime/mission/script_debug_report.h),
// read into the view (ActivityView::live_logic, documents/mission_live.h) for the board to draw; and the calls the
// editor asks of that game (play_debug: fire an event, set a variable), queued for the Shell, which speaks to the
// game's MCP endpoint (session/live_logic_link.h) and says what each came to. Only an OpenNova runtime game started
// in a mission reports: the game install runs no MCP endpoint (run/launch_plan.h).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/mission_live.h>

namespace opennova::editor {

class SessionCore;
struct EditorRequest;

// What a play_debug call asks the game: one event's chain to read true at its next check (game_debug's
// fire_event), or a mission variable set (its set_mission_variable).
enum class PlayDebugOp : uint8_t { FireEvent, SetVariable };

// One call the Shell makes of the running game, numbered in the order asked (from 1).
struct PlayDebugCall {
	uint64_t serial = 0;
	PlayDebugOp op = PlayDebugOp::FireEvent;
	int64_t event = -1;    // FireEvent: the event by its index (0 the first)
	int64_t variable = -1; // SetVariable: the variable, V0..V511 (the bank the game's row takes)
	int64_t value = 0;     // SetVariable: its value, a 32-bit word
};

// The game's banks the calls reach: its mission variables V0..V511 [runtime/world/var_store.h].
inline constexpr int64_t kLiveVariableCount = 512;

// The game_debug tool's arguments for a call (an invoke of its row, authority confirmed: the editor's own game), and
// for the snapshot read (an invoke of mission_logic_snapshot, which needs none).
io::JsonValue play_debug_arguments(const PlayDebugCall &call);
io::JsonValue live_snapshot_arguments();
// A call in words ("fire event 3", "set V12 to 1").
std::string play_debug_words(const PlayDebugCall &call);

// The game's mission_logic_snapshot record (the row's result, or the invoke's whole answer {error, result}) read
// into `out`: its tick, its events with their latch, countdowns, each trigger's last truth and a fire pending, its
// mission variables (those absent read 0); `game_mission` the game's mission by its file's basename, "" at its
// menu. A joiner's report (authority false) holds no rows. False, `error` why, for a record that is no snapshot.
// `out.mission`, `played_revision`, `stale` and `serial` are the session's and are left as they were.
bool live_logic_from_snapshot(const io::JsonValue &snapshot, LiveLogic &out, std::string &game_mission,
                              std::string &error);

// The live logic of the game Play runs: what it last reported, the document revision it was played at, and the
// calls asked of it. Owned by Play (PlayController), which tells it when its game starts and ends.
class LiveLogicSession {
public:
	explicit LiveLogicSession(SessionCore &core);
	LiveLogicSession(const LiveLogicSession &) = delete;
	LiveLogicSession &operator=(const LiveLogicSession &) = delete;

	// Play started a game in `mission` (the project's spelling of its .bms; "" its menu) in run mode `mode`
	// (kRunMode*): the mission's open document's state is the one played (Play saves it first).
	void started(const std::string &mission, const std::string &mode);
	// The game ended, or the project closed: its logic and its calls are gone.
	void ended();
	// Whether the Shell asks the game for its logic: a game of the OpenNova runtime runs, with its MCP port, in
	// a mission.
	bool polling() const;
	// The Shell's: what the game reported (`text`, the snapshot's JSON); the view's live logic made from it when
	// it is the played mission's (a game at its menu, or in another mission, reports none). False, `error` why,
	// for a text that does not read; one that arrives while no game reports is passed over.
	bool report(const std::string &text, std::string &error);
	// The poll's: whether the played mission's document has moved on since it was played (`stale`).
	void follow();
	// play_debug: the call the request asks queued for the Shell; refused (play.debug) while no game reports, for
	// an event past the game's events, a variable past V511 or a value past 32 bits, and for neither or both of
	// an event and a variable.
	void serve(const EditorRequest &request);
	// The calls asked after `after`, oldest first.
	std::vector<PlayDebugCall> calls_since(uint64_t after) const;
	// The Shell's: what the game made of call `serial` (`ok`, else its words why not), said in Output.
	void answered(uint64_t serial, bool ok, const std::string &words);

private:
	SessionCore &core_;
	std::string mission_; // the played mission by its project-relative path, its document's ("" none)
	std::string mode_;
	// The played document: its identity, load and revision when it was played (known: it was open then, or
	// opened unedited since).
	bool played_known_ = false;
	uint64_t played_document_ = 0, played_load_ = 0, played_revision_ = 0;
	std::vector<PlayDebugCall> calls_;
	uint64_t next_serial_ = 1;
};

} // namespace opennova::editor
