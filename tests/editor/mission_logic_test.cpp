// Adding and editing a mission's logic without its format (editor/documents/mission_logic.h,
// mission_uses.h, ADR 0046 S15, Events and scripts), over the minted synth_logic mission: a trigger's
// and an action's form (its type by name, the parameters its type reads with their plain labels and
// their values in words, the words and the event's sentence), an event's form (its parts, flags,
// steps and the limits); an add by type with its kinds' defaults, one batch the sentence then reads;
// the 20 an event holds refused before the edit, in words; a retype keeping what still applies; a
// move within an event and to another (one undo step); a negation and a join; what names a record
// (an entity, an area, an event, a group, a path; a second holder of an SSN said inert); and the
// `mission_logic` and `mission_uses` queries over a session. With the game install (a SKIP-LEG without
// OPENNOVA_JO_DIR), every shipped trigger and action given a form and retyped to its own type planning
// nothing, and the value each value kind's records hold most often measured: the defaults a new
// record's parameters take.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_logic.h>
#include <editor/documents/mission_sentence.h>
#include <editor/documents/mission_uses.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <formats/mission/bms.h>
#include <formats/mission/mission_params.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace bms = opennova::bms;
namespace mission = opennova::mission;
using opennova::io::JsonValue;

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

bool same(const std::string &got, const std::string &want) {
	if (got != want) std::printf("  got:  %s\n  want: %s\n", got.c_str(), want.c_str());
	return got == want;
}

std::vector<uint8_t> fixture_bytes() {
	return test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_logic.bms");
}

std::unique_ptr<Document> open(const std::vector<uint8_t> &bytes, const char *name = "synth_logic.bms") {
	const DocumentType *type = document_type_for(AssetKind::Mission);
	if (!type) return nullptr;
	std::unique_ptr<Document> document = records_of(type->make());
	Diagnostic error;
	if (!document || !document->load_bytes(bytes, name, AssetKind::Mission, "jo", error)) return nullptr;
	return document;
}

const MissionDocument &as_mission(const Document &document) { return dynamic_cast<const MissionDocument &>(document); }

// The minted mission's records: its two events, event 1's trigger and action, the walker.
struct Fixture {
	std::unique_ptr<Document> document;
	NodeAddress first, second, trigger, action, walker, zone20, zone30;
	int32_t ssn = 0;
	bool load() {
		document = open(fixture_bytes());
		if (!document) return false;
		const MissionDocument &m = as_mission(*document);
		refresh();
		const std::vector<const Node *> organics = m.rows_of(MissionKind::Organic), areas = m.rows_of(MissionKind::Area);
		walker = {organics[0]->id, organics[0]->kind, 0};
		ssn = static_cast<const EntityRow &>(*organics[0]).native.id;
		zone20 = {areas[0]->id, areas[0]->kind, 0};
		zone30 = {areas[1]->id, areas[1]->kind, 0};
		return true;
	}
	void refresh() {
		const MissionDocument &m = as_mission(*document);
		const std::vector<const Node *> events = m.rows_of(MissionKind::Event);
		first = {events[0]->id, events[0]->kind, 0};
		second = {events[1]->id, events[1]->kind, 0};
		const std::vector<Document::Collection> lists = m.collections_of(first);
		trigger = lists[0].ids.empty() ? NodeAddress() : NodeAddress{first.row, k(MissionKind::Trigger), lists[0].ids[0]};
		action = lists[1].ids.empty() ? NodeAddress() : NodeAddress{first.row, k(MissionKind::Action), lists[1].ids[0]};
	}
	bool apply(const std::vector<Edit> &edits) {
		Diagnostic error;
		const bool ok = document->apply(edits, error);
		if (!ok) std::printf("  refused: %s\n", error.message.c_str());
		refresh();
		return ok;
	}
	size_t count(const NodeAddress &event, MissionKind kind) const {
		for (const Document::Collection &list : document->collections_of(event))
			if (list.spec.kind == k(kind)) return list.ids.size();
		return 0;
	}
};

