#include <editor/session/live_logic.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/model/document_base.h>
#include <editor/project/project_files.h>
#include <editor/run/run_directory.h>
#include <editor/session/document_set.h>
#include <editor/session/editor_request.h>
#include <editor/session/session_core.h>

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
	out.set("id", io::json_string(call.op == PlayDebugOp::FireEvent ? "fire_event" : "set_mission_variable"));
	out.set("args", std::move(args));
	out.set("confirm_authority", JsonValue::make_bool(true));
	return out;
}

JsonValue live_snapshot_arguments() {
	JsonValue out = JsonValue::make_object();
	out.set("op", io::json_string("invoke"));
	out.set("id", io::json_string("mission_logic_snapshot"));
	return out;
}

std::string play_debug_words(const PlayDebugCall &call) {
	if (call.op == PlayDebugOp::FireEvent) return "fire event " + std::to_string(call.event + 1);
	return "set V" + std::to_string(call.variable) + " to " + std::to_string(call.value);
}

bool live_logic_from_snapshot(const JsonValue &snapshot, LiveLogic &out, std::string &game_mission, std::string &error) {
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
	played_known_ = false;
	if (mission_.empty()) return;
	if (const DocumentBase *document = core_.documents().document_for(mission_)) {
		played_known_ = true;
		played_document_ = document->identity();
		played_load_ = document->load_generation();
		played_revision_ = document->revision();
	}
}

void LiveLogicSession::ended() {
	const bool had = !core_.view().activity.live_logic.empty();
	mission_.clear();
	mode_.clear();
	played_known_ = false;
	calls_.clear();
	core_.view().activity.live_logic = LiveLogic();
	if (had) core_.touch(ViewConcern::Run);
}

bool LiveLogicSession::polling() const {
	const ActivityView &activity = core_.view().activity;
	return !mission_.empty() && mode_ == kRunModeRuntime && activity.play_state == PlayState::Running &&
	       activity.play_mcp_port != 0;
}

bool LiveLogicSession::report(const std::string &text, std::string &error) {
	if (!polling()) return true;
	JsonValue snapshot;
	if (!io::json_parse(text, snapshot, error)) {
		error = "The game's report does not read: " + error;
		return false;
	}
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
	read.played_revision = played_revision_;
	read.serial = live.serial + 1;
	live = std::move(read);
	follow();
	core_.touch(ViewConcern::Run);
	return true;
}

void LiveLogicSession::follow() {
	LiveLogic &live = core_.view().activity.live_logic;
	if (live.empty()) return;
	const DocumentBase *document = core_.documents().document_for(mission_);
	bool stale = live.stale;
	if (!document) {
		stale = false; // the file on disk is what played (Play saved it)
	} else if (!played_known_) {
		// Opened since it was played: what it read then is what played, until it is edited.
		if (!document->dirty()) {
			played_known_ = true;
			played_document_ = document->identity();
			played_load_ = document->load_generation();
			played_revision_ = document->revision();
			live.played_revision = played_revision_;
		}
		stale = document->dirty();
	} else {
		stale = document->identity() != played_document_ || document->load_generation() != played_load_ ||
		        document->revision() != played_revision_;
	}
	if (stale != live.stale) {
		live.stale = stale;
		core_.touch(ViewConcern::Run);
	}
}

void LiveLogicSession::serve(const EditorRequest &request) {
	const LiveLogic &live = core_.view().activity.live_logic;
	if (!polling() || live.empty()) {
		core_.refuse_request(CoreFinding::PlayDebug,
		                     "No game Play runs reports this mission's logic: play_debug asks a game of the OpenNova "
		                     "runtime (play_mode runtime) started in a mission, once it reports.");
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
		if (!live.event(size_t(request.event))) {
			core_.refuse_request(CoreFinding::PlayDebug, "The game reports no event " + std::to_string(request.event + 1) +
			                                                     " (" + std::to_string(live.events.size()) + " events).");
			return;
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

void LiveLogicSession::answered(uint64_t serial, bool ok, const std::string &words) {
	const auto at = std::find_if(calls_.begin(), calls_.end(), [serial](const PlayDebugCall &c) { return c.serial == serial; });
	if (at == calls_.end()) return;
	const std::string what = play_debug_words(*at);
	const bool fire = at->op == PlayDebugOp::FireEvent;
	calls_.erase(at);
	const std::string line = ok ? "The game took the call to " + what +
	                                      (fire ? ": its triggers read true at its next check." : ".")
	                            : "The game refused to " + what + (words.empty() ? std::string(".") : ": " + words);
	core_.note(line);
	core_.view().activity.status = line;
	core_.touch(ViewConcern::Output);
}

} // namespace opennova::editor
