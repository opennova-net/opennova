// A mission's names (ADR 0046 S24, Names and Live). S0: a mission variable's base words are the script's name for
// it ("V12"), a trigger and an action reading a variable word it through MissionNames::variable (a name the
// mission gives it overriding them); and the live logic the session holds while Play runs is empty until a game
// reports (documents/mission_live.h), an event found by its index. C2/C3 over the minted synth_logic mission: a
// group named in its record's dropped bytes and a variable named in the tagged layer table, as the document's
// words, labels and values say them; the edits that name a variable (one batch), the 31 the table takes, a table
// of the original editor's layer names never written over, and a tagged table's records never edited as layer
// names (read-only, refused).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/documents/mission_live.h>
#include <editor/documents/mission_sentence.h>
#include <editor/documents/mission_variable_names.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/reference_queries.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/mission_ground_facts.h>
#include <base/io/hash.h>
#include <base/io/json.h>
#include <base/io/strutil.h>
#include <base/vfs/file_source.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/live_logic.h>
#include <editor/session/live_logic_link.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_json.h>
#include <editor/session/view_json.h>
#include <editor/session/view/activity_view.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_mis.h>
#include <formats/trn/trn_io.h>
#include <runtime/world/collision.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace bms = opennova::bms;
namespace mission = opennova::mission;
using opennova::io::JsonValue;

namespace {

// A mission's names with V3 named.
class NamedVariables final : public MissionNames {
public:
	std::string variable(int64_t index) const override { return index == 3 ? "alarm_raised" : MissionNames::variable(index); }
};

bms::Trigger variable_trigger(int32_t compare, int32_t variable, int32_t value) {
	bms::Trigger trigger{};
	trigger.main_type = bms::TriggerMainType::MissionVariable;
	trigger.sub_type = compare;
	trigger.param1 = variable;
	trigger.param2 = value;
	return trigger;
}

bms::Action variable_action(int32_t change, int32_t variable, int32_t value) {
	bms::Action action{};
	action.action_type = bms::ActionType::MisvarChange;
	action.action_sub_type = change;
	action.param1 = variable;
	action.param2 = value;
	return action;
}

int test_variable_words() {
	const MissionNames base;
	TEST_EXPECT(base.variable(12) == "V12" && base.variable(0) == "V0");
	TEST_EXPECT(trigger_words(variable_trigger(1, 3, 2), base) == "V3 equals 2");
	TEST_EXPECT(action_words(variable_action(1, 3, 1), base, nullptr) == "set V3 to 1");
	const NamedVariables named;
	TEST_EXPECT(trigger_words(variable_trigger(1, 3, 2), named) == "alarm_raised equals 2");
	TEST_EXPECT(action_words(variable_action(2, 3, 5), named, nullptr) == "add 5 to alarm_raised");
	TEST_EXPECT(action_words(variable_action(2, 4, 5), named, nullptr) == "add 5 to V4");
	return 0;
}

int test_live_logic() {
	const ActivityView activity;
	TEST_EXPECT(activity.live_logic.empty() && activity.live_logic.events.empty() && activity.live_logic.serial == 0);
	LiveLogic live;
	live.mission = "missions/logic.bms";
	LiveEvent event;
	event.index = 4;
	event.truth = {1, 0, -1};
	live.events.push_back(event);
	TEST_EXPECT(!live.empty() && live.event(4) && live.event(4)->truth.size() == 3 && !live.event(0));
	TEST_EXPECT(live.event(4)->last_checked_tick == -1 && !live.event(4)->active && !live.event(4)->fired);
	return 0;
}

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

bool same(const std::string &got, const std::string &want) {
	if (got != want) std::printf("  got:  %s\n  want: %s\n", got.c_str(), want.c_str());
	return got == want;
}

std::unique_ptr<Document> open_mission() {
	const std::vector<uint8_t> bytes =
	        test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_logic.bms");
	const DocumentType *type = document_type_for(AssetKind::Mission);
	if (!type) return nullptr;
	std::unique_ptr<Document> document = records_of(type->make());
	Diagnostic error;
	if (!document || !document->load_bytes(bytes, "synth_logic.bms", AssetKind::Mission, "jo", error)) return nullptr;
	return document;
}

// The mission row's records of `kind` (its Groups, its Layers), by their identities in order.
std::vector<NodeAddress> listed(const MissionDocument &m, MissionKind kind) {
	std::vector<NodeAddress> out;
	const MissionRow *row = m.mission_row();
	for (const Document::Collection &list : m.collections_of({row->id, row->kind, 0}))
		if (list.spec.kind == k(kind))
			for (const NodeId id : list.ids) out.push_back({row->id, k(kind), id});
	return out;
}

bool apply_edits(Document &document, const std::vector<Edit> &edits) {
	Diagnostic error;
	const bool ok = document.apply(edits, error);
	if (!ok) std::printf("  refused: %s\n", error.message.c_str());
	return ok;
}

Edit set_of(const NodeAddress &address, const std::string &field, const std::string &value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = value;
	return edit;
}

int test_group_names() {
	std::unique_ptr<Document> document = open_mission();
	TEST_EXPECT(document != nullptr);
	const MissionDocument &m = dynamic_cast<const MissionDocument &>(*document);
	const std::vector<NodeAddress> groups = listed(m, MissionKind::Group);
	TEST_EXPECT(groups.size() == bms::kGroupRecordCount);
	TEST_EXPECT(same(DocumentMissionNames(m).group(4), "group 4"));
	// A name, an edit of the group's record (its dropped bytes), as any field is.
	TEST_EXPECT(apply_edits(*document, {set_of(groups[4], "name", "Bridge patrol")}));
	const DocumentMissionNames names(m);
	TEST_EXPECT(same(names.group(4), "Bridge patrol") && same(names.group(5), "group 5") && same(names.group(0), "no group"));
	TEST_EXPECT(mission_record_label(m, groups[4], nullptr).rfind("Group 4: Bridge patrol (", 0) == 0);
	TEST_EXPECT(bms::group_name(m.mission_row()->native.group_records[4]) == "Bridge patrol");
	// What the format refuses (a quote) the edit does, the name as it was.
	TEST_EXPECT(!apply_edits(*document, {set_of(groups[4], "name", "Say \"hi\"")}));
	TEST_EXPECT(same(DocumentMissionNames(m).group(4), "Bridge patrol"));
	return 0;
}

// A mission's events by their index live in their own section of its scope (a script's event()): a lookup naming
// the section finds the event, one of the mission's scope alone (or of none) does not, the zone ids and SSNs the
// same numbers are kept apart (reference_queries' find_definition, the editor MCP's record by symbol).
int test_event_section() {
	std::unique_ptr<Document> document = open_mission();
	TEST_EXPECT(document != nullptr);
	const MissionDocument &m = dynamic_cast<const MissionDocument &>(*document);
	const std::vector<const Node *> events = m.rows_of(MissionKind::Event);
	TEST_EXPECT(events.size() == 2 && mission_event_scope(m.path()) == "SYNTH_LOGIC.BMS/EVENTS");
	NodeAddress found;
	TEST_EXPECT(find_definition(AssetGraph(), m, "1", found, "SYNTH_LOGIC.BMS/EVENTS") && found.row == events[1]->id);
	// The mission's scope alone: the entity of SSN 1 the fixture holds, never the event.
	TEST_EXPECT(find_definition(AssetGraph(), m, "1", found, "SYNTH_LOGIC.BMS") && found.kind != k(MissionKind::Event));
	TEST_EXPECT(!find_definition(AssetGraph(), m, "0", found, "SYNTH_LOGIC.BMS") || found.kind != k(MissionKind::Event));
	return 0;
}

int test_variable_names() {
	std::unique_ptr<Document> document = open_mission();
	TEST_EXPECT(document != nullptr);
	const MissionDocument &m = dynamic_cast<const MissionDocument &>(*document);
	TEST_EXPECT(mission_variable_table(m) == mission::LayerTable::Empty && mission_variable_names_left(m) == 31);
	std::vector<Edit> edits;
	std::string error;
	// V12 named: the tag and one record, one batch.
	TEST_EXPECT(mission_variable_name_edits(m, 12, "alarm_raised", edits, error) && edits.size() == 2);
	TEST_EXPECT(apply_edits(*document, edits));
	TEST_EXPECT(mission_variable_table(m) == mission::LayerTable::VariableNames && mission_variable_names_left(m) == 30);
	TEST_EXPECT(same(mission_variable_name(m, 12), "alarm_raised") && mission_variable_name(m, 3).empty());
	{
		const DocumentMissionNames names(m);
		TEST_EXPECT(same(names.variable(12), "alarm_raised") && same(names.variable(3), "V3"));
		TEST_EXPECT(same(trigger_words(variable_trigger(1, 12, 2), names), "alarm_raised equals 2"));
	}
	// The table reads as the names, its records not editable as layer names.
	const std::vector<NodeAddress> layers = listed(m, MissionKind::Layer);
	TEST_EXPECT(layers.size() == bms::kLayerRecordCount);
	TEST_EXPECT(same(mission_record_label(m, layers[0], nullptr), "Variable names (the editor's)"));
	TEST_EXPECT(same(mission_record_label(m, layers[1], nullptr), "Variable 12: alarm_raised"));
	TEST_EXPECT(same(mission_record_label(m, layers[2], nullptr), "No variable name"));
	for (const FieldSchema &schema : m.fields(k(MissionKind::Layer)))
		if (schema.id == "name") TEST_EXPECT(m.field_on(layers[1], schema).read_only);
	TEST_EXPECT(!apply_edits(*document, {set_of(layers[0], "name", "Base layer")})); // the tag
	TEST_EXPECT(!apply_edits(*document, {set_of(layers[1], "name", "Patrols")}));    // a layer name among the names
	TEST_EXPECT(same(mission_variable_name(m, 12), "alarm_raised"));
	// A second name sorts by index; a rename changes one record; nothing changed plans nothing.
	TEST_EXPECT(mission_variable_name_edits(m, 3, "bridge blown", edits, error) && apply_edits(*document, edits));
	TEST_EXPECT(same(mission_record_label(m, layers[1], nullptr), "Variable 3: bridge blown") &&
	            same(mission_record_label(m, layers[2], nullptr), "Variable 12: alarm_raised"));
	TEST_EXPECT(mission_variable_name_edits(m, 12, "alarm", edits, error) && edits.size() == 1 && apply_edits(*document, edits));
	TEST_EXPECT(mission_variable_name_edits(m, 12, "alarm", edits, error) && edits.empty());
	// What the format refuses: past V255 (the script's own VAR words), a name too long for its record.
	TEST_EXPECT(!mission_variable_name_edits(m, 256, "x", edits, error) && error.find("V255") != std::string::npos);
	TEST_EXPECT(!mission_variable_name_edits(m, 4, "eighteen chars....", edits, error));
	// The 31 the table takes: a 32nd refused.
	for (int i = 100; i < 129 && mission_variable_names_left(m) > 0; ++i)
		TEST_EXPECT(mission_variable_name_edits(m, i, "v" + std::to_string(i), edits, error) && apply_edits(*document, edits));
	TEST_EXPECT(mission_variable_names_left(m) == 0 && mission_variable_names(m).size() == 31);
	TEST_EXPECT(!mission_variable_name_edits(m, 200, "one more", edits, error));
	// Every name taken away empties the table again, as shipped.
	for (const mission::VariableName &name : mission_variable_names(m))
		TEST_EXPECT(mission_variable_name_edits(m, name.index, "", edits, error) && apply_edits(*document, edits));
	TEST_EXPECT(mission_variable_table(m) == mission::LayerTable::Empty && mission_variable_names_left(m) == 31);

	// A table of the original editor's layer names: edited as layer names, never written over by a name.
	TEST_EXPECT(apply_edits(*document, {set_of(layers[0], "name", "Base layer")}));
	TEST_EXPECT(mission_variable_table(m) == mission::LayerTable::LayerNames && mission_variable_names_left(m) == 0);
	for (const FieldSchema &schema : m.fields(k(MissionKind::Layer)))
		if (schema.id == "name") TEST_EXPECT(!m.field_on(layers[1], schema).read_only);
	TEST_EXPECT(!mission_variable_name_edits(m, 12, "alarm", edits, error) && error.find("layer names") != std::string::npos);
	TEST_EXPECT(same(mission_record_label(m, layers[0], nullptr), m.record_name(layers[0])));
	return 0;
}

// The variables op of mission_logic over a session: the names listed with the table and how many are left; the
// edits naming one, passed back through edit_record as they are; a refusal answered in words.
int test_variables_wire() {
	editor_test::TempProjectDir dir("opennova_mission_names_wire");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Names"));
	editor_test::create_missing_files(session);
	const std::vector<uint8_t> bytes =
	        test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_logic.bms");
	TEST_EXPECT(editor_test::write_bytes(session.view().project.root + "/missions/logic.bms", bytes));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("missions/logic.bms"));
	const auto ask = [&](const std::string &args) {
		JsonValue parsed;
		std::string error;
		opennova::io::json_parse(args, parsed, error);
		JsonValue answer = session.query("mission_logic", parsed, error);
		if (!error.empty()) std::printf("  refused: %s\n", error.c_str());
		return answer;
	};
	JsonValue listed = ask("{\"path\": \"missions/logic.bms\", \"op\": \"variables\"}");
	TEST_EXPECT(listed.get_string("table", "") == "empty" && listed.get_number("left", 0) == 31 &&
	            listed.get("names") && listed.get("names")->array.empty() && !listed.get("edits"));
	JsonValue planned = ask("{\"path\": \"missions/logic.bms\", \"op\": \"variables\", \"variable\": 12, \"name\": \"alarm_raised\"}");
	const JsonValue *edits = planned.get("edits");
	TEST_EXPECT(edits && edits->array.size() == 2 && !planned.get("refusal"));
	if (!edits) return 1;
	JsonValue request = JsonValue::make_object();
	request.set("kind", opennova::io::json_string("edit_record"));
	request.set("path", opennova::io::json_string("missions/logic.bms"));
	request.set("edits", *edits);
	TEST_EXPECT(session.handle_json(request).get_bool("ok", false));
	session.run_operations();
	listed = ask("{\"path\": \"missions/logic.bms\", \"op\": \"variables\"}");
	const JsonValue *names = listed.get("names");
	TEST_EXPECT(listed.get_string("table", "") == "names" && listed.get_number("left", 0) == 30 && names &&
	            names->array.size() == 1 && names->array[0].get_number("index", -1) == 12 &&
	            names->array[0].get_string("name", "") == "alarm_raised");
	// One Undo takes the name back whole.
	editor_test::handle_to_end(session, request::undo());
	TEST_EXPECT(ask("{\"path\": \"missions/logic.bms\", \"op\": \"variables\"}").get_string("table", "") == "empty");
	const JsonValue refused = ask("{\"path\": \"missions/logic.bms\", \"op\": \"variables\", \"variable\": 300, \"name\": \"x\"}");
	TEST_EXPECT(refused.get_string("refusal", "").find("V255") != std::string::npos && refused.get("edits") &&
	            refused.get("edits")->array.empty());
	return 0;
}

