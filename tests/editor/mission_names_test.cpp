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
#include <base/io/json.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/activity_view.h>
#include <formats/mission/bms.h>

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

} // namespace

int main() {
	if (test_variable_words() != 0) return 1;
	if (test_live_logic() != 0) return 1;
	if (test_group_names() != 0) return 1;
	if (test_variable_names() != 0) return 1;
	if (test_variables_wire() != 0) return 1;
	std::printf("editor_mission_names: all tests passed\n");
	return 0;
}