int test_forms() {
	Fixture f;
	TEST_EXPECT(f.load());
	const MissionDocument &m = as_mission(*f.document);
	const DocumentMissionNames names(m);
	const std::string walker = "Organic #" + std::to_string(f.ssn);
	LogicForm form;
	TEST_EXPECT(logic_form(m, f.trigger, names, form));
	TEST_EXPECT(!form.action && form.type && same(form.type_words, "Entity is in an area") && same(form.type->group, "Areas and waypoints"));
	TEST_EXPECT(!form.negated && form.join == LogicJoin::And && form.last && form.index == 0 && form.count == 1);
	// The parameters the type reads, labelled and named; none it does not read holds a value.
	TEST_EXPECT(form.params.size() == 2 && form.unread.empty());
	TEST_EXPECT(form.params[0].field == "param1" && form.params[0].kind == mission::ParamKind::Entity &&
	            same(form.params[0].label, "Entity") && same(form.params[0].words, walker) && form.params[0].picks);
	TEST_EXPECT(form.params[1].kind == mission::ParamKind::Zone && same(form.params[1].label, "Zone") &&
	            same(form.params[1].words, "Zone 20"));
	TEST_EXPECT(same(form.words, walker + " is in Zone 20"));
	TEST_EXPECT(same(form.sentence, "When " + walker + " is in Zone 20, then re-arm event 2."));
	TEST_EXPECT(logic_form(m, f.action, names, form));
	TEST_EXPECT(form.action && same(form.type_words, "Re-arm event") && form.params.size() == 1 &&
	            same(form.params[0].words, "event 2"));
	// Neither an event nor an entity has a record form.
	TEST_EXPECT(!logic_form(m, f.first, names, form) && !logic_form(m, f.walker, names, form));
	// The event's form: its parts, its flags, its steps and what it holds of the most.
	LogicEventForm event;
	TEST_EXPECT(logic_event_form(m, f.second.row, names, event));
	TEST_EXPECT(event.index == 1 && same(event.words.when, "When event 1 has fired") && same(event.words.delay, "after 5.1 s"));
	TEST_EXPECT(!event.repeats && !event.at_start && !event.at_end && event.delay == 5 && event.repeat == 0);
	TEST_EXPECT(event.most_steps == 1023 && event.triggers == 1 && event.actions == 1 && event.most == 20);
	TEST_EXPECT(event.trigger_refusal.empty() && event.action_refusal.empty());
	return 0;
}

