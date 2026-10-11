// A mission's names (ADR 0046 S24, Names and Live). S0: a mission variable's base words are the script's name for
// it ("V12"), a trigger and an action reading a variable word it through MissionNames::variable (a name the
// mission gives it overriding them); and the live logic the session holds while Play runs is empty until a game
// reports (documents/mission_live.h), an event found by its index. C2/C3 over the minted synth_logic mission: a
// group named in its record's dropped bytes and a variable named in the tagged layer table, as the document's
// words, labels and values say them; the edits that name a variable (one batch), the 31 the table takes, a table
// of the original editor's layer names never written over, and a tagged table's records never edited as layer
// names (read-only, refused).
#include <cstdio>
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
#include <base/io/json.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/live_logic.h>
#include <editor/session/live_logic_link.h>
#include <editor/session/request_factories.h>
#include <editor/session/view_json.h>
#include <editor/session/view/activity_view.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>

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

} // namespace

int main() {
	if (test_variable_words() != 0) return 1;
	if (test_live_logic() != 0) return 1;
	if (test_group_names() != 0) return 1;
	if (test_event_section() != 0) return 1;
	if (test_variable_names() != 0) return 1;
	if (test_variables_wire() != 0) return 1;
	if (test_live_session() != 0) return 1;
	std::printf("editor_mission_names: all tests passed\n");
	return 0;
}
