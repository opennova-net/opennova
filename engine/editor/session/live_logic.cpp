#include <editor/session/live_logic.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include <base/io/file_io.h>
#include <base/io/hash.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_table.h>
#include <editor/model/document_base.h>
#include <editor/project/project_files.h>
#include <editor/run/run_directory.h>
#include <editor/session/document_set.h>
#include <editor/session/editor_request.h>
#include <editor/session/session_core.h>
#include <runtime/devtools/debug_control_ids.h>

namespace opennova::editor {

namespace {

using io::JsonValue;

// A whole number of the record, or `fallback`.
int64_t whole(const JsonValue &record, const char *key, int64_t fallback) {
	const JsonValue *value = record.get(key);
	return value && value->is_number() ? int64_t(std::llround(value->number)) : fallback;
}

// The mission's name as the game spells its basename: the file's name without its extension.
std::string mission_stem(const std::string &path) {
	std::string name = basename_of(path);
	const size_t dot = name.rfind('.');
	if (dot != std::string::npos) name.resize(dot);
	return name;
}

} // namespace

JsonValue play_debug_arguments(const PlayDebugCall &call) {
	JsonValue args = JsonValue::make_object();
	if (call.op == PlayDebugOp::FireEvent) {
		args.set("event", io::json_number(double(call.event)));
	} else {
		args.set("index", io::json_number(double(call.variable)));
		args.set("value", io::json_number(double(call.value)));
	}
	JsonValue out = JsonValue::make_object();
	out.set("op", io::json_string("invoke"));
	out.set("id", io::json_string(call.op == PlayDebugOp::FireEvent ? devtools::control_id::kFireEvent
	                                                                 : devtools::control_id::kSetMissionVariable));
	out.set("args", std::move(args));
	out.set("confirm_authority", JsonValue::make_bool(true));
	return out;
}

JsonValue live_snapshot_arguments() {
	JsonValue out = JsonValue::make_object();
	out.set("op", io::json_string("invoke"));
	out.set("id", io::json_string(devtools::control_id::kMissionLogicSnapshot));
	return out;
}

// A call in words ("fire event 3", "set V12 to 1").
static std::string play_debug_words(const PlayDebugCall &call) {
	if (call.op == PlayDebugOp::FireEvent) return "fire event " + std::to_string(call.event + 1);
	return "set V" + std::to_string(call.variable) + " to " + std::to_string(call.value);
}

// The game's mission_logic_snapshot record (the row's result, or the invoke's whole answer {error, result}) read
// into `out`: its tick, its events with their latch, countdowns, each trigger's last truth and a fire pending, its
// mission variables (those absent read 0); `game_mission` the game's mission by its file's basename, "" at its
// menu. A joiner's report (authority false) holds no rows. False, `error` why, for a record that is no snapshot.
// `out.mission`, `played_revision`, `stale` and `serial` are the session's and are left as they were.
static bool live_logic_from_snapshot(const JsonValue &snapshot, LiveLogic &out, std::string &game_mission, std::string &error) {
	// The invoke's whole answer carries the record as its result.
	const JsonValue *record = &snapshot;
	if (const JsonValue *result = snapshot.get("result"); result && result->is_object()) record = result;
	if (!record->is_object() || !record->get("events") || !record->get("events")->is_array()) {
		error = "The game's report is no mission logic snapshot (no events).";
		return false;
	}
	game_mission = record->get_string("mission", "");
	out.tick = whole(*record, "tick", 0);
	out.events.clear();
	out.variables.assign(size_t(kLiveVariableCount), 0);
	if (!record->get_bool("authority", true)) return true;
	for (const JsonValue &row : record->get("events")->array) {
		if (!row.is_object()) continue;
		LiveEvent event;
		const int64_t index = whole(row, "index", -1);
		if (index < 0) continue;
		event.index = size_t(index);
		event.active = row.get_bool("active", false);
		event.fired = row.get_bool("fired", false);
		event.activate_countdown = whole(row, "activate_countdown", 0);
		event.activate_reload = whole(row, "activate_reload", 0);
		event.repeat_countdown = whole(row, "repeat_countdown", 0);
		event.repeat_reload = whole(row, "repeat_reload", 0);
		if (const JsonValue *truth = row.get("triggers"); truth && truth->is_array())
			for (const JsonValue &term : truth->array)
				event.truth.push_back(int8_t(term.is_number() ? std::clamp(std::llround(term.number), -1LL, 1LL) : -1));
		event.chain = int8_t(std::clamp(whole(row, "chain", -1), int64_t(-1), int64_t(1)));
		event.last_checked_tick = whole(row, "last_checked_tick", -1);
		event.fire_pending = row.get_bool("fire_pending", false);
		out.events.push_back(std::move(event));
	}
	if (const JsonValue *variables = record->get("variables"); variables && variables->is_array())
		for (const JsonValue &variable : variables->array) {
			const int64_t index = whole(variable, "index", -1);
			if (index >= 0 && index < kLiveVariableCount) out.variables[size_t(index)] = int32_t(whole(variable, "value", 0));
		}
	return true;
}

LiveLogicSession::LiveLogicSession(SessionCore &core) : core_(core) {}

void LiveLogicSession::started(const std::string &mission, const std::string &mode) {
	ended();
	// The mission by its path in the project, as its document and the board name it (Play names it by its
	// logical name).
	const AssetEntry *entry = mission.empty() ? nullptr : core_.project_file(mission);
	mission_ = entry ? entry->relative_path : mission;
	mode_ = mode;
	if (mission_.empty()) return;
	// The file as the game loads it (Play saved it first), and the open document of it, its rows the played ones.
	std::vector<uint8_t> bytes;
	std::string error;
	played_fingerprint_ = io::read_file_bytes(core_.view().project.root + "/" + mission_, bytes, error)
	                              ? io::fnv1a64_bytes(io::kFnv1a64Offset, bytes.data(), bytes.size())
	                              : 0;
	if (const DocumentBase *document = core_.documents().document_for(mission_); document && !document->dirty())
		adopt(*document);
	follow();
}

void LiveLogicSession::adopt(const DocumentBase &document) {
	played_known_ = true;
	played_document_ = document.identity();
	played_load_ = document.load_generation();
	played_revision_ = document.revision();
	played_.clear();
	const auto *mission = dynamic_cast<const MissionDocument *>(records_of(document));
	if (!mission) return;
	for (const Node *row : mission->rows_of(MissionKind::Event)) {
		PlayedEvent event;
		event.row = row->id;
		const EventRow &chain = static_cast<const EventRow &>(*row);
		if (!chain.ids.lists.empty())
			for (size_t i = 0; i < chain.native.triggers.size() && i < chain.ids.lists[0].size(); ++i)
				event.triggers.push_back(chain.ids.lists[0][i].id);
		played_.push_back(std::move(event));
	}
}

void LiveLogicSession::ended() {
	const bool had = !core_.view().activity.live_logic.empty();
	mission_.clear();
	mode_.clear();
	played_fingerprint_ = 0;
	played_known_ = false;
	played_.clear();
	checked_open_ = false;
	stale_ = false;
	said_quiet_ = false;
	calls_.clear();
	core_.view().activity.live_logic = LiveLogic();
	if (had) core_.touch(ViewConcern::Run);
}

bool LiveLogicSession::reachable() const {
	const ActivityView &activity = core_.view().activity;
	return !mission_.empty() && mode_ == kRunModeRuntime && activity.play_state == PlayState::Running &&
	       activity.play_mcp_port != 0;
}

bool LiveLogicSession::polling() const {
	return reachable() && core_.documents().document_for(mission_) != nullptr &&
	       core_.view().workspace.document(mission_).logic;
}

bool LiveLogicSession::report(const JsonValue &snapshot, std::string &error) {
	if (!polling()) return true;
	LiveLogic &live = core_.view().activity.live_logic;
	LiveLogic read = live;
	std::string game_mission;
	if (!live_logic_from_snapshot(snapshot, read, game_mission, error)) return false;
	// A game in another mission than the one played (or at its menu) reports none of this one's logic.
	if (!strutil::iequals(game_mission, mission_stem(mission_))) {
		if (!live.empty()) {
			live = LiveLogic();
			core_.touch(ViewConcern::Run);
		}
		return true;
	}
	read.mission = mission_;
	read.quiet_ms = 0;
	read.serial = ++report_serial_;
	live = std::move(read);
	said_quiet_ = false;
	checked_open_ = false; // weighed again with the report
	follow();
	core_.touch(ViewConcern::Run);
	return true;
}

void LiveLogicSession::quiet(int64_t ms) {
	LiveLogic &live = core_.view().activity.live_logic;
	if (live.empty() || live.quiet_ms == ms) return;
	live.quiet_ms = ms;
	if (ms >= 2000 && !said_quiet_) {
		said_quiet_ = true;
		core_.note("The game has not answered the editor for 2 s: the board shows its last report until it does.");
	}
	core_.touch(ViewConcern::Run);
}

void LiveLogicSession::publish() {
	LiveLogic &live = core_.view().activity.live_logic;
	if (live.empty()) return;
	const auto same = [](const PlayedEvent &a, const PlayedEvent &b) { return a.row == b.row && a.triggers == b.triggers; };
	const bool changed = live.stale != stale_ || live.played_revision != played_revision_ ||
	                     live.played.size() != played_.size() ||
	                     !std::equal(played_.begin(), played_.end(), live.played.begin(), same);
	if (!changed) return;
	live.stale = stale_;
	live.played_revision = played_revision_;
	live.played = played_;
	core_.touch(ViewConcern::Run);
}

void LiveLogicSession::follow() {
	if (mission_.empty()) return;
	// Nothing asked of the game: its last report goes (the pane that showed it closed).
	LiveLogic &live = core_.view().activity.live_logic;
	if (!polling() && !live.empty()) {
		live = LiveLogic();
		core_.touch(ViewConcern::Run);
	}
	const DocumentBase *document = core_.documents().document_for(mission_);
	const uint64_t now[3] = {document ? document->identity() : 0, document ? document->load_generation() : 0,
	                         document ? document->revision() : 0};
	const bool dirty = document && document->dirty();
	if (checked_open_ && std::equal(now, now + 3, checked_) && dirty == checked_dirty_) return publish();
	std::copy(now, now + 3, checked_);
	checked_dirty_ = dirty;
	checked_open_ = true;
	if (!document) {
		stale_ = false; // the file is what played (Play saved it), whatever document reads it later
	} else if (played_known_ && document->identity() == played_document_ && document->load_generation() == played_load_) {
		stale_ = document->revision() != played_revision_;
	} else if (!dirty && played_fingerprint_ != 0 && document->file_fingerprint() == played_fingerprint_) {
		// Read since from the played bytes, unedited: the played one now, its rows recorded.
		adopt(*document);
		stale_ = false;
	} else {
		// Another document of the mission (edited, or read from other bytes): none of its rows are the played ones.
		played_known_ = false;
		played_.clear();
		stale_ = true;
	}
	publish();
}

void LiveLogicSession::serve(const EditorRequest &request) {
	const LiveLogic &live = core_.view().activity.live_logic;
	if (!reachable() || live.empty()) {
		core_.refuse_request(CoreFinding::PlayDebug,
		                     "No game Play runs reports this mission's logic: play_debug asks a game of the OpenNova "
		                     "runtime (play_mode runtime) started in a mission, once it reports (its Logic pane shown).");
		return;
	}
	const bool fire = request.event >= 0, set = request.variable >= 0;
	if (fire == set) {
		core_.refuse_request(CoreFinding::PlayDebug,
		                     "play_debug takes an event (to fire it) or a variable and its value (to set it), one of the two.");
		return;
	}
	PlayDebugCall call;
	if (fire) {
		const size_t index = size_t(request.event);
		const std::string number = std::to_string(request.event + 1);
		if (!live.event(index)) {
			core_.refuse_request(CoreFinding::PlayDebug, "The game reports no event " + number + " (" +
			                                                     std::to_string(live.events.size()) + " events).");
			return;
		}
		// The game's event of that index is the played one: refused where the document has removed or moved it
		// since, so what is fired is never another than the one the person means.
		const DocumentBase *open = core_.documents().document_for(mission_);
		const auto *mission = open ? dynamic_cast<const MissionDocument *>(records_of(*open)) : nullptr;
		if (mission && (stale_ || !played_.empty())) {
			const std::vector<const Node *> rows = mission->rows_of(MissionKind::Event);
			const NodeId played = index < played_.size() ? played_[index].row : 0;
			const bool held =
			        played != 0 && std::any_of(rows.begin(), rows.end(), [played](const Node *row) { return row->id == played; });
			if (!held || index >= rows.size() || rows[index]->id != played) {
				core_.refuse_request(CoreFinding::PlayDebug, "Event " + number + (held || played == 0 ? " moved" : " was removed") +
				                                                     " since Play started: Play again to fire it.");
				return;
			}
		}
		call.op = PlayDebugOp::FireEvent;
		call.event = request.event;
	} else {
		if (request.variable >= kLiveVariableCount || request.value < INT32_MIN || request.value > INT32_MAX) {
			core_.refuse_request(CoreFinding::PlayDebug, "A mission variable is V0 to V" +
			                                                     std::to_string(kLiveVariableCount - 1) +
			                                                     ", its value a 32-bit whole number.");
			return;
		}
		call.op = PlayDebugOp::SetVariable;
		call.variable = request.variable;
		call.value = request.value;
	}
	call.serial = next_serial_++;
	calls_.push_back(call);
	core_.view().activity.status = "Asked the game to " + play_debug_words(call) + ".";
	core_.touch(ViewConcern::Output);
}

std::vector<PlayDebugCall> LiveLogicSession::calls_since(uint64_t after) const {
	std::vector<PlayDebugCall> out;
	for (const PlayDebugCall &call : calls_)
		if (call.serial > after) out.push_back(call);
	return out;
}

void LiveLogicSession::answered(uint64_t serial, PlayDebugAnswer answer, const std::string &words) {
	const auto at = std::find_if(calls_.begin(), calls_.end(), [serial](const PlayDebugCall &c) { return c.serial == serial; });
	if (at == calls_.end()) return;
	const std::string what = play_debug_words(*at);
	const bool fire = at->op == PlayDebugOp::FireEvent;
	calls_.erase(at);
	std::string line;
	switch (answer) {
	case PlayDebugAnswer::Taken:
		line = "The game took the call to " + what + (fire ? ": its triggers read true at its next check." : ".");
		break;
	case PlayDebugAnswer::Refused:
		line = "The game refused to " + what + (words.empty() ? std::string(".") : ": " + words);
		break;
	case PlayDebugAnswer::NoAnswer:
		line = "The game did not answer the call to " + what + " within 3 s; it may still make it" +
		       (fire ? " (the board shows when it fires)." : ".");
		break;
	case PlayDebugAnswer::Gone:
		line = "The game went away before it answered the call to " + what + ".";
		break;
	}
	core_.note(line);
	core_.view().activity.status = line;
	core_.touch(ViewConcern::Output);
}

} // namespace opennova::editor