// A byte stream that answers as the game's MCP endpoint does: the requests it was sent, its answers queued.
struct FakeChannel : GameMcpChannel {
	State now = State::Closed;
	int port = 0;
	int connects = 0;
	std::vector<std::string> sent;
	std::string inbox;
	void connect(int p) override {
		port = p;
		++connects;
		now = State::Connected;
	}
	State state() override { return now; }
	bool send(const std::string &bytes) override {
		sent.push_back(bytes);
		return true;
	}
	std::string receive() override {
		std::string out;
		out.swap(inbox);
		return out;
	}
	void close() override { now = State::Closed; }
	// An HTTP answer of `body` (with the endpoint's session id).
	void answer(const std::string &body, const char *session = nullptr) {
		inbox += "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
		if (session) inbox += std::string("Mcp-Session-Id: ") + session + "\r\n";
		inbox += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: keep-alive\r\n\r\n" + body;
	}
	// The JSON-RPC body of the last request sent.
	JsonValue last_body() const {
		JsonValue out;
		std::string error;
		if (!sent.empty()) opennova::io::json_parse(sent.back().substr(sent.back().find("\r\n\r\n") + 4), out, error);
		return out;
	}
};

// A tools/call answer whose tool result is `structured`.
std::string tool_answer(const JsonValue &structured, bool error = false, const std::string &words = std::string()) {
	JsonValue text = JsonValue::make_object();
	text.set("type", opennova::io::json_string("text"));
	text.set("text", opennova::io::json_string(error ? words : opennova::io::json_write(structured)));
	JsonValue content = JsonValue::make_array();
	content.push(std::move(text));
	JsonValue result = JsonValue::make_object();
	result.set("content", std::move(content));
	result.set("isError", JsonValue::make_bool(error));
	if (!error) result.set("structuredContent", structured);
	JsonValue out = JsonValue::make_object();
	out.set("jsonrpc", opennova::io::json_string("2.0"));
	out.set("id", opennova::io::json_number(1));
	out.set("result", std::move(result));
	return opennova::io::json_write(out);
}

JsonValue parsed(const std::string &text) {
	JsonValue out;
	std::string error;
	opennova::io::json_parse(text, out, error);
	return out;
}