int test_add_and_limit() {
	Fixture f;
	TEST_EXPECT(f.load());
	const MissionDocument &m = as_mission(*f.document);
	// "Entity is destroyed" added to event 2: an Add with its type, its sub-type; one batch, one step.
	const LogicType *destroyed = logic_type(false, int32_t(bms::TriggerMainType::Single), 4);
	TEST_EXPECT(destroyed);
	std::vector<Edit> edits;
	std::string refusal;
	TEST_EXPECT(logic_add_edits(m, f.second.row, *destroyed, SIZE_MAX, edits, refusal));
	TEST_EXPECT(edits.size() == 2 && edits[0].operation == EditOperation::Add && edits[0].field == "main_type" &&
	            edits[1].address.child == batch_made(0) && edits[1].field == "sub_type");
	TEST_EXPECT(f.apply(edits));
	TEST_EXPECT(f.count(f.second, MissionKind::Trigger) == 2);
	TEST_EXPECT(same(m.record_title(f.second),
	                 "When event 1 has fired and SSN 0 (no entity has it) is destroyed, then, after 5.1 s, kill group 2."));
	TEST_EXPECT(f.document->can_undo());
	// A sub-goal's action takes its kinds' defaults: the first sub-goal.
	const LogicType *won = logic_type(true, int32_t(bms::ActionType::SubGoalWon), 0);
	TEST_EXPECT(won && logic_add_edits(m, f.second.row, *won, 0, edits, refusal));
	TEST_EXPECT(f.apply(edits));
	TEST_EXPECT(same(m.record_title(f.second),
	                 "When event 1 has fired and SSN 0 (no entity has it) is destroyed, then, after 5.1 s, win sub-goal 1; kill "
	                 "group 2."));
	// Twenty triggers: the event's form and the add say so before the edit.
	const LogicType *alive = logic_type(false, int32_t(bms::TriggerMainType::Single), 5);
	for (size_t i = f.count(f.first, MissionKind::Trigger); i < 20; ++i) {
		TEST_EXPECT(logic_add_edits(m, f.first.row, *alive, SIZE_MAX, edits, refusal));
		TEST_EXPECT(f.apply(edits));
	}
	TEST_EXPECT(f.count(f.first, MissionKind::Trigger) == 20);
	const std::string full = logic_add_refusal(m, f.first.row, false);
	TEST_EXPECT(full.find("holds 20 triggers, the most an event holds") != std::string::npos);
	TEST_EXPECT(logic_add_refusal(m, f.first.row, true).empty());
	TEST_EXPECT(!logic_add_edits(m, f.first.row, *alive, SIZE_MAX, edits, refusal) && same(refusal, full));
	LogicEventForm event;
	TEST_EXPECT(logic_event_form(m, f.first.row, DocumentMissionNames(m), event) && same(event.trigger_refusal, full));
	return 0;
}

int test_retype_move_negate() {
	Fixture f;
	TEST_EXPECT(f.load());
	const MissionDocument &m = as_mission(*f.document);
	const std::string walker = "Organic #" + std::to_string(f.ssn);
	std::vector<Edit> edits;
	std::string refusal;
	// Entity is in an area -> Entity is destroyed: the entity kept, the zone (unread now) cleared.
	const LogicType *destroyed = logic_type(false, int32_t(bms::TriggerMainType::Single), 4);
	TEST_EXPECT(logic_retype_edits(m, f.trigger, *destroyed, edits, refusal));
	TEST_EXPECT(edits.size() == 2 && edits[0].field == "sub_type" && edits[1].field == "param2" && edits[1].value == Value(int64_t(0)));
	TEST_EXPECT(f.apply(edits));
	TEST_EXPECT(same(m.record_title(f.trigger), walker + " is destroyed"));
	// To its own type: nothing.
	TEST_EXPECT(logic_retype_edits(m, f.trigger, *destroyed, edits, refusal) && edits.empty());
	// -> Group is wiped out: the entity is no group, so the first parameter takes the group's default.
	const LogicType *wiped = logic_type(false, int32_t(bms::TriggerMainType::Group), 4);
	TEST_EXPECT(logic_retype_edits(m, f.trigger, *wiped, edits, refusal));
	TEST_EXPECT(f.apply(edits) && same(m.record_title(f.trigger), "no group has no unit left alive"));
	TEST_EXPECT(f.document->can_undo());
	f.document->undo();
	f.document->undo();
	f.refresh();
	TEST_EXPECT(same(m.record_title(f.trigger), walker + " is in Zone 20"));
	// Negated, then joined by or to a second trigger.
	Edit edit;
	TEST_EXPECT(logic_negate_edit(m, f.trigger, true, edit) && f.apply({edit}));
	TEST_EXPECT(same(m.record_title(f.trigger), walker + " is not in Zone 20"));
	const LogicType *alive = logic_type(false, int32_t(bms::TriggerMainType::Single), 5);
	TEST_EXPECT(logic_add_edits(m, f.first.row, *alive, SIZE_MAX, edits, refusal) && f.apply(edits));
	TEST_EXPECT(logic_join_edit(m, f.trigger, LogicJoin::Or, edit) && f.apply({edit}));
	TEST_EXPECT(same(m.record_title(f.first), "When " + walker + " is not in Zone 20 or SSN 0 (no entity has it) is alive, then re-arm event 2."));
	// The action moved to event 2 (added there with its fields, removed here: one step), then back.
	const std::string before = f.document->serialize().text;
	TEST_EXPECT(logic_move_edits(m, f.action, f.second.row, SIZE_MAX, edits, refusal));
	TEST_EXPECT(edits.back().operation == EditOperation::Remove);
	TEST_EXPECT(f.apply(edits));
	TEST_EXPECT(f.count(f.first, MissionKind::Action) == 0 && f.count(f.second, MissionKind::Action) == 2);
	TEST_EXPECT(same(m.record_title(f.second), "When event 1 has fired, then, after 5.1 s, kill group 2; re-arm event 2."));
	f.document->undo();
	f.refresh();
	TEST_EXPECT(f.document->serialize().text == before);
	// Within its event: a Move.
	TEST_EXPECT(logic_add_edits(m, f.first.row, *logic_type(true, int32_t(bms::ActionType::BlueWin), 0), SIZE_MAX, edits, refusal) &&
	            f.apply(edits));
	TEST_EXPECT(logic_move_edits(m, f.action, f.first.row, 1, edits, refusal) && edits.size() == 1 &&
	            edits[0].operation == EditOperation::Move && f.apply(edits));
	TEST_EXPECT(same(m.record_title(f.first).substr(m.record_title(f.first).find(", then")),
	                 ", then end the round: the blue team wins; re-arm event 2."));
	return 0;
}

