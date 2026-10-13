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
#include <runtime/world/var_store.h>

namespace opennova::editor {

class DocumentBase;
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

// The game's banks the calls reach: its mission variables V0..V511, the runtime's bank.
inline constexpr int64_t kLiveVariableCount = world::ScriptVarStore::kMissionVars;

// What became of a call the Shell made of the game: it took it, it refused it (its words), it did not answer in
// time (it may still make it: its tools run one at a time, behind another client's), or the stream to it closed
// before it answered.
enum class PlayDebugAnswer : uint8_t { Taken, Refused, NoAnswer, Gone };

// The game_debug tool's arguments for a call (an invoke of its row, authority confirmed: the editor's own game), and
// for the snapshot read (an invoke of mission_logic_snapshot, which needs none).
io::JsonValue play_debug_arguments(const PlayDebugCall &call);
io::JsonValue live_snapshot_arguments();

// The live logic of the game Play runs: what it last reported, the document state it was played at, and the
// calls asked of it. Owned by Play (PlayController), which tells it when its game starts and ends.
//
// The played state is recorded as Play starts the game (Play saves first, so the file is what the game loads):
// the file's fingerprint, and from the mission's document when one is open its events' rows and their triggers'
// records. A document of the mission read since (opened, or read again) is the played one while it is clean and
// its file is the played bytes, its rows then recorded; any other is stale and draws none of the game's state.
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
	// Whether a game the editor can ask runs: one of the OpenNova runtime, with its MCP port, in a mission.
	bool reachable() const;
	// Whether the Shell asks that game for its logic: while the played mission's Logic pane is shown (its
	// document open, the workspace's `logic` on). Otherwise nothing is asked (and the last report goes): the
	// game builds a whole snapshot each time it is asked, on its main thread.
	bool polling() const;
	// The Shell's: what the game reported (the snapshot's record); the view's live logic made from it when it is
	// the played mission's (a game at its menu, or in another mission, reports none). False, `error` why, for a
	// record that is no snapshot; one that arrives while nothing is polled is passed over.
	bool report(const io::JsonValue &snapshot, std::string &error);
	// The Shell's: how long the game has not answered (0: it answers again). The first time it falls silent past
	// two seconds Output says so; the board says how long.
	void quiet(int64_t ms);
	// The poll's: whether the played mission's document has moved on since it was played (`stale`), and the
	// played rows of a document of the played bytes read since. The last report goes once nothing is polled.
	void follow();
	// play_debug: the call the request asks queued for the Shell; refused (play.debug) while no game reports, for
	// an event past the game's events or one removed or moved since Play started, a variable past V511 or a value
	// past 32 bits, and for neither or both of an event and a variable.
	void serve(const EditorRequest &request);
	// The calls asked after `after`, oldest first.
	std::vector<PlayDebugCall> calls_since(uint64_t after) const;
	// The Shell's: what became of call `serial` (`words`: the game's refusal), said in Output.
	void answered(uint64_t serial, PlayDebugAnswer answer, const std::string &words);

private:
	// `document` (clean, its file the played bytes) as the played one: its identity, load and revision, and its
	// events' rows.
	void adopt(const DocumentBase &document);
	void publish();

	SessionCore &core_;
	std::string mission_; // the played mission by its project-relative path, its document's ("" none)
	std::string mode_;
	uint64_t played_fingerprint_ = 0; // the file as Play started the game on it
	// The played document: its identity, load and revision (known: it was open then, or one of the played bytes
	// was read since), and its events' rows by the game's index.
	bool played_known_ = false;
	uint64_t played_document_ = 0, played_load_ = 0, played_revision_ = 0;
	std::vector<PlayedEvent> played_;
	// What follow() last weighed (the document's identity, load, revision, whether dirty) and its verdict.
	uint64_t checked_[3] = {};
	bool checked_dirty_ = false, checked_open_ = false, stale_ = false;
	bool said_quiet_ = false;
	std::vector<PlayDebugCall> calls_;
	uint64_t next_serial_ = 1;
	uint64_t report_serial_ = 0; // never back: a board keyed on it never meets an old one again
};

} // namespace opennova::editor