// The game's mission_logic_snapshot answer (the invoke's {error, result}) for mission `mission` at `tick`: event 0
// latched waiting on its delay, its trigger held; event 1 not checked, a fire asked; V12 = 7.
JsonValue snapshot_of(const char *mission, int tick = 640) {
	return parsed(std::string(R"({"error": 0, "result": {"authority": true, "mission": ")") + mission +
	              R"(", "tick": )" + std::to_string(tick) + R"(, "events": [{"index": 0, "active": true, "fired": false, "activate_countdown": 192, )"
	              R"("activate_reload": 320, "repeat_countdown": 0, "repeat_reload": 0, "triggers": [1], "chain": 1, )"
	              R"("last_checked_tick": 576, "fire_pending": false}, {"index": 1, "active": false, "fired": false, )"
	              R"("activate_countdown": 0, "activate_reload": 0, "repeat_countdown": 0, "repeat_reload": 0, )"
	              R"("triggers": [-1], "chain": -1, "last_checked_tick": -1, "fire_pending": true}], )"
	              R"("variables": [{"index": 12, "value": 7}, {"index": 600, "value": 1}]}, "state": null})");
}

// The live logic over a session (S24 Live): an OpenNova Play in the mission, its report read into the view and the
// live op, the calls play_debug queues and what the game made of them, stale once the document moves on, nothing
// once the game ends; and the link to the game's endpoint (session/live_logic_link) over a fake stream: initialize,
// the snapshot polled, a call made, an answer that does not come given up.
int test_live_session() {
	editor_test::TempProjectDir dir("opennova_mission_live_session");
	editor_test::FakePlatform platform;
	platform.terminate_exits = false;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Live"));
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	// A mission the build takes as it is (the default one) with two events, the first waiting on V1.
	std::vector<uint8_t> bytes;
	{
		bms::File file;
		mission::make_default(file);
		mission::add_event(file, mission::MissionEventRecord{});
		mission::add_event(file, mission::MissionEventRecord{});
		mission::MissionTriggerRecord trigger;
		trigger.main_type = 4; // MissionVariable
		trigger.sub_type = 1;  // equals
		trigger.param1 = 1;
		trigger.param2 = 1;
		std::string made;
		TEST_EXPECT(mission::insert_event_trigger(file, 0, 0, trigger, made) && bms::write(file, bytes, made));
	}
	const char *path = "missions/logic.bms";
	TEST_EXPECT(editor_test::write_bytes(v.project.root + "/" + path, bytes));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document(path));
	const std::string runtime = dir.file("runtime/opennova.exe");
	TEST_EXPECT(editor_test::write_text(runtime, "MZ"));
	PlayLauncher launcher;
	launcher.executable = runtime;
	launcher.mcp_port = 8993;
	session.set_launcher_source(editor_test::fixed_launcher(launcher));
	TEST_EXPECT(!session.live_logic_reachable() && !session.live_logic_polling());
	// Nothing to ask before a game reports.
	editor_test::handle_to_end(session, request::play_debug_fire(0));
	TEST_EXPECT(session.outcome().refused && !session.outcome().findings.empty() &&
	            session.outcome().findings.back().code() == "play.debug");
	const auto logic_pane = [&](bool shown) {
		editor_test::handle_to_end(session, request::set_workspace(std::string(R"({"document": {"path": "missions/logic.bms", "logic": )") +
		                                                           (shown ? "true" : "false") + "}}"));
	};
	const auto events_of = [&]() {
		const MissionDocument &m = dynamic_cast<const MissionDocument &>(*session.document_for(path));
		return m.rows_of(MissionKind::Event);
	};
	const auto live_op = [&]() {
		JsonValue args;
		std::string error;
		opennova::io::json_parse(R"({"path": "missions/logic.bms", "op": "live"})", args, error);
		return session.query("mission_logic", args, error);
	};

	editor_test::handle_to_end(session, request::play(path));
	TEST_EXPECT(v.activity.play_state == PlayState::Running && v.activity.play_mcp_port == 8993);
	if (v.activity.play_state != PlayState::Running) return 1;
	std::string error;
	const LiveLogic &live = v.activity.live_logic;
	// A game the editor can ask runs, but nothing is asked of it until the mission's Logic pane shows: a report
	// arriving meanwhile is passed over.
	TEST_EXPECT(session.live_logic_reachable() && !session.live_logic_polling());
	TEST_EXPECT(session.report_live_logic(snapshot_of("LOGIC"), error) && live.empty());
	logic_pane(true);
	TEST_EXPECT(session.live_logic_polling());
	// The game's report: its basename names the played mission (as the game spells it, any case); the played
	// events' rows are the document's, recorded as Play started.
	TEST_EXPECT(session.report_live_logic(snapshot_of("LOGIC"), error));
	std::vector<const Node *> events = events_of();
	TEST_EXPECT(live.mission == path && live.tick == 640 && !live.stale && live.played.size() == 2 &&
	            live.played[0].row == events[0]->id && live.played[1].row == events[1]->id &&
	            live.played[0].triggers.size() == 1);
	const uint64_t first_serial = live.serial;
	TEST_EXPECT(first_serial > 0);
	TEST_EXPECT(live.events.size() == 2 && live.event_of(events[0]->id, 0) && live.event(0)->active &&
	            live.event(0)->activate_countdown == 192 && live.event(0)->truth == std::vector<int8_t>({1}) &&
	            live.event(0)->chain == 1 && live.event(1)->fire_pending && live.event(1)->last_checked_tick == -1);
	TEST_EXPECT(!live.event_of(events[1]->id, 0)); // another row at that place: none of the game's state
	TEST_EXPECT(live.variables.size() == 512 && live.variables[12] == 7); // V600 is past the bank: dropped
	TEST_EXPECT(view_section_to_json(v, ViewSection::Run).get("live") &&
	            view_section_to_json(v, ViewSection::Run).get("live")->get_number("events", 0) == 2);
	// The live op: by the document's events and triggers, where they are the played ones.
	JsonValue answer = live_op();
	TEST_EXPECT(answer.get_bool("playing", false) && answer.get_bool("reachable", false) && answer.get_bool("polling", false) &&
	            answer.get_number("tick", 0) == 640);
	const JsonValue *reported = answer.get("events");
	TEST_EXPECT(reported && reported->array.size() == 2 && reported->array[0].get_number("event", 0) == double(events[0]->id) &&
	            reported->array[0].get("triggers") && reported->array[0].get("triggers")->array.size() == 1 &&
	            reported->array[0].get("triggers")->array[0].get_number("truth", 0) == 1 &&
	            reported->array[0].get("triggers")->array[0].get("trigger"));
	const JsonValue *variables = answer.get("variables");
	TEST_EXPECT(variables && variables->array.size() == 1 && variables->array[0].get_number("index", 0) == 12 &&
	            variables->array[0].get_number("value", 0) == 7 && variables->array[0].get_string("name", "") == "V12");

	// play_debug: a call queued for the Shell; refused past the game's events, for neither or both.
	editor_test::handle_to_end(session, request::play_debug_fire(1));
	TEST_EXPECT(!session.outcome().refused);
	editor_test::handle_to_end(session, request::play_debug_set(12, -5));
	TEST_EXPECT(!session.outcome().refused);
	editor_test::handle_to_end(session, request::play_debug_fire(9));
	TEST_EXPECT(session.outcome().refused && session.outcome().findings.back().message.find("no event 10") != std::string::npos);
	EditorRequest both = request::play_debug_fire(0);
	both.variable = 3;
	editor_test::handle_to_end(session, both);
	TEST_EXPECT(session.outcome().refused);
	editor_test::handle_to_end(session, request::play_debug_set(512, 1));
	TEST_EXPECT(session.outcome().refused);
	std::vector<PlayDebugCall> calls = session.play_debug_since(0);
	TEST_EXPECT(calls.size() == 2 && calls[0].op == PlayDebugOp::FireEvent && calls[0].event == 1 &&
	            calls[1].op == PlayDebugOp::SetVariable && calls[1].variable == 12 && calls[1].value == -5);
	TEST_EXPECT(session.play_debug_since(calls[0].serial).size() == 1);
	// The wire form of the request.
	JsonValue wire;
	opennova::io::json_parse(R"({"kind": "play_debug", "variable": 4, "value": -2})", wire, error);
	TEST_EXPECT(session.handle_json(wire).get_bool("ok", false));
	TEST_EXPECT(session.play_debug_since(0).size() == 3 && session.play_debug_since(0)[2].value == -2);
	// What became of them, said in Output: refused in the game's words, taken.
	session.answer_play_debug(calls[0].serial, PlayDebugAnswer::Refused,
	                          "Already fired: a fire-once event runs again only after a ResetEvent.");
	TEST_EXPECT(v.activity.status.find("The game refused to fire event 2: Already fired") == 0);
	session.answer_play_debug(calls[1].serial, PlayDebugAnswer::Taken, "");
	TEST_EXPECT(v.activity.status == "The game took the call to set V12 to -5.");

	// The link over a fake stream: initialize, then the call left queued, then the snapshot when due.
	FakeChannel channel;
	LiveLogicLink link;
	link.step(session, channel, 0);
	TEST_EXPECT(channel.connects == 1 && channel.port == 8993 && channel.sent.size() == 1 &&
	            channel.last_body().get_string("method", "") == "initialize" &&
	            channel.sent[0].find("Host: 127.0.0.1:8993\r\n") != std::string::npos);
	channel.answer(R"({"jsonrpc": "2.0", "id": 1, "result": {}})", "abc123");
	link.step(session, channel, 10);
	link.step(session, channel, 20);
	TEST_EXPECT(channel.sent.size() == 2 && channel.sent[1].find("Mcp-Session-Id: abc123\r\n") != std::string::npos);
	const JsonValue call = channel.last_body();
	TEST_EXPECT(call.get_string("method", "") == "tools/call" && call.get("params") &&
	            call.get("params")->get_string("name", "") == "game_debug" &&
	            call.get("params")->get("arguments")->get_string("id", "") == "set_mission_variable" &&
	            call.get("params")->get("arguments")->get_bool("confirm_authority", false));
	channel.answer(tool_answer(parsed(R"({"error": 0, "result": true})")));
	link.step(session, channel, 30);
	TEST_EXPECT(link.stats().calls == 1 && v.activity.status == "The game took the call to set V4 to -2.");
	// The snapshot, due: the view updated from its answer.
	link.step(session, channel, 300);
	TEST_EXPECT(channel.last_body().get("params")->get("arguments")->get_string("id", "") == "mission_logic_snapshot");
	channel.answer(tool_answer(snapshot_of("logic", 704)));
	link.step(session, channel, 310);
	TEST_EXPECT(link.stats().snapshots == 1 && live.tick == 704 && live.serial == first_serial + 1 && live.quiet_ms == 0);
	// A snapshot not answered is given up after 3 s, the stream opened again; past 2 s the board says how long
	// the game has been silent, and Output says so once.
	link.step(session, channel, 600);
	link.step(session, channel, 2700);
	TEST_EXPECT(live.quiet_ms >= LiveLogicLink::kQuietMs);
	bool said = false;
	for (const std::string &line : v.activity.output) said = said || line.find("has not answered the editor") != std::string::npos;
	TEST_EXPECT(said);
	link.step(session, channel, 600 + LiveLogicLink::kAnswerMs + 1);
	TEST_EXPECT(link.stats().failures == 1 && channel.connects == 2);
	// A call not answered in time: the game may still make it.
	editor_test::handle_to_end(session, request::play_debug_set(5, 1));
	link.step(session, channel, 4000); // initialize again
	channel.answer(R"({"jsonrpc": "2.0", "id": 5, "result": {}})", "abc124");
	link.step(session, channel, 4010);
	link.step(session, channel, 4020); // the call sent
	link.step(session, channel, 4020 + LiveLogicLink::kAnswerMs + 1);
	TEST_EXPECT(v.activity.status == "The game did not answer the call to set V5 to 1 within 3 s; it may still make it.");
	// A call in flight when the stream drops: the game went away.
	editor_test::handle_to_end(session, request::play_debug_set(6, 1));
	link.step(session, channel, 8000);
	channel.answer(R"({"jsonrpc": "2.0", "id": 7, "result": {}})", "abc125");
	link.step(session, channel, 8010);
	link.step(session, channel, 8020);
	channel.now = GameMcpChannel::State::Failed;
	link.step(session, channel, 8030);
	TEST_EXPECT(v.activity.status == "The game went away before it answered the call to set V6 to 1.");
	channel.now = GameMcpChannel::State::Connected;

	// The mission's tab closed and opened again unchanged: not stale, its new rows the played ones.
	editor_test::handle_to_end(session, request::close_document(path));
	session.poll();
	TEST_EXPECT(!session.live_logic_polling() && live.empty());
	editor_test::handle_to_end(session, request::open_document(path));
	logic_pane(true);
	TEST_EXPECT(session.report_live_logic(snapshot_of("logic"), error));
	events = events_of();
	TEST_EXPECT(!live.stale && live.played.size() == 2 && live.played[0].row == events[0]->id && live.event_of(events[0]->id, 0));
	TEST_EXPECT(live.serial > first_serial + 1); // never back

	// An event removed since Play started: the event that moved up shows none of the game's state, the live op
	// names no document event there, and a fire of either is refused in words.
	const NodeId first = events[0]->id;
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = {first, events[0]->kind, 0};
	editor_test::handle_to_end(session, request::edit_record(path, remove));
	session.poll();
	events = events_of();
	TEST_EXPECT(live.stale && events.size() == 1 && !live.event_of(events[0]->id, 0));
	const JsonValue after_removal = live_op();
	reported = after_removal.get("events");
	TEST_EXPECT(reported && reported->array.size() == 2 && !reported->array[0].get("event") && !reported->array[1].get("event"));
	editor_test::handle_to_end(session, request::play_debug_fire(0));
	TEST_EXPECT(session.outcome().refused &&
	            session.outcome().findings.back().message == "Event 1 was removed since Play started: Play again to fire it.");
	editor_test::handle_to_end(session, request::play_debug_fire(1));
	TEST_EXPECT(session.outcome().refused &&
	            session.outcome().findings.back().message == "Event 2 moved since Play started: Play again to fire it.");
	// Undone: the played rows back at their places.
	editor_test::handle_to_end(session, request::undo(path));
	session.poll();
	events = events_of();
	TEST_EXPECT(events.size() == 2 && live.event_of(events[1]->id, 1));

	// The pane closed: nothing asked, the report gone.
	logic_pane(false);
	session.poll();
	TEST_EXPECT(!session.live_logic_polling() && live.empty());
	link.step(session, channel, 12000);
	TEST_EXPECT(channel.now == GameMcpChannel::State::Closed);
	logic_pane(true);

	// Edited since it was played: stale, the played revision kept.
	TEST_EXPECT(session.report_live_logic(snapshot_of("logic"), error));
	const uint64_t played = live.played_revision;
	const MissionDocument &m = dynamic_cast<const MissionDocument &>(*session.document_for(path));
	editor_test::handle_to_end(session, request::edit_record(path, set_of(listed(m, MissionKind::Group)[4], "name", "Bridge patrol")));
	session.poll();
	TEST_EXPECT(live.stale && live.played_revision == played);
	// A report of another mission (the game moved on to it) holds none of this one's.
	TEST_EXPECT(session.report_live_logic(snapshot_of("other"), error) && live.empty());
	TEST_EXPECT(!session.report_live_logic(parsed(R"({"tick": 4})"), error) && !error.empty());
	// The game ends: no logic, no calls, nothing asked.
	TEST_EXPECT(session.report_live_logic(snapshot_of("logic"), error) && !live.empty());
	platform.exit_child(v.activity.play_pid);
	session.poll();
	TEST_EXPECT(v.activity.play_state == PlayState::Stopped && live.empty() && !session.live_logic_reachable() &&
	            session.play_debug_since(0).empty());
	link.step(session, channel, 20000);
	TEST_EXPECT(channel.now == GameMcpChannel::State::Closed);

	// Played with the mission not open, then opened (the played bytes: its rows the played ones) and edited before
	// the game reports: stale, the rows still the played ones where nothing moved them.
	editor_test::handle_to_end(session, request::save(path));
	editor_test::handle_to_end(session, request::close_document(path));
	editor_test::handle_to_end(session, request::play(path));
	TEST_EXPECT(v.activity.play_state == PlayState::Running);
	editor_test::handle_to_end(session, request::open_document(path));
	logic_pane(true);
	session.poll(); // the editor's next frame: the document read from the played bytes is the played one
	const MissionDocument &reopened = dynamic_cast<const MissionDocument &>(*session.document_for(path));
	editor_test::handle_to_end(session,
	                           request::edit_record(path, set_of(listed(reopened, MissionKind::Group)[5], "name", "Convoy")));
	session.poll();
	TEST_EXPECT(session.report_live_logic(snapshot_of("logic"), error));
	TEST_EXPECT(live.stale && live.played.size() == 2 && live.event_of(events_of()[0]->id, 0));
	editor_test::handle_to_end(session, request::play_debug_fire(0));
	TEST_EXPECT(!session.outcome().refused);
	platform.exit_child(v.activity.play_pid);
	session.poll();
	return 0;
}


// --- the names the .bms has no room for (S24 Names: an event's name, an area's and a path's description) --

// An event's name and an area's and a path's description: undoable fields of the rows, which the .bms written
// does not change, the mission's names in every word that names them; what the .mis cannot carry refused.
int test_row_names() {
	std::unique_ptr<Document> document = open_mission();
	TEST_EXPECT(document != nullptr);
	if (!document) return 1;
	const MissionDocument &m = dynamic_cast<const MissionDocument &>(*document);
	const std::string written = document->serialize().text;
	const std::vector<const Node *> events = m.rows_of(MissionKind::Event), areas = m.rows_of(MissionKind::Area),
	                                paths = m.rows_of(MissionKind::WaypointPath);
	TEST_EXPECT(events.size() == 2 && !areas.empty() && paths.size() == bms::kWaypointRecordCount);
	if (events.size() != 2 || areas.empty()) return 1;
	const NodeAddress event{events[0]->id, events[0]->kind, 0}, area{areas[0]->id, areas[0]->kind, 0},
	                  path{paths[3]->id, paths[3]->kind, 0};
	const int32_t zone = static_cast<const AreaRow &>(*areas[0]).native.id;
	TEST_EXPECT(same(DocumentMissionNames(m).event(0), "event 1") && same(DocumentMissionNames(m).path(3), "path 3"));
	TEST_EXPECT(apply_edits(*document, {set_of(event, "name", "Get to the bridge"), set_of(area, "description", "The bridge"),
	                                    set_of(path, "description", "North road")}));
	Value value;
	TEST_EXPECT(document->get(event, "name", value) && value == Value(std::string("Get to the bridge")));
	TEST_EXPECT(document->get(area, "description", value) && value == Value(std::string("The bridge")));
	// The rows' own: the .bms the same bytes.
	TEST_EXPECT(document->dirty() && document->serialize().text == written);
	const DocumentMissionNames names(m);
	TEST_EXPECT(same(names.event(0), "Get to the bridge") && same(names.event(1), "event 2") &&
	            same(names.zone(zone), "The bridge") && same(names.path(3), "North road") && same(names.path(4), "path 4"));
	TEST_EXPECT(mission_record_label(m, event, nullptr).rfind("Event 1 (Get to the bridge): ", 0) == 0);
	TEST_EXPECT(mission_record_label(m, area, nullptr).rfind("Zone " + std::to_string(zone) + ": The bridge", 0) == 0);
	TEST_EXPECT(mission_record_label(m, path, nullptr).rfind("Path 3: North road (", 0) == 0);
	TEST_EXPECT(same(mission_record_brief(m, event, nullptr), "Event 1 (Get to the bridge)"));
	// What the .mis cannot carry is refused (mis_name_allowed), never rewritten: a ';', a double quote, a name past
	// the original editor's dialog (59 for an event, 249 for a description).
	TEST_EXPECT(!apply_edits(*document, {set_of(event, "name", "north; south")}));
	TEST_EXPECT(!apply_edits(*document, {set_of(event, "name", "say \"hi\"")}));
	TEST_EXPECT(!apply_edits(*document, {set_of(event, "name", std::string(60, 'e'))}));
	TEST_EXPECT(!apply_edits(*document, {set_of(area, "description", std::string(250, 'a'))}));
	TEST_EXPECT(apply_edits(*document, {set_of(event, "name", std::string(59, 'e'))}));
	TEST_EXPECT(apply_edits(*document, {set_of(area, "description", "Pont de l'\xC3\xA9t\xC3\xA9")}));
	// Undo takes each step back.
	document->undo();
	document->undo();
	TEST_EXPECT(document->get(event, "name", value) && value == Value(std::string("Get to the bridge")));
	document->undo();
	TEST_EXPECT(document->get(event, "name", value) && value == Value(std::string()) && !document->dirty());
	return 0;
}