// What names a record: the walker by the event's trigger, zone 20 likewise, event 2 by the action,
// group 2 by event 2's kill; the second holder of an SSN named by none.
int test_uses() {
	Fixture f;
	TEST_EXPECT(f.load());
	const MissionDocument &m = as_mission(*f.document);
	const DocumentMissionNames names(m);
	const std::string walker = "Organic #" + std::to_string(f.ssn);
	MissionUses uses = mission_uses(m, f.walker, names);
	TEST_EXPECT(same(uses.what, walker) && uses.inert.empty() && uses.uses.size() == 1);
	TEST_EXPECT(uses.uses[0].record == f.trigger && uses.uses[0].field == "param1" && uses.uses[0].event_index == 0 &&
	            same(uses.uses[0].words, walker + " is in Zone 20") && uses.uses[0].sentence.find("When ") == 0);
	uses = mission_uses(m, f.zone20, names);
	TEST_EXPECT(same(uses.what, "Zone 20") && uses.uses.size() == 1 && uses.uses[0].field == "param2");
	TEST_EXPECT(mission_uses(m, f.zone30, names).uses.empty());
	uses = mission_uses(m, f.second, names);
	TEST_EXPECT(same(uses.what, "event 2") && uses.uses.size() == 1 && uses.uses[0].record == f.action);
	uses = mission_uses(m, f.first, names);
	TEST_EXPECT(uses.uses.size() == 1 && uses.uses[0].event_index == 1); // event 2's trigger
	// Group 2, a record of the mission row's Groups.
	const MissionRow *mission_row = m.mission_row();
	NodeAddress group;
	for (const Document::Collection &list : m.collections_of({mission_row->id, mission_row->kind, 0}))
		if (list.spec.kind == k(MissionKind::Group)) group = {mission_row->id, list.spec.kind, list.ids[2]};
	uses = mission_uses(m, group, names);
	TEST_EXPECT(same(uses.what, "group 2") && uses.uses.size() == 1 && same(uses.uses[0].words, "kill group 2"));
	// A building is no event's business by itself only through its SSN: none names it.
	const Node *building = m.rows_of(MissionKind::Building)[0];
	TEST_EXPECT(mission_uses(m, {building->id, building->kind, 0}, names).uses.empty());
	// A second holder of the walker's SSN: inert, said why.
	Edit edit;
	edit.address = {building->id, building->kind, 0};
	edit.field = "id";
	edit.value = int64_t(f.ssn);
	TEST_EXPECT(f.apply({edit}));
	uses = mission_uses(m, {building->id, building->kind, 0}, DocumentMissionNames(m));
	TEST_EXPECT(!uses.inert.empty() && uses.inert.find("find the first in pool order") != std::string::npos);
	return 0;
}