// A mission of two events (the first waiting on V1, the second showing a text), zone 4 and group 2's member,
// and the names its .mis holds.
bms::File names_mission() {
	bms::File file;
	mission::make_default(file);
	mission::add_event(file, mission::MissionEventRecord{});
	mission::add_event(file, mission::MissionEventRecord{});
	mission::MissionTriggerRecord trigger;
	trigger.main_type = 4; // MissionVariable
	trigger.sub_type = 1;  // equals
	trigger.param1 = 1;
	trigger.param2 = 1;
	mission::MissionActionRecord action;
	action.action_type = 5;
	action.action_sub_type = 1;
	action.param1 = 3;
	std::string made;
	mission::insert_event_trigger(file, 0, 0, trigger, made);
	mission::insert_event_action(file, 1, 0, action, made);
	mission::AreaTriggerRecord area;
	area.wp_number = 4;
	area.max_x = 10.0f;
	area.max_y = 10.0f;
	mission::add_area_trigger(file, area);
	mission::sync_counts(file);
	return file;
}
mission::MisNames file_names() {
	mission::MisNames names;
	names.events = {"Get to the bridge", ""};
	names.areas[4] = "The bridge";
	names.paths[3] = "North road";
	return names;
}
// The mission with event 2 holding one more action: another mission's events.
bms::File other_mission(const bms::File &from) {
	bms::File other = from;
	mission::MissionActionRecord extra;
	extra.action_type = 5;
	extra.param1 = 7;
	std::string made;
	mission::insert_event_action(other, 1, 1, extra, made);
	mission::sync_counts(other);
	return other;
}
std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }
uint64_t fingerprint_of(const std::vector<uint8_t> &bytes) {
	return opennova::io::fnv1a64_bytes(opennova::io::kFnv1a64Offset, bytes.data(), bytes.size());
}
// A .mis as the original editor saves it: its first line says when (the rest as written).
std::string original_editors(const std::string &text) {
	return "// mission metafile, saved on 10/11/2026, 12:00" + text.substr(text.find("\r\n"));
}
// The .mis as the session finds it: its bytes (null: none there), or one the file system would not read.
CompanionFile beside(const std::vector<uint8_t> *bytes) {
	CompanionFile out;
	if (bytes) {
		out.state = CompanionFile::State::Read;
		out.bytes = *bytes;
	}
	return out;
}
CompanionFile refused_by_disk(const std::string &why) {
	CompanionFile out;
	out.state = CompanionFile::State::Unreadable;
	out.error = why;
	return out;
}
// The mission's bytes read as the session reads a mission: the .mis handed over first.
std::unique_ptr<Document> read_with(const std::vector<uint8_t> &bms_bytes, const CompanionFile &mis) {
	std::unique_ptr<Document> document = records_of(document_type_for(AssetKind::Mission)->make());
	auto *m = dynamic_cast<MissionDocument *>(document.get());
	if (!m) return nullptr;
	m->take_names_file(mis);
	Diagnostic error;
	if (!document->load_bytes(bms_bytes, "names.bms", AssetKind::Mission, "jo", error)) return nullptr;
	return document;
}
std::unique_ptr<Document> read_with(const std::vector<uint8_t> &bms_bytes, const std::vector<uint8_t> *mis) {
	return read_with(bms_bytes, beside(mis));
}
std::vector<Diagnostic> names_findings(const MissionDocument &m) {
	std::vector<Diagnostic> out;
	names_file_findings(m, out);
	return out;
}
// What a save writes over `now`, where it writes there.
bool save_text(MissionDocument &m, const CompanionFile &now, std::string &text) {
	return m.names_file_due(now) && m.names_file_text(text);
}
NodeAddress event_at(const MissionDocument &m, size_t index) {
	const std::vector<const Node *> events = m.rows_of(MissionKind::Event);
	return index < events.size() ? NodeAddress{events[index]->id, events[index]->kind, 0} : NodeAddress{};
}

// The .mis read with the mission: its names the rows' where it matches (a group the .bms names none of among
// them), what the setters refuse left out and listed, what does not match listed with its fixes, and where a save
// writes it: the editor's own alone, never the original editor's, one that does not match, one changed outside.
int test_names_file() {
	const bms::File plain = names_mission();
	bms::File grouped = plain;
	std::string error, mis_text, text;
	TEST_EXPECT(bms::set_group_name(grouped.group_records[2], "Bravo", error));
	const mission::MisNames names = file_names();
	TEST_EXPECT(mission::write_mis_text(grouped, mis_text, error, nullptr, &names));
	std::vector<uint8_t> bms_bytes;
	TEST_EXPECT(bms::write(plain, bms_bytes, error));
	const std::vector<uint8_t> mis = bytes_of(mis_text);

	// Matched, and the editor's own: the names taken, the group's among them, the document clean; a save writes it
	// again, the same text.
	std::unique_ptr<Document> document = read_with(bms_bytes, &mis);
	TEST_EXPECT(document != nullptr);
	if (!document) return 1;
	auto &m = dynamic_cast<MissionDocument &>(*document);
	TEST_EXPECT(m.names_file().found == MissionNamesFile::Found::Matched && m.names_file().ours && !document->dirty() &&
	            !m.companion_unsaved());
	const DocumentMissionNames read(m);
	TEST_EXPECT(same(read.event(0), "Get to the bridge") && same(read.event(1), "event 2") && same(read.zone(4), "The bridge") &&
	            same(read.path(3), "North road") && same(read.group(2), "Bravo"));
	TEST_EXPECT(names_findings(m).empty());
	TEST_EXPECT(save_text(m, beside(&mis), text) && text == mis_text);

	// The original editor's .mis of the same mission (B1): its names read, the file never written by a save (it writes
	// what the editor's writer does not). A name edited stays unsaved, listed with Write it from the mission, whose
	// words say what the rewrite loses; the write spends it.
	const std::vector<uint8_t> original = bytes_of(original_editors(mis_text));
	document = read_with(bms_bytes, &original);
	auto &f = dynamic_cast<MissionDocument &>(*document);
	TEST_EXPECT(f.names_file().found == MissionNamesFile::Found::Matched && !f.names_file().ours &&
	            same(DocumentMissionNames(f).event(0), "Get to the bridge") && same(DocumentMissionNames(f).group(2), "Bravo"));
	TEST_EXPECT(!f.names_file_due(beside(&original)) && !f.companion_unsaved() && names_findings(f).empty());
	TEST_EXPECT(apply_edits(*document, {set_of(event_at(f, 1), "name", "Hold the bridge")}));
	TEST_EXPECT(f.companion_unsaved() && !f.names_file_due(beside(&original)) && !f.names_left().outside);
	std::vector<Diagnostic> found = names_findings(f);
	TEST_EXPECT(found.size() == 1 && found[0].code() == "mission.names_foreign" && found[0].planned.size() == 1 &&
	            found[0].message.find("stay unsaved") != std::string::npos);
	if (found.size() != 1 || found[0].planned.size() != 1 || found[0].planned[0].edits.size() != 1) return 1;
	const PlannedFix &write = found[0].planned[0];
	TEST_EXPECT(write.label == "Write it from the mission" && write.detail.find("weapon availability") != std::string::npos &&
	            write.detail.find("base height") != std::string::npos);
	const Edit &over = write.edits[0];
	const auto *hex = std::get_if<std::string>(&over.value);
	TEST_EXPECT(over.operation == EditOperation::SetFileValue && over.field == kMisOverwriteField && hex &&
	            *hex == fingerprint_hex(fingerprint_of(original)));
	TEST_EXPECT(apply_edits(*document, write.edits) && names_findings(f).empty() &&
	            f.names_file_overwrite() == fingerprint_of(original));
	TEST_EXPECT(save_text(f, beside(&original), text) && text.find("Hold the bridge") != std::string::npos);
	f.names_file_written(true, fingerprint_of(bytes_of(text)), std::string());
	TEST_EXPECT(f.names_file().ours && !f.companion_unsaved() && f.names_file_overwrite() == 0);
	// The original's bytes put back after the write it was for: another program's, left as they are (m6).
	TEST_EXPECT(!f.names_file_due(beside(&original)) && f.names_left().outside);
	// The value names its bytes as 16 hex digits (a number past 2^53 does not survive JSON); a number is refused.
	uint64_t back = 0;
	TEST_EXPECT(fingerprint_from_hex(fingerprint_hex(0xfedcba9876543210ull), back) && back == 0xfedcba9876543210ull);
	TEST_EXPECT(!fingerprint_from_hex("12", back) && !fingerprint_from_hex("zzzzzzzzzzzzzzzz", back));
	Edit number = over;
	number.value = int64_t(42);
	Diagnostic refused;
	TEST_EXPECT(!document->apply({number}, refused) && refused.message.find("16 hex digits") != std::string::npos);

	// The original editor's .mis left by a save after the mission's records moved under it (an area's zone id): it no
	// longer matches, so the next open takes none of its names, which the row says (N2), Write it from the mission
	// saying what it loses.
	document = read_with(bms_bytes, &original);
	auto &g = dynamic_cast<MissionDocument &>(*document);
	{
		const std::vector<const Node *> zones = g.rows_of(MissionKind::Area);
		TEST_EXPECT(zones.size() == 1);
		if (zones.size() != 1) return 1;
		Edit moved_zone;
		moved_zone.address = {zones[0]->id, zones[0]->kind, 0};
		moved_zone.field = "id";
		moved_zone.value = int64_t(5);
		TEST_EXPECT(apply_edits(*document, {moved_zone}));
	}
	TEST_EXPECT(!g.names_file_due(beside(&original)) && g.names_left().stale.find("describes zone 4") != std::string::npos);
	found = names_findings(g);
	TEST_EXPECT(found.size() == 1 && found[0].code() == "mission.names_unmatched" &&
	            found[0].message.find("no longer matches the mission") != std::string::npos &&
	            found[0].message.find("next open takes none of its names") != std::string::npos &&
	            found[0].planned.size() == 1 && found[0].planned[0].detail.find("weapon availability") != std::string::npos);

	// The editor's own .mis gone since it was read, every name cleared: nothing to write, the names saved as none, the
	// mission no longer unsaved for them (N3).
	{
		std::string plain_text;
		TEST_EXPECT(mission::write_mis_text(plain, plain_text, error, nullptr, &names));
		const std::vector<uint8_t> own = bytes_of(plain_text);
		document = read_with(bms_bytes, &own);
		auto &c = dynamic_cast<MissionDocument &>(*document);
		TEST_EXPECT(c.names_file().found == MissionNamesFile::Found::Matched && c.names_file().ours);
		const std::vector<const Node *> zones = c.rows_of(MissionKind::Area), paths = c.rows_of(MissionKind::WaypointPath);
		TEST_EXPECT(apply_edits(*document, {set_of(event_at(c, 0), "name", ""),
		                                    set_of({zones[0]->id, zones[0]->kind, 0}, "description", ""),
		                                    set_of({paths[3]->id, paths[3]->kind, 0}, "description", "")}));
		TEST_EXPECT(c.companion_unsaved() && !c.names_file_due(CompanionFile()));
		TEST_EXPECT(c.names_file().found == MissionNamesFile::Found::None && !c.companion_unsaved() &&
		            names_findings(c).empty());
	}

	// Another mission's .mis, the original editor's (event 2's lines differ): no name taken, listed with what still
	// matches and Write it from the mission; a save never writes it, the names taken staying unsaved (M2).
	const std::vector<uint8_t> foreign = [&] {
		std::string other_text;
		mission::write_mis_text(other_mission(grouped), other_text, error, nullptr, &names);
		return bytes_of(original_editors(other_text));
	}();
	document = read_with(bms_bytes, &foreign);
	auto &u = dynamic_cast<MissionDocument &>(*document);
	TEST_EXPECT(u.names_file().found == MissionNamesFile::Found::Unmatched && !u.names_file().ours &&
	            u.names_file().why.find("event 2") != std::string::npos);
	TEST_EXPECT(same(DocumentMissionNames(u).event(0), "event 1") && same(DocumentMissionNames(u).group(2), "group 2"));
	found = names_findings(u);
	TEST_EXPECT(found.size() == 1 && found[0].code() == "mission.names_unmatched" && found[0].planned.size() == 2 &&
	            found[0].message.find("Save leaves it as it is") != std::string::npos);
	if (found.size() != 1 || found[0].planned.size() != 2) return 1;
	TEST_EXPECT(found[0].planned[0].label == "Take the names that still match" && found[0].planned[0].edits.size() == 4 &&
	            found[0].planned[1].label == "Write it from the mission" &&
	            found[0].planned[1].detail.find("weapon availability") != std::string::npos);
	TEST_EXPECT(!u.names_file_due(beside(&foreign)));
	// Take the names that still match: the first event's (the second's lines differ), the area's, the path's, the
	// group's; one step Undo takes back. Taken, they stay unsaved: the save still leaves the file.
	TEST_EXPECT(apply_edits(*document, found[0].planned[0].edits));
	TEST_EXPECT(same(DocumentMissionNames(u).event(0), "Get to the bridge") && same(DocumentMissionNames(u).zone(4), "The bridge") &&
	            same(DocumentMissionNames(u).path(3), "North road") && same(DocumentMissionNames(u).group(2), "Bravo"));
	TEST_EXPECT(u.companion_unsaved() && !u.names_file_due(beside(&foreign)));
	found = names_findings(u);
	TEST_EXPECT(found.size() == 1 && found[0].message.find("stay unsaved") != std::string::npos);
	document->undo();
	TEST_EXPECT(same(DocumentMissionNames(u).event(0), "event 1") && !document->dirty() && !u.companion_unsaved());
	// Write it from the mission: a file-wide value, the bytes a save writes over; the listing gone.
	found = names_findings(u);
	TEST_EXPECT(found.size() == 1 && found[0].planned.size() == 2 && apply_edits(*document, found[0].planned[1].edits));
	TEST_EXPECT(u.names_file_overwrite() == fingerprint_of(foreign) && document->dirty() && names_findings(u).empty());
	TEST_EXPECT(save_text(u, beside(&foreign), text) && text.find("Get to the bridge") == std::string::npos);
	u.names_file_written(true, fingerprint_of(bytes_of(text)), std::string());
	TEST_EXPECT(u.names_file().found == MissionNamesFile::Found::Matched && u.names_file().ours);

	// The editor's own .mis of the mission as it was, its events no longer the mission's (M2): nothing taken, never
	// written over by a save, listed with the names that still match and Write it from the mission, whose words say
	// the names of the events that no longer match go with it.
	const std::vector<uint8_t> stale = [&] {
		std::string other_text;
		mission::write_mis_text(other_mission(grouped), other_text, error, nullptr, &names);
		return bytes_of(other_text);
	}();
	document = read_with(bms_bytes, &stale);
	auto &e = dynamic_cast<MissionDocument &>(*document);
	TEST_EXPECT(e.names_file().found == MissionNamesFile::Found::Unmatched && e.names_file().ours &&
	            !e.names_file_due(beside(&stale)) && same(DocumentMissionNames(e).event(0), "event 1"));
	found = names_findings(e);
	TEST_EXPECT(found.size() == 1 && found[0].planned.size() == 2 &&
	            found[0].planned[1].detail.find("no longer match") != std::string::npos);

	// Changed outside the editor since it was read (another OpenNova's names): a save leaves it, listed with Read the
	// names again (it still matches) and Write it from the mission. Read again: its names the mission's, the mission
	// measured against those bytes, which are the editor's own to write again; Undo takes it back.
	document = read_with(bms_bytes, &mis);
	auto &o = dynamic_cast<MissionDocument &>(*document);
	mission::MisNames renamed = names;
	renamed.events = {"Cross the river", "Hold"};
	renamed.paths[3].clear();
	std::string renamed_text;
	TEST_EXPECT(mission::write_mis_text(grouped, renamed_text, error, nullptr, &renamed));
	const std::vector<uint8_t> changed = bytes_of(renamed_text);
	TEST_EXPECT(!o.names_file_due(beside(&changed)) && o.names_left().outside);
	found = names_findings(o);
	TEST_EXPECT(found.size() == 1 && found[0].code() == "mission.names_outside" && found[0].planned.size() == 2);
	if (found.size() != 1 || found[0].planned.size() != 2) return 1;
	TEST_EXPECT(found[0].planned[0].label == "Read the names again" && found[0].planned[1].label == "Write it from the mission");
	TEST_EXPECT(apply_edits(*document, found[0].planned[0].edits));
	TEST_EXPECT(same(DocumentMissionNames(o).event(0), "Cross the river") && same(DocumentMissionNames(o).event(1), "Hold") &&
	            same(DocumentMissionNames(o).path(3), "path 3") && names_findings(o).empty() && !o.companion_unsaved());
	TEST_EXPECT(save_text(o, beside(&changed), text) && text == renamed_text);
	document->undo();
	TEST_EXPECT(same(DocumentMissionNames(o).event(0), "Get to the bridge") && !o.names_file_due(beside(&changed)));
	// Gone since it was read: the editor's to make again, the mission holding names.
	TEST_EXPECT(o.names_file_due(CompanionFile()));

	// None beside the mission: one holding no name makes none (M4); one naming an event makes it.
	document = read_with(bms_bytes, nullptr);
	auto &n = dynamic_cast<MissionDocument &>(*document);
	TEST_EXPECT(n.names_file().found == MissionNamesFile::Found::None && !n.names_file_due(CompanionFile()));
	TEST_EXPECT(apply_edits(*document, {set_of(event_at(n, 0), "name", "Start")}) && n.companion_unsaved() &&
	            save_text(n, CompanionFile(), text) && text.find("\"Start\"") != std::string::npos);
	// What the writer refuses (a zone id outside 1..99) is listed while the names stay unsaved, never written; the
	// mission's own file is not the .mis's to fail.
	const std::vector<const Node *> areas = n.rows_of(MissionKind::Area);
	TEST_EXPECT(areas.size() == 1);
	if (areas.size() != 1) return 1;
	Edit zero;
	zero.address = {areas[0]->id, areas[0]->kind, 0};
	zero.field = "id";
	zero.value = int64_t(0);
	TEST_EXPECT(apply_edits(*document, {zero}) && n.names_file_due(CompanionFile()) && !n.names_file_text(text));
	found = names_findings(n);
	TEST_EXPECT(found.size() == 1 && found[0].code() == "mission.names_unwritten" &&
	            found[0].message.find("zone id 0") != std::string::npos &&
	            found[0].message.find("alone. The mission's own file was saved.") != std::string::npos);
	TEST_EXPECT(document->serialize().ok());

	// A .mis that does not read: listed with Write it from the mission alone.
	const std::vector<uint8_t> broken = bytes_of("begin event 0\r\n  name \"x\"\r\n");
	document = read_with(bms_bytes, &broken);
	auto &b = dynamic_cast<MissionDocument &>(*document);
	found = names_findings(b);
	TEST_EXPECT(b.names_file().found == MissionNamesFile::Found::Unreadable && found.size() == 1 &&
	            found[0].code() == "mission.names_unmatched" && found[0].planned.size() == 1 &&
	            found[0].planned[0].label == "Write it from the mission");
	// One the file system would not read: listed, never taken for none (m5); a save that cannot read it either
	// leaves it.
	document = read_with(bms_bytes, refused_by_disk("in use by another program"));
	auto &l = dynamic_cast<MissionDocument &>(*document);
	found = names_findings(l);
	TEST_EXPECT(l.names_file().found == MissionNamesFile::Found::Unopened && found.size() == 1 &&
	            found[0].code() == "mission.names_unmatched" && found[0].planned.empty() &&
	            found[0].message.find("could not be read (in use by another program)") != std::string::npos);
	TEST_EXPECT(!l.names_file_due(refused_by_disk("in use by another program")) && !l.names_left().unwritten.empty());

	// A name the setters refuse (an event's past the dialog's 59 bytes, written by hand), and path 0's description
	// (no path): left out of the mission, listed (m2).
	std::string long_text = mis_text;
	long_text.replace(long_text.find("Get to the bridge"), std::string("Get to the bridge").size(), std::string(60, 'n'));
	long_text += "begin waypoint 0\r\n  description \"Nowhere\"\r\nend waypoint\r\n";
	const std::vector<uint8_t> hand = bytes_of(long_text);
	document = read_with(bms_bytes, &hand);
	auto &h = dynamic_cast<MissionDocument &>(*document);
	found = names_findings(h);
	TEST_EXPECT(h.names_file().found == MissionNamesFile::Found::Matched && same(DocumentMissionNames(h).event(0), "event 1") &&
	            same(DocumentMissionNames(h).zone(4), "The bridge") && !h.companion_unsaved());
	TEST_EXPECT(found.size() == 1 && found[0].code() == "mission.names_refused" &&
	            found[0].message.find("event 1's name (A name holds 59 bytes at most") != std::string::npos &&
	            found[0].message.find("path 0's description (path 0 is no path)") != std::string::npos);

	// A read from bytes alone (no .mis handed over: a closed file's validation, an import's read): nothing beside.
	std::unique_ptr<Document> alone = records_of(document_type_for(AssetKind::Mission)->make());
	Diagnostic load_error;
	TEST_EXPECT(alone && alone->load_bytes(bms_bytes, "names.bms", AssetKind::Mission, "jo", load_error));
	auto &a = dynamic_cast<MissionDocument &>(*alone);
	TEST_EXPECT(a.names_file().found == MissionNamesFile::Found::Unread && !a.names_file_due(CompanionFile()) &&
	            names_findings(a).empty() && !a.companion_unsaved());
	return 0;
}

// The editor's own .mis of a mission with entities in every pool: what it reads, written again, is the bytes read
// (the fixed point, each entity's heights as read), and a save over another terrain moves the unlocked entities'
// heights alone (their z less the terrain's height under them, D-MIS-4), the file written still the editor's own.
int test_names_file_entities() {
	bms::File file = names_mission();
	const mission::EntityKind kinds[] = {mission::EntityKind::Item, mission::EntityKind::Building, mission::EntityKind::Marker,
	                                     mission::EntityKind::Organic};
	int32_t ssn = 1;
	for (const mission::EntityKind kind : kinds) {
		bms::Entity entity = mission::new_entity(kind, 106102, ssn);
		entity.x = bms::to_fixed_16_16(10.0 * ssn);
		entity.y = bms::to_fixed_16_16(-5.0 * ssn);
		entity.z = bms::to_fixed_16_16(2.5 * ssn);
		mission::entities(file, kind)->push_back(entity);
		++ssn;
	}
	mission::sync_counts(file);
	const std::vector<int32_t> heights = {bms::to_fixed_16_16(12.5), bms::to_fixed_16_16(3.0), bms::to_fixed_16_16(-1.25),
	                                      bms::to_fixed_16_16(40.0)};
	const mission::MisNames names = file_names();
	std::string error, mis_text, text;
	TEST_EXPECT(mission::write_mis_text(file, mis_text, error, &heights, &names));
	std::vector<uint8_t> bms_bytes;
	TEST_EXPECT(bms::write(file, bms_bytes, error));
	const std::vector<uint8_t> mis = bytes_of(mis_text);
	std::unique_ptr<Document> document = read_with(bms_bytes, &mis);
	TEST_EXPECT(document != nullptr);
	if (!document) return 1;
	auto &m = dynamic_cast<MissionDocument &>(*document);
	TEST_EXPECT(m.names_file().found == MissionNamesFile::Found::Matched && m.names_file().ours);
	m.set_base_heights(heights);
	TEST_EXPECT(save_text(m, beside(&mis), text) && same(text, mis_text));
	std::vector<int32_t> moved = heights;
	for (int32_t &height : moved) height += bms::to_fixed_16_16(1.0);
	m.set_base_heights(moved);
	const bms::Entity &last = file.organics.back();
	TEST_EXPECT(save_text(m, beside(&mis), text) && text != mis_text &&
	            text.find("  position " + std::to_string(last.x) + " " + std::to_string(last.y) + " " +
	                      std::to_string(last.z - moved[3]) + "\r\n") != std::string::npos);
	const std::vector<uint8_t> written = bytes_of(text);
	document = read_with(bms_bytes, &written);
	TEST_EXPECT(document && dynamic_cast<MissionDocument &>(*document).names_file().ours);
	return 0;
}