// The two queries over a session: the event's form, the types, an add planned and applied through
// edit_record, a refusal answered, a record's uses.
int test_wire() {
	editor_test::TempProjectDir dir("opennova_mission_logic_wire");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Logic"));
	editor_test::create_missing_files(session);
	TEST_EXPECT(editor_test::write_bytes(session.view().project.root + "/missions/logic.bms", fixture_bytes()));
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("missions/logic.bms"));
	const Document *document = session.document_for("missions/logic.bms");
	TEST_EXPECT(document);
	if (!document) return 1;
	const MissionDocument &m = as_mission(*document);
	const Node *event = m.rows_of(MissionKind::Event)[1];
	const auto ask = [&](const char *name, const std::string &args) {
		JsonValue parsed;
		std::string error;
		opennova::io::json_parse(args, parsed, error);
		JsonValue answer = session.query(name, parsed, error);
		if (!error.empty()) std::printf("  %s refused: %s\n", name, error.c_str());
		return answer;
	};
	const std::string id = std::to_string(event->id);
	JsonValue form = ask("mission_logic", "{\"path\": \"missions/logic.bms\", \"op\": \"form\", \"id\": " + id + "}");
	TEST_EXPECT(same(form.get_string("sentence", ""), "When event 1 has fired, then, after 5.1 s, kill group 2."));
	TEST_EXPECT(form.get_number("delay_steps", -1) == 5 && form.get_number("most", 0) == 20);
	JsonValue types = ask("mission_logic", "{\"op\": \"types\", \"list\": \"action\", \"limit\": 200}");
	TEST_EXPECT(types.get("types") && types.get_number("count", 0) == double(logic_types(true).size()));
	JsonValue planned = ask("mission_logic", "{\"path\": \"missions/logic.bms\", \"op\": \"add\", \"id\": " + id +
	                                                 ", \"list\": \"action\", \"type\": 6}");
	const JsonValue *edits = planned.get("edits");
	TEST_EXPECT(edits && edits->array.size() == 1 && !planned.get("refusal"));
	// The planned edits passed back as they are.
	JsonValue request = JsonValue::make_object();
	request.set("kind", opennova::io::json_string("edit_record"));
	request.set("path", opennova::io::json_string("missions/logic.bms"));
	request.set("edits", *edits);
	const JsonValue answer = session.handle_json(request);
	session.run_operations();
	TEST_EXPECT(answer.get_bool("ok", false));
	TEST_EXPECT(same(m.record_title({event->id, event->kind, 0}), "When event 1 has fired, then, after 5.1 s, kill group 2; show text 0."));
	const JsonValue unknown = ask("mission_logic", "{\"path\": \"missions/logic.bms\", \"op\": \"add\", \"id\": " + id +
	                                                   ", \"list\": \"action\", \"type\": 29}");
	TEST_EXPECT(unknown.is_null());
	const Node *organic = m.rows_of(MissionKind::Organic)[0];
	JsonValue uses = ask("mission_uses", "{\"path\": \"missions/logic.bms\", \"id\": " + std::to_string(organic->id) + "}");
	TEST_EXPECT(uses.get_number("count", 0) == 1 && uses.get("uses") && uses.get("uses")->array[0].get_string("field", "") == "param1");
	return 0;
}