// Copy, Cut and Paste carry an event's name and an area's description (m1).
int test_names_clipboard() {
	const bms::File plain = names_mission();
	std::string error;
	std::vector<uint8_t> bms_bytes;
	TEST_EXPECT(bms::write(plain, bms_bytes, error));
	std::unique_ptr<Document> document = read_with(bms_bytes, nullptr);
	TEST_EXPECT(document != nullptr);
	if (!document) return 1;
	auto &m = dynamic_cast<MissionDocument &>(*document);
	const NodeAddress event = event_at(m, 1);
	const std::vector<const Node *> areas = m.rows_of(MissionKind::Area);
	const NodeAddress area{areas[0]->id, areas[0]->kind, 0};
	TEST_EXPECT(apply_edits(*document, {set_of(event, "name", "Hold the bridge"), set_of(area, "description", "The bridge")}));
	const std::string payload = document->copy({area, event});
	TEST_EXPECT(!payload.empty() && document->pastes_rows(payload));
	Edit paste;
	paste.operation = EditOperation::Paste;
	paste.value = payload;
	paste.position = SIZE_MAX;
	TEST_EXPECT(apply_edits(*document, {paste}));
	const std::vector<const Node *> events = m.rows_of(MissionKind::Event), pasted_areas = m.rows_of(MissionKind::Area);
	TEST_EXPECT(events.size() == 3 && pasted_areas.size() == 2);
	if (events.size() != 3 || pasted_areas.size() != 2) return 1;
	TEST_EXPECT(same(static_cast<const EventRow &>(*events[2]).name, "Hold the bridge") &&
	            same(static_cast<const AreaRow &>(*pasted_areas[1]).description, "The bridge"));
	return 0;
}

const Diagnostic *finding_of(const SessionView &view, const char *code, const std::string &asset) {
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code() == code && d.asset == asset) return &d;
	return nullptr;
}

// A session over a new project for the .mis: missions written into it, read, edited and saved.
struct NamesProject {
	editor_test::TempProjectDir dir;
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;
	explicit NamesProject(const char *name) : dir(name) {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Names"));
		editor_test::create_missing_files(session);
		root = session.view().project.root;
	}
	const SessionView &view() const { return session.view(); }
	void run(const EditorRequest &request) { editor_test::handle_to_end(session, request); }
	bool put(const std::string &file, const std::vector<uint8_t> &bytes) { return editor_test::write_bytes(root + "/" + file, bytes); }
	bool put(const std::string &file, const std::string &text) { return put(file, bytes_of(text)); }
	std::string text(const std::string &file) const { return test_io::read_file_text(root + "/" + file); }
	bool exists(const std::string &file) const {
		std::error_code ec;
		return std::filesystem::exists(system_path(root + "/" + file), ec);
	}
	const MissionDocument *mission(const std::string &path) { return dynamic_cast<const MissionDocument *>(session.document_for(path)); }
	mission::MisNames names_in(const std::string &file) const {
		mission::MisNames read;
		std::string why;
		mission::parse_mis_names(text(file), read, why);
		return read;
	}
	// The open mission's event `index` named `name` (an edit of the mission).
	void name_event(const std::string &path, size_t index, const std::string &name) {
		run(request::edit_record(path, set_of(event_at(*mission(path), index), "name", name)));
	}
};

// The .mis through the session: read on open, written after the mission's save where it is the editor's own, an
// outside change listed and left (its names unsaved, the close prompt asking) until Write it from the mission, sent
// over the wire as an MCP client sends it, writes over it; a rename taking it with the mission; what the writer
// refuses listed while the mission saves, its names unsaved; a mission with none beside it and no name makes none.
int test_names_file_session() {
	NamesProject p("opennova_mission_names_file");
	const SessionView &v = p.view();
	const bms::File plain = names_mission();
	std::string error, mis_text;
	const mission::MisNames names = file_names();
	TEST_EXPECT(mission::write_mis_text(plain, mis_text, error, nullptr, &names));
	std::vector<uint8_t> bms_bytes;
	TEST_EXPECT(bms::write(plain, bms_bytes, error));
	TEST_EXPECT(p.put("missions/names.bms", bms_bytes) && p.put("missions/names.mis", mis_text));
	p.run(request::rescan());
	const std::string path = "missions/names.bms";
	p.run(request::open_document(path));
	const MissionDocument *m = p.mission(path);
	TEST_EXPECT(m && m->names_file().found == MissionNamesFile::Found::Matched);
	if (!m) return 1;
	TEST_EXPECT(same(DocumentMissionNames(*m).event(0), "Get to the bridge"));
	// A name edited and saved: the .bms as it was, the .mis with the name.
	p.name_event(path, 1, "Hold the bridge");
	p.run(request::save(path));
	TEST_EXPECT(test_io::read_file(p.root + "/" + path) == bms_bytes);
	TEST_EXPECT(p.names_in("missions/names.mis").events == std::vector<std::string>({"Get to the bridge", "Hold the bridge"}));
	TEST_EXPECT(!p.session.document_for(path)->unsaved());
	// Another program's .mis since: the next save leaves it, the names unsaved (the close prompt asks), listed with
	// Read the names again and Write it from the mission.
	const std::string outside = mis_text + "// a note\r\n";
	TEST_EXPECT(p.put("missions/names.mis", outside));
	p.name_event(path, 1, "Take the bridge");
	p.run(request::save(path));
	TEST_EXPECT(p.text("missions/names.mis") == outside);
	TEST_EXPECT(!p.session.document_for(path)->dirty() && p.session.document_for(path)->companion_unsaved() &&
	            v.activity.status.find("keeps its names unsaved") != std::string::npos);
	p.run(request::close_document(path));
	TEST_EXPECT(v.dialogs.unsaved_prompt.open && p.session.document_for(path) != nullptr);
	p.run(request::resolve_unsaved(UnsavedChoice::Cancel));
	const Diagnostic *listed = finding_of(v, "mission.names_outside", path);
	TEST_EXPECT(listed && listed->planned.size() == 2);
	if (!listed || listed->planned.size() != 2) return 1;
	const std::vector<ProblemFix> fixes = fixes_for(*listed, v);
	TEST_EXPECT(fixes.size() == 2 && fixes[0].label == "Read the names again" && fixes[1].label == "Write it from the mission");
	if (fixes.size() != 2) return 1;
	// The file there now is no editor's own: what Write it from the mission loses of it is said (N4).
	TEST_EXPECT(fixes[1].detail.find("weapon availability") != std::string::npos);
	// Write it from the mission over the wire (M3): the request as JSON text, read back as an MCP client sends it.
	JsonValue sent;
	std::string parse_error;
	TEST_EXPECT(opennova::io::json_parse(opennova::io::json_write(editor_request_to_json(fixes[1].request, p.session.document_for(path))),
	                                     sent, parse_error));
	TEST_EXPECT(p.session.handle_json(sent).get_bool("ok", false));
	p.session.run_operations();
	p.run(request::save(path));
	TEST_EXPECT(p.names_in("missions/names.mis").events == std::vector<std::string>({"Get to the bridge", "Take the bridge"}) &&
	            !finding_of(v, "mission.names_outside", path) && !p.session.document_for(path)->unsaved());
	// A rename takes the .mis with the mission.
	p.run(request::rename_asset("names.bms", "renamed.bms"));
	TEST_EXPECT(p.exists("missions/renamed.bms") && p.exists("missions/renamed.mis") && !p.exists("missions/names.mis"));
	const std::string renamed = "missions/renamed.bms";
	// What the writer refuses (an area's zone id 0, its description then in no slot): the mission saved, the .mis
	// left, listed, the names unsaved (M1).
	const MissionDocument *r = p.mission(renamed);
	TEST_EXPECT(r != nullptr);
	if (!r) return 1;
	const std::vector<const Node *> areas = r->rows_of(MissionKind::Area);
	Edit zero;
	zero.address = {areas[0]->id, areas[0]->kind, 0};
	zero.field = "id";
	zero.value = int64_t(0);
	const std::string before = p.text("missions/renamed.mis");
	p.run(request::edit_record(renamed, zero));
	p.run(request::save(renamed));
	TEST_EXPECT(!p.session.document_for(renamed)->dirty() && p.session.document_for(renamed)->companion_unsaved() &&
	            p.text("missions/renamed.mis") == before);
	TEST_EXPECT(finding_of(v, "mission.names_unwritten", renamed) != nullptr);
	// A mission with none beside it: a save that holds no name makes none; one naming an event makes it.
	TEST_EXPECT(p.put("missions/fresh.bms", bms_bytes));
	p.run(request::rescan());
	p.run(request::open_document("missions/fresh.bms"));
	const MissionDocument *fresh = p.mission("missions/fresh.bms");
	TEST_EXPECT(fresh && fresh->names_file().found == MissionNamesFile::Found::None);
	if (!fresh) return 1;
	Edit hidden;
	hidden.address = {fresh->mission_row()->id, fresh->mission_row()->kind, 0};
	hidden.field = "start_time";
	hidden.value = int64_t(600);
	p.run(request::edit_record("missions/fresh.bms", hidden));
	p.run(request::save("missions/fresh.bms"));
	TEST_EXPECT(!p.exists("missions/fresh.mis"));
	p.name_event("missions/fresh.bms", 0, "Start");
	p.run(request::save("missions/fresh.bms"));
	TEST_EXPECT(p.names_in("missions/fresh.mis").events == std::vector<std::string>({"Start", ""}));
	return 0;
}