// The kinds whose value is a quantity or a choice (a count, hit points, a distance, seconds, a speed,
// on or off, a team, a sub-goal): a new record's default is the one the shipped records hold most
// often. An index (a waypoint, a variable, a dialog, a marker number, a HUD item, a light channel, an
// input bit) and a record named (picked) start at 0.
bool measured(mission::ParamKind kind) {
	using K = mission::ParamKind;
	return kind == K::Count || kind == K::Hp || kind == K::DistanceM || kind == K::Seconds || kind == K::SpeedKph ||
	       kind == K::Bool || kind == K::Team || kind == K::SubGoal;
}

// Every shipped trigger and action given a form, retyped to its own type planning nothing; and the
// value the records of each measured kind hold most often: what a new record's parameter of the kind
// takes (logic_param_default), pinned here.
int test_retail() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every shipped trigger and action given a form)");
	std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
	expansions.insert(expansions.begin(), std::string());
	std::set<std::string> seen;
	size_t missions = 0, records = 0, uses = 0;
	std::map<mission::ParamKind, std::map<int64_t, size_t>> values;
	double took_ms = 0;
	for (const std::string &expansion : expansions) {
		opennova::Vfs game;
		game.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
		TEST_EXPECT(game.mount_game(root, expansion, opennova::VfsMountMode::Packed));
		for (const opennova::VfsFileLocation &file : game.list_files()) {
			if (retail::lower_ascii(std::filesystem::path(file.logical_name).extension().string()) != ".bms") continue;
			if (!seen.insert(file.source_path + "|" + retail::lower_ascii(file.logical_name)).second) continue;
			std::vector<uint8_t> bytes;
			TEST_EXPECT(game.read_file(file.logical_name, bytes));
			std::unique_ptr<Document> document = open(bytes, file.logical_name.c_str());
			TEST_EXPECT(document && !document->blocked());
			if (!document) continue;
			++missions;
			const MissionDocument &m = as_mission(*document);
			const DocumentMissionNames names(m);
			const auto started = std::chrono::steady_clock::now();
			for (const Node *row : m.rows_of(MissionKind::Event)) {
				for (const Document::Collection &list : m.collections_of({row->id, row->kind, 0}))
					for (const NodeId id : list.ids) {
						const NodeAddress record{row->id, list.spec.kind, id};
						LogicForm form;
						TEST_EXPECT(logic_form(m, record, names, form) && form.type);
						if (!form.type) continue;
						++records;
						std::vector<Edit> edits;
						std::string refusal;
						TEST_EXPECT(logic_retype_edits(m, record, *form.type, edits, refusal) && edits.empty());
						for (const LogicParam &param : form.params)
							if (measured(param.kind)) ++values[param.kind][param.value];
					}
			}
			for (const Node *row : m.rows_of(MissionKind::Organic)) uses += mission_uses(m, {row->id, row->kind, 0}, names).uses.size();
			took_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		}
	}
	if (missions == 0) return retail::skip_leg("OPENNOVA_JO_DIR with the game's missions in its archives");
	std::printf("retail: %zu missions, %zu triggers and actions given a form and retyped to themselves, %zu uses of the "
	            "organics, in %.1f ms\n",
	            missions, records, uses, took_ms);
	// The defaults: each measured kind's most common value.
	size_t differing = 0;
	for (const auto &[kind, counts] : values) {
		int64_t most = 0;
		size_t held = 0, all = 0;
		for (const auto &[value, count] : counts) {
			all += count;
			if (count > held) {
				held = count;
				most = value;
			}
		}
		std::printf("  kind %d: %zu records, most often %lld (%zu); the default %lld\n", int(kind), all, (long long)most, held,
		            (long long)logic_param_default(kind));
		differing += logic_param_default(kind) != most;
	}
	TEST_EXPECT(differing == 0);
	TEST_EXPECT(missions == 115 && records == 3858 + 4971);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (test_forms() != 0) return 1;
	if (test_add_and_limit() != 0) return 1;
	if (test_retype_move_negate() != 0) return 1;
	if (test_uses() != 0) return 1;
	if (test_wire() != 0) return 1;
	return test_retail();
}