// The original editor's .mis through the session (B1): its names read, a save of the open mission and a closed
// file's rewrite both leave it byte for byte; the editor's own one beside a closed mission is written again.
int test_names_file_original() {
	NamesProject p("opennova_mission_names_original");
	const SessionView &v = p.view();
	bms::File plain = names_mission();
	std::string error, mis_text;
	const mission::MisNames names = file_names();
	TEST_EXPECT(mission::write_mis_text(plain, mis_text, error, nullptr, &names));
	const std::string original = original_editors(mis_text);
	std::vector<uint8_t> bms_bytes;
	TEST_EXPECT(bms::write(plain, bms_bytes, error));
	TEST_EXPECT(p.put("missions/orig.bms", bms_bytes) && p.put("missions/orig.mis", original));
	// A closed mission whose save rewrites it (a path record listing a stop no marker carries: path_rebuilt).
	bms::File rebuilt = plain;
	bms::WaypointRecord &record = rebuilt.waypoint_records[3];
	TEST_EXPECT(record.waypoint_numbers.empty() && record.padding.size() == 128);
	record.waypoint_numbers.push_back(5); // a slot word out of the record's 128 bytes
	record.padding.erase(record.padding.begin(), record.padding.begin() + 4);
	record.marker_count = 1;
	std::vector<uint8_t> rebuilt_bytes;
	TEST_EXPECT(bms::write(rebuilt, rebuilt_bytes, error));
	TEST_EXPECT(p.put("missions/closed.bms", rebuilt_bytes) && p.put("missions/closed.mis", original));
	TEST_EXPECT(p.put("missions/ours.bms", rebuilt_bytes) && p.put("missions/ours.mis", mis_text));
	p.run(request::rescan());
	p.run(request::open_document("missions/orig.bms"));
	const MissionDocument *m = p.mission("missions/orig.bms");
	TEST_EXPECT(m && m->names_file().found == MissionNamesFile::Found::Matched && !m->names_file().ours);
	if (!m) return 1;
	TEST_EXPECT(same(DocumentMissionNames(*m).event(0), "Get to the bridge"));
	p.name_event("missions/orig.bms", 1, "Hold the bridge");
	TEST_EXPECT(finding_of(v, "mission.names_foreign", "missions/orig.bms") != nullptr);
	p.run(request::save("missions/orig.bms"));
	TEST_EXPECT(p.text("missions/orig.mis") == original && p.session.document_for("missions/orig.bms")->companion_unsaved());
	TEST_EXPECT(finding_of(v, "mission.names_foreign", "missions/orig.bms") != nullptr);
	// Its names alone unsaved, the file gone since: Save writes the mission's (a save of names alone, the
	// listing gone with it).
	std::error_code gone;
	std::filesystem::remove(system_path(p.root + "/missions/orig.mis"), gone);
	TEST_EXPECT(!p.session.document_for("missions/orig.bms")->dirty());
	p.run(request::save("missions/orig.bms"));
	TEST_EXPECT(p.names_in("missions/orig.mis").events == std::vector<std::string>({"Get to the bridge", "Hold the bridge"}) &&
	            !p.session.document_for("missions/orig.bms")->unsaved() &&
	            !finding_of(v, "mission.names_foreign", "missions/orig.bms"));
	// The closed files' saves: the original editor's left, the editor's own written again from the mission.
	p.run(request::save("missions/closed.bms"));
	if (test_io::read_file(p.root + "/missions/closed.bms") == rebuilt_bytes) std::printf("  status: %s\n", v.activity.status.c_str());
	TEST_EXPECT(test_io::read_file(p.root + "/missions/closed.bms") != rebuilt_bytes);
	TEST_EXPECT(p.text("missions/closed.mis") == original);
	// The editor's own: its bytes the same (the same names, no terrain), its last write moved (it was written).
	namespace fs = std::filesystem;
	std::error_code stamp_error;
	const fs::path ours_mis = system_path(p.root + "/missions/ours.mis");
	const fs::file_time_type long_ago = fs::last_write_time(ours_mis, stamp_error) - std::chrono::hours(48);
	fs::last_write_time(ours_mis, long_ago, stamp_error);
	p.run(request::save("missions/ours.bms"));
	TEST_EXPECT(test_io::read_file(p.root + "/missions/ours.bms") != rebuilt_bytes &&
	            p.names_in("missions/ours.mis").events == std::vector<std::string>({"Get to the bridge", ""}) &&
	            p.text("missions/ours.mis") == mis_text && fs::last_write_time(ours_mis, stamp_error) > long_ago);
	// The closed original editor's .mis was left: its stamp is the one it had.
	const fs::path closed_mis = system_path(p.root + "/missions/closed.mis");
	const fs::file_time_type closed_at = fs::last_write_time(closed_mis, stamp_error);
	p.run(request::save("missions/closed.bms"));
	TEST_EXPECT(fs::last_write_time(closed_mis, stamp_error) == closed_at);
	// Our own write of an open mission's .mis is no change outside the editor: a look for changes reads nothing again.
	p.run(request::open_document("missions/ours.bms"));
	const DocumentBase *open = p.session.document_for("missions/ours.bms");
	TEST_EXPECT(open != nullptr);
	if (!open) return 1;
	const uint64_t loaded = open->load_generation();
	p.name_event("missions/ours.bms", 1, "Hold the bridge");
	p.run(request::save("missions/ours.bms"));
	p.run(request::rescan());
	TEST_EXPECT(p.session.document_for("missions/ours.bms") == open && open->load_generation() == loaded &&
	            !open->unsaved() && !finding_of(v, "document.conflict", "missions/ours.bms") &&
	            same(DocumentMissionNames(*p.mission("missions/ours.bms")).event(1), "Hold the bridge"));
	return 0;
}

// A mission whose file another program rewrote (M2): read again, its events no longer the .mis's, nothing taken and
// nothing written over by a later save; two missions of one name in two folders each with the .mis beside it (m4);
// move, duplicate and delete taking the .mis with the mission.
int test_names_file_chores() {
	NamesProject p("opennova_mission_names_chores");
	const SessionView &v = p.view();
	const bms::File plain = names_mission();
	std::string error, mis_text;
	const mission::MisNames names = file_names();
	TEST_EXPECT(mission::write_mis_text(plain, mis_text, error, nullptr, &names));
	std::vector<uint8_t> bms_bytes, other_bytes;
	TEST_EXPECT(bms::write(plain, bms_bytes, error) && bms::write(other_mission(plain), other_bytes, error));
	TEST_EXPECT(p.put("missions/a/x.bms", bms_bytes) && p.put("missions/a/x.mis", mis_text) &&
	            p.put("missions/b/x.bms", bms_bytes));
	p.run(request::rescan());
	// Each its own: b's mission has none beside it; its name saved makes b's .mis, a's as it was.
	p.run(request::open_document("missions/b/x.bms"));
	const MissionDocument *b = p.mission("missions/b/x.bms");
	TEST_EXPECT(b && b->names_file().found == MissionNamesFile::Found::None);
	p.name_event("missions/b/x.bms", 0, "Elsewhere");
	p.run(request::save("missions/b/x.bms"));
	TEST_EXPECT(p.names_in("missions/b/x.mis").events == std::vector<std::string>({"Elsewhere", ""}) &&
	            p.text("missions/a/x.mis") == mis_text);
	// a's mission read again after another program rewrote it (its events no longer the .mis's): nothing taken, a
	// save of another edit leaves the .mis, listed with Take and Write.
	p.run(request::open_document("missions/a/x.bms"));
	TEST_EXPECT(p.mission("missions/a/x.bms") && same(DocumentMissionNames(*p.mission("missions/a/x.bms")).event(0), "Get to the bridge"));
	TEST_EXPECT(p.put("missions/a/x.bms", other_bytes));
	p.run(request::rescan());
	const MissionDocument *a = p.mission("missions/a/x.bms");
	TEST_EXPECT(a && a->names_file().found == MissionNamesFile::Found::Unmatched && a->names_file().ours &&
	            same(DocumentMissionNames(*a).event(0), "event 1"));
	if (!a) return 1;
	Edit hidden;
	hidden.address = {a->mission_row()->id, a->mission_row()->kind, 0};
	hidden.field = "start_time";
	hidden.value = int64_t(600);
	p.run(request::edit_record("missions/a/x.bms", hidden));
	p.run(request::save("missions/a/x.bms"));
	TEST_EXPECT(p.text("missions/a/x.mis") == mis_text);
	const Diagnostic *listed = finding_of(v, "mission.names_unmatched", "missions/a/x.bms");
	TEST_EXPECT(listed && listed->planned.size() == 2);
	p.run(request::close_document("missions/a/x.bms"));
	p.run(request::close_document("missions/b/x.bms"));
	// Moved, duplicated, deleted: the .mis goes with the mission.
	std::error_code made;
	std::filesystem::create_directories(system_path(p.root + "/missions/c"), made);
	p.run(request::rescan());
	p.run(request::move_asset("missions/a/x.bms", "missions/c"));
	TEST_EXPECT(p.exists("missions/c/x.bms") && p.exists("missions/c/x.mis") && !p.exists("missions/a/x.mis"));
	p.run(request::duplicate_asset("missions/c/x.bms", "y.bms"));
	TEST_EXPECT(p.exists("missions/c/y.bms") && p.exists("missions/c/y.mis") && p.text("missions/c/y.mis") == mis_text);
	p.run(request::delete_asset("missions/c/y.bms", true));
	TEST_EXPECT(!p.exists("missions/c/y.bms") && !p.exists("missions/c/y.mis") && p.exists("missions/c/x.mis"));
	// A mission at the top level has none beside it though a folder holds a .mis of its name (m4): never that one.
	TEST_EXPECT(p.put("x.bms", bms_bytes));
	p.run(request::rescan());
	p.run(request::open_document("x.bms"));
	TEST_EXPECT(p.mission("x.bms") && p.mission("x.bms")->names_file().found == MissionNamesFile::Found::None);
	p.run(request::close_document("x.bms"));
	// A move onto a .mis of its name, none going with it, is refused: that file would hold its names.
	TEST_EXPECT(p.put("missions/d/z.mis", mis_text) && p.put("missions/z.bms", bms_bytes));
	p.run(request::rescan());
	const ActionOutcome moved = editor_test::handle_to_end(p.session, request::move_asset("missions/z.bms", "missions/d"));
	const bool said = std::any_of(moved.findings.begin(), moved.findings.end(), [](const Diagnostic &d) {
		return d.message.find("missions/d/z.mis, which would hold z.bms's names") != std::string::npos;
	});
	if (!said)
		for (const Diagnostic &d : moved.findings) std::printf("  refused: %s\n", d.message.c_str());
	TEST_EXPECT(moved.refused && said && p.exists("missions/z.bms") && !p.exists("missions/d/z.bms"));
	return 0;
}

// The paths that close a mission and open it again, or read it again, each ask first where its names are unsaved
// (a save left them out of the original editor's .mis beside it: N1): a folder renamed, several files moved, a move
// taken back, a rename taken back, a rename while the validation runs, a rename of a file the mission names; and
// Quit and a project closed.
int test_names_unsaved_paths() {
	NamesProject p("opennova_mission_names_unsaved");
	const SessionView &v = p.view();
	bms::File plain = names_mission();
	std::string error, mis_text;
	TEST_EXPECT(mission::set_header_string(plain, "terrain", "Tmap", error));
	const mission::MisNames names = file_names();
	TEST_EXPECT(mission::write_mis_text(plain, mis_text, error, nullptr, &names));
	const std::string original = original_editors(mis_text);
	std::vector<uint8_t> bms_bytes;
	TEST_EXPECT(bms::write(plain, bms_bytes, error));
	const std::string tmap = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/terrain/tmap/";
	TEST_EXPECT(p.put("Tmap.trn", test_io::read_file(tmap + "Tmap.trn")) && p.put("Tmap.cpt", test_io::read_file(tmap + "Tmap.cpt")));
	for (const char *mission_path : {"missions/f/a.bms", "missions/b.bms", "missions/u.bms", "missions/r.bms"}) {
		const std::string bms_path = mission_path;
		TEST_EXPECT(p.put(bms_path, bms_bytes) && p.put(bms_path.substr(0, bms_path.size() - 4) + ".mis", original));
	}
	TEST_EXPECT(p.put("missions/c.bms", bms_bytes) && p.put("missions/v.bms", bms_bytes));
	std::error_code made;
	std::filesystem::create_directories(system_path(p.root + "/missions/h"), made);
	p.run(request::rescan());
	// A mission's names left unsaved: one renamed and saved, the original editor's .mis left as it is.
	const auto leave_names = [&](const std::string &path) {
		p.run(request::open_document(path));
		if (!p.mission(path)) return false;
		p.name_event(path, 1, "Hold the bridge");
		p.run(request::save(path));
		const DocumentBase *open = p.session.document_for(path);
		return open && !open->dirty() && open->companion_unsaved();
	};
	// The prompt open over the request, naming the mission; then cancelled.
	const auto asks = [&](const std::string &path) {
		const DialogsView::UnsavedPrompt &prompt = v.dialogs.unsaved_prompt;
		const bool named = prompt.open && std::find(prompt.files.begin(), prompt.files.end(), path) != prompt.files.end();
		if (!named) std::printf("  no prompt naming %s (status: %s)\n", path.c_str(), v.activity.status.c_str());
		if (prompt.open) p.run(request::resolve_unsaved(UnsavedChoice::Cancel));
		return named;
	};
	// A rename taken back: the rename of a clean mission, its names then left unsaved, Rename back.
	p.run(request::rename_asset("missions/r.bms", "r2.bms"));
	TEST_EXPECT(p.exists("missions/r2.bms") && p.exists("missions/r2.mis"));
	TEST_EXPECT(leave_names("missions/r2.bms"));
	p.run(request::rename_back());
	TEST_EXPECT(asks("missions/r2.bms") && p.exists("missions/r2.bms"));
	// A folder renamed.
	TEST_EXPECT(leave_names("missions/f/a.bms"));
	p.run(request::rename_folder("missions/f", "g"));
	TEST_EXPECT(asks("missions/f/a.bms") && p.exists("missions/f/a.bms"));
	// Several files moved together.
	TEST_EXPECT(leave_names("missions/b.bms"));
	p.run(request::move_assets("missions/b.bms", {"missions/c.bms"}, "missions/h"));
	TEST_EXPECT(asks("missions/b.bms") && p.exists("missions/b.bms"));
	// A move taken back: a clean mission moved with another file (a file chore), its names then left unsaved, file Undo.
	p.run(request::move_assets("missions/u.bms", {"missions/v.bms"}, "missions/h"));
	TEST_EXPECT(p.exists("missions/h/u.bms") && p.exists("missions/h/u.mis"));
	TEST_EXPECT(leave_names("missions/h/u.bms"));
	p.run(request::undo_file());
	TEST_EXPECT(asks("missions/h/u.bms") && p.exists("missions/h/u.bms"));
	// A rename of the terrain the mission names: the mission is read again with its new name.
	p.run(request::rename_asset("Tmap.trn", "Tmap2.trn"));
	TEST_EXPECT(asks("missions/b.bms") && p.exists("Tmap.trn"));
	// A rename while the validation runs (an edit just made, not validated yet): every unsaved mission is named.
	p.run(request::open_document("missions/c.bms"));
	p.session.handle(request::edit_record("missions/c.bms", set_of(event_at(*p.mission("missions/c.bms"), 0), "name", "Start")));
	p.session.handle(request::rename_asset("missions/c.bms", "c2.bms"));
	TEST_EXPECT(asks("missions/b.bms"));
	p.session.run_operations();
	// Quit, and the project closed.
	p.run(request::quit());
	TEST_EXPECT(asks("missions/b.bms"));
	p.run(request::close_project());
	TEST_EXPECT(asks("missions/b.bms") && v.project.open);
	return 0;
}

// Files by name, each put with a stamp of its own (the ground's files in a test).
class MemoryFiles : public opennova::FileSource {
public:
	void put(const std::string &name, std::vector<uint8_t> bytes) {
		files_[opennova::strutil::to_lower(name)] = {std::move(bytes), ++serial_};
	}
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		const auto found = files_.find(opennova::strutil::to_lower(name));
		if (found == files_.end()) return false;
		out = found->second.first;
		return true;
	}
	uint64_t stamp(const std::string &name) const override {
		const auto found = files_.find(opennova::strutil::to_lower(name));
		return found == files_.end() ? 0 : found->second.second;
	}

private:
	std::map<std::string, std::pair<std::vector<uint8_t>, uint64_t>> files_;
	uint64_t serial_ = 0;
};

// Each entity's height in the .mis a save writes is taken off the terrain's height under it, as the original editor
// samples it over the mission's terrain (M4: mission::mis_terrain_height, the .cpt's atlas the game loads blended
// bilinearly over the .trn's own sector grid): every pool's, a marker's and an itemless entity's too, each unlocked
// (D-MIS-4: its z less the height, its base as it stands, no height_lock line). Where the terrain does not read, each
// is written locked at its z (D-MIS-17).
int test_names_base_heights() {
	NamesProject p("opennova_mission_names_heights");
	const std::string tmap = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/terrain/tmap/";
	const std::vector<uint8_t> trn = test_io::read_file(tmap + "Tmap.trn"), cpt = test_io::read_file(tmap + "Tmap.cpt");
	TEST_EXPECT(!trn.empty() && !cpt.empty() && p.put("Tmap.trn", trn) && p.put("Tmap.cpt", cpt));
	// The terrain as the original editor reads it: the .cpt's atlas as the session's ground reads it, the .trn's own
	// sector grid, origin and wraps.
	auto files = std::make_shared<MemoryFiles>();
	files->put("Tmap.trn", trn);
	files->put("Tmap.cpt", cpt);
	MissionGround ground;
	MissionSceneHeader header;
	header.terrain = "Tmap";
	ground.follow(files, 1, header, "missions/heights");
	TEST_EXPECT(ground.terrain() && ground.height_field()->dim == 1024);
	if (!ground.terrain()) return 1;
	opennova::TrnConfig config;
	std::string trn_error;
	TEST_EXPECT(opennova::load_mission_trn(std::string(trn.begin(), trn.end()), opennova::TrnLaterTexts{}, config, trn_error));
	mission::MisTerrain terrain;
	terrain.heights = ground.height_field()->heightmap;
	terrain.sectors = &config.sector_grid[0][0];
	terrain.origin_x = config.origin_x;
	terrain.origin_y = config.origin_y;
	terrain.wrap_x = config.wrap_x != 0;
	terrain.wrap_y = config.wrap_y != 0;
	// Four places on the island (about the origin) inside their texels, so each blends four, each at a height of its
	// own, and each where the original editor's blend and the game's column (world::terrain_column_height) round
	// apart: the same atlas, bilinear both, a few 16.16 units apart in about a quarter of the island's places.
	std::vector<std::pair<int32_t, int32_t>> spots;
	std::vector<int32_t> seen;
	for (double y = -300.61; y <= 300.0 && spots.size() < 4; y += 25.0)
		for (double x = -300.37; x <= 300.0 && spots.size() < 4; x += 25.0) {
			const int32_t fx = bms::to_fixed_16_16(x), fy = bms::to_fixed_16_16(y);
			const int32_t h = mission::mis_terrain_height(terrain, fx, fy);
			int32_t column = 0;
			if (h == 0 || std::find(seen.begin(), seen.end(), h) != seen.end() ||
			    !opennova::world::terrain_column_height(ground.height_field(), fx, fy, column) || column == h)
				continue;
			seen.push_back(h);
			spots.emplace_back(fx, fy);
		}
	TEST_EXPECT(spots.size() == 4);
	if (spots.size() != 4) return 1;
	bms::File file;
	std::string error;
	mission::BlankMission blank;
	blank.name = "Heights";
	blank.terrain = "Tmap";
	TEST_EXPECT(mission::make_blank(file, blank, error));
	const mission::EntityKind kinds[] = {mission::EntityKind::Item, mission::EntityKind::Building, mission::EntityKind::Marker,
	                                     mission::EntityKind::Organic};
	for (size_t i = 0; i < 4; ++i) {
		bms::Entity entity = mission::new_entity(kinds[i], i == 3 ? 0 : 106102, int32_t(i) + 1);
		entity.x = spots[i].first;
		entity.y = spots[i].second;
		entity.z = bms::to_fixed_16_16(1.0);
		mission::entities(file, kinds[i])->push_back(entity);
	}
	mission::add_event(file, mission::MissionEventRecord{});
	mission::sync_counts(file);
	std::vector<uint8_t> bytes;
	TEST_EXPECT(bms::write(file, bytes, error) && p.put("missions/heights.bms", bytes));
	p.run(request::rescan());
	p.run(request::open_document("missions/heights.bms"));
	TEST_EXPECT(p.mission("missions/heights.bms") != nullptr);
	if (!p.mission("missions/heights.bms")) return 1;
	p.name_event("missions/heights.bms", 0, "Start");
	p.run(request::save("missions/heights.bms"));
	// The items' sections in the writer's order (items, buildings, markers, organics), each its position's z, its
	// extra_bheight and no height_lock (the reader sorts an item into its pool by its type, not by the section's
	// place).
	const auto heights_of = [](const std::string &text, std::vector<int64_t> &zs, std::vector<int64_t> &bases) {
		zs.clear();
		bases.clear();
		for (size_t at = 0; (at = text.find("\n  position ", at)) != std::string::npos; ++at) {
			const std::string line = text.substr(at + 1, text.find('\r', at + 1) - at - 1);
			zs.push_back(std::stoll(line.substr(line.rfind(' ') + 1)));
		}
		for (size_t at = 0; (at = text.find("\n  extra_bheight ", at)) != std::string::npos; ++at)
			bases.push_back(std::stoll(text.substr(at + 17, text.find('\r', at) - at - 17)));
	};
	std::string text = p.text("missions/heights.mis");
	std::vector<int64_t> zs, bases;
	heights_of(text, zs, bases);
	TEST_EXPECT(zs.size() == 4 && bases.size() == 4 && text.find("height_lock") == std::string::npos);
	if (zs.size() != 4 || bases.size() != 4) return 1;
	// Pinned: the original editor's blend over Tmap there (its sampler's, not the game's column).
	const int64_t pinned[4] = {5079735, 6308535, 4977335, 6206135};
	for (size_t i = 0; i < 4; ++i) {
		const int64_t under = int64_t(bms::to_fixed_16_16(1.0)) - zs[i];
		if (under != pinned[i]) std::printf("  entity %zu: the terrain's height %lld\n", i, static_cast<long long>(under));
		TEST_EXPECT(under == mission::mis_terrain_height(terrain, spots[i].first, spots[i].second) && under == pinned[i] &&
		            bases[i] == 0);
	}
	// The terrain gone: no height read, each entity written locked at its z.
	TEST_EXPECT(p.put("Tmap.trn", std::string("garbage")));
	p.run(request::rescan());
	p.name_event("missions/heights.bms", 0, "Started");
	p.run(request::save("missions/heights.bms"));
	text = p.text("missions/heights.mis");
	heights_of(text, zs, bases);
	size_t locks = 0;
	for (size_t at = 0; (at = text.find("\n  height_lock 1\r", at)) != std::string::npos; ++at) ++locks;
	TEST_EXPECT(zs.size() == 4 && locks == 4);
	for (size_t i = 0; i < zs.size(); ++i) TEST_EXPECT(zs[i] == bms::to_fixed_16_16(1.0) && bases[i] == 0);
	return 0;
}

// The name fields and Duplicate's wording: path 0 takes no description (m3); a Duplicate refused by a .mis of the
// copy's name says it would hold the copy's names, never that the game would take it (m8).
int test_names_refusals() {
	std::unique_ptr<Document> document = open_mission();
	TEST_EXPECT(document != nullptr);
	if (!document) return 1;
	const MissionDocument &m = dynamic_cast<const MissionDocument &>(*document);
	const std::vector<const Node *> paths = m.rows_of(MissionKind::WaypointPath);
	TEST_EXPECT(!paths.empty() && static_cast<const PathRow &>(*paths[0]).native.number == 0);
	if (paths.empty()) return 1;
	Diagnostic refused;
	TEST_EXPECT(!document->apply({set_of({paths[0]->id, paths[0]->kind, 0}, "description", "Nowhere")}, refused) &&
	            refused.message.find("Path 0 is no path") != std::string::npos);
	TEST_EXPECT(apply_edits(*document, {set_of({paths[1]->id, paths[1]->kind, 0}, "description", "Somewhere")}));

	NamesProject p("opennova_mission_names_refusals");
	const bms::File plain = names_mission();
	std::string error;
	std::vector<uint8_t> bms_bytes;
	TEST_EXPECT(bms::write(plain, bms_bytes, error) && p.put("missions/x.bms", bms_bytes) && p.put("missions/y.mis", "stray"));
	p.run(request::rescan());
	p.run(request::duplicate_asset("missions/x.bms", "y.bms"));
	TEST_EXPECT(!p.exists("missions/y.bms") && p.view().activity.status.find("which would hold y.bms's names") != std::string::npos);
	if (p.view().activity.status.find("which would hold") == std::string::npos)
		std::printf("  status: %s\n", p.view().activity.status.c_str());
	return 0;
}
} // namespace

int main() {
	if (test_variable_words() != 0) return 1;
	if (test_live_logic() != 0) return 1;
	if (test_group_names() != 0) return 1;
	if (test_event_section() != 0) return 1;
	if (test_variable_names() != 0) return 1;
	if (test_variables_wire() != 0) return 1;
	if (test_live_session() != 0) return 1;
	if (test_row_names() != 0) return 1;
	if (test_names_file() != 0) return 1;
	if (test_names_file_entities() != 0) return 1;
	if (test_names_clipboard() != 0) return 1;
	if (test_names_file_session() != 0) return 1;
	if (test_names_file_original() != 0) return 1;
	if (test_names_file_chores() != 0) return 1;
	if (test_names_base_heights() != 0) return 1;
	if (test_names_refusals() != 0) return 1;
	if (test_names_unsaved_paths() != 0) return 1;
	std::printf("editor_mission_names: all tests passed\n");
	return 0;
}
