// A mission's logic in words (editor/documents/mission_sentence.h, ADR 0046 S15, Events and scripts):
// each trigger and action worded from its type and its typed parameters, the names of what they name
// from a MissionNames, the negation flipping a trigger's verb, the chain's flat left-to-right fold of the
// joins held together where a join changes, an event's flags, delay and repeat in its sentence, a type
// no row names and a value naming nothing said so; over the minted synth_logic mission, the document's
// titles (an event its sentence, a trigger its words after its join, an action its words) and its names
// (an entity by its kind and SSN, a zone by its record, an SSN or a zone no record holds said so); the
// words on the wire (the `record` query's title) and in Problems (a row's `record_title`); the
// types "Add trigger" and "Add action" offer, each with its title, group and line, every row of
// mission_params' triggers and actions among them. With the game install (a SKIP-LEG without
// OPENNOVA_JO_DIR), every event of every shipped mission worded, none of a type no row names, timed.
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
#include <editor/documents/mission_sentence.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <formats/mission/bms.h>
#include <formats/mission/mission_chains.h>
#include <formats/mission/mission_field.h>
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

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

// Two words alike, both printed where they differ.
bool same(const std::string &got, const std::string &want) {
	if (got != want) std::printf("  got:  %s\n  want: %s\n", got.c_str(), want.c_str());
	return got == want;
}

bms::Trigger trigger(bms::TriggerMainType main, int32_t sub, int32_t p1 = 0, int32_t p2 = 0, int32_t p3 = 0,
                     int32_t flags = 0) {
	bms::Trigger out{};
	out.condition_flags = flags;
	out.main_type = main;
	out.sub_type = sub;
	out.param1 = p1;
	out.param2 = p2;
	out.param3 = p3;
	return out;
}

bms::Action action(bms::ActionType type, int32_t sub = 0, int32_t p1 = 0, int32_t p2 = 0, int32_t p3 = 0) {
	bms::Action out{};
	out.action_type = type;
	out.action_sub_type = sub;
	out.param1 = p1;
	out.param2 = p2;
	out.param3 = p3;
	return out;
}

// The project's names over a few values, and a string table of one key.
class Names : public MissionNames {
public:
	std::string entity(int64_t ssn) const override {
		return ssn == 10034 ? "Hostage #10034" : ssn == 2200 ? "Truck #2200" : MissionNames::entity(ssn);
	}
	std::string zone(int64_t id) const override { return id == 3 ? "Zone 3 (bridge)" : MissionNames::zone(id); }
	std::string text(const std::string &section, const std::string &key) const override {
		if (section == "Triggered Text" && key == "ID004") return "Mission failed";
		if (section == "WinConditions" && key == "STRWINCOND007") return "Rescue the hostage";
		if (section == "WinConditions" && key == "STRWINMSG007") return "Hostage rescued";
		return std::string();
	}
};

int test_words() {
	const Names names;
	using M = bms::TriggerMainType;
	using A = bms::ActionType;
	// A trigger by its type's words, its parameters named; negated, its verb flips.
	TEST_EXPECT(trigger_words(trigger(M::Single, 4, 10034), names) == "Hostage #10034 is destroyed");
	TEST_EXPECT(trigger_words(trigger(M::Single, 4, 10034, 0, 0, bms::Trigger::kConditionNegated), names) ==
	            "Hostage #10034 is not destroyed");
	TEST_EXPECT(trigger_words(trigger(M::Single, 10, 10000, 3), names) == "the player is in Zone 3 (bridge)");
	TEST_EXPECT(trigger_words(trigger(M::Group, 6, 4, 3), names) == "group 4 has lost 3 units or more");
	TEST_EXPECT(trigger_words(trigger(M::Group, 7, 4, 5, -1), names) == "group 4 has passed the nearest waypoint of path 5");
	TEST_EXPECT(trigger_words(trigger(M::Single, 43, 10034, 2200, 25), names) == "Hostage #10034 is within 25 m of Truck #2200");
	TEST_EXPECT(trigger_words(trigger(M::Event, 9, 2), names) == "event 3 has fired"); // the sub-type unread
	TEST_EXPECT(trigger_words(trigger(M::MissionVariable, 3, 1, 5), names) == "variable 1 is greater than 5");
	TEST_EXPECT(trigger_words(trigger(M::Player, 36, 30), names) == "the player has been outside the mission area for 30 s");
	// Never a guess: a type no row names says so.
	TEST_EXPECT(trigger_words(trigger(M::Single, 8, 10034), names) == "a trigger of unknown kind 8 (false)");
	TEST_EXPECT(trigger_words(trigger(static_cast<M>(0), 0), names) == "a trigger of no type (false)");
	TEST_EXPECT(trigger_words(trigger(static_cast<M>(12), 1), names) == "a trigger of unknown type 12 (false)");

	// An action by its type's words; a text key's string quoted where the names know it.
	TEST_EXPECT(action_words(action(A::OutputText, 0, 4), names, nullptr) == "show text 4: 'Mission failed'");
	TEST_EXPECT(action_words(action(A::OutputText, 0, 5), names, nullptr) == "show text 5");
	TEST_EXPECT(action_words(action(A::KillSingle, 0, 10034), names, nullptr) == "kill Hostage #10034");
	TEST_EXPECT(action_words(action(A::ChangeGTeamAction, 0, 4, 2), names, nullptr) == "move group 4 to the Evil team (team 2)");
	TEST_EXPECT(action_words(action(A::MisvarChange, 2, 1, 7), names, nullptr) == "add 7 to variable 1");
	TEST_EXPECT(action_words(action(A::MisvarChange, 4, 1, 7), names, nullptr) == "add 1 to variable 1");
	// A Redirect: a path from a stop, or a command naming an entity [world-wac-ai-re.md 11].
	TEST_EXPECT(action_words(action(A::RedirectGroupTo, 0, 4, 5, 2), names, nullptr) ==
	            "send group 4 along path 5, from waypoint 2");
	TEST_EXPECT(action_words(action(A::RedirectSingleTo, 0, 10034, 125, 2200), names, nullptr) ==
	            "send Hostage #10034 to board Truck #2200 (any seat)");
	TEST_EXPECT(action_words(action(A::RedirectSingleTo, 0, 10034, 0, 0), names, nullptr) == "stop Hostage #10034 (no path)");
	// An AI change by its command; sub-type 0 does nothing; an area's soldiers by their team.
	TEST_EXPECT(action_words(action(A::ChangeGroupAI, 5, 4), names, nullptr) == "put group 4 on red alert");
	TEST_EXPECT(action_words(action(A::ChangeSingleAI, 8, 10034, 80), names, nullptr) == "set Hostage #10034's accuracy to 80");
	TEST_EXPECT(action_words(action(A::ChangeGroupAI, 43, 6, 1), names, nullptr) == "turn group 6's indestructibility on");
	TEST_EXPECT(action_words(action(A::ChangeGroupAI, 0, 4), names, nullptr) == "do nothing to group 4 (no AI change)");
	TEST_EXPECT(action_words(action(A::AreaAiRed, 5, 3), names, nullptr) ==
	            "put the red team's soldiers in Zone 3 (bridge) on red alert");
	TEST_EXPECT(action_words(action(A::ChangeSingleAI, 99, 10034), names, nullptr) ==
	            "apply AI command 99 to Hostage #10034 (what it does is unknown)");
	// A sub-goal by its slot, with the objective's line and its message where the header gives the id.
	bms::Header header{};
	header.win_conditions[1] = 7;
	TEST_EXPECT(action_words(action(A::SubGoalWon, 0, 2), names, &header) ==
	            "win sub-goal 2 ('Rescue the hostage'), saying 'Hostage rescued'");
	TEST_EXPECT(action_words(action(A::SubGoalWon, 0, 2), names, nullptr) == "win sub-goal 2");
	TEST_EXPECT(action_words(action(A::ShowWinSubgoal, 0, 2, 0), names, &header) == "hide win objective 2");
	TEST_EXPECT(action_words(action(static_cast<A>(29)), names, nullptr) == "an action of unknown type 29 (does nothing)");
	TEST_EXPECT(action_words(action(A::ExecuteWac), names, nullptr) == "do nothing (Execute WAC has no effect)");
	return 0;
}

int test_sentences() {
	const Names names;
	using M = bms::TriggerMainType;
	using A = bms::ActionType;
	mission::EventChain chain{};
	chain.triggers = {trigger(M::Single, 4, 10034)};
	chain.actions = {action(A::OutputText, 0, 4), action(A::SubGoalLost, 0, 2)};
	TEST_EXPECT(event_sentence(chain, names, nullptr) == "When Hostage #10034 is destroyed, then show text 4: 'Mission failed'; lose sub-goal 2.");
	// The joins fold left to right, each the previous trigger's [orig: EventTrigger_EvaluateChain
	// @0x454050]: a join that changes holds what came before it together.
	chain.triggers = {trigger(M::Group, 10, 3, 3), trigger(M::Player, 39, 2200, 0, 0, bms::Trigger::kConditionOr),
	                  trigger(M::Group, 10, 4, 3), trigger(M::Player, 39, 10034)};
	chain.actions = {action(A::PlayWavList, 0, 30)};
	TEST_EXPECT(event_sentence(chain, names, nullptr) ==
	            "When ((group 3 has a unit in Zone 3 (bridge) and the player is on Truck #2200) or group 4 has a unit in Zone 3 "
	            "(bridge)) and the player is on Hostage #10034, then play dialog 30.");
	chain.triggers = {trigger(M::Single, 5, 10034, 0, 0, bms::Trigger::kConditionXor), trigger(M::Single, 5, 2200)};
	chain.actions.clear();
	TEST_EXPECT(event_sentence(chain, names, nullptr) ==
	            "When either Hostage #10034 is alive or else Truck #2200 is alive (not both), then nothing.");
	// No trigger: at once; the flags, the delay and the repeat in words (a unit is 64 ticks: 1.024 s).
	chain.triggers.clear();
	chain.actions = {action(A::BlueWin)};
	TEST_EXPECT(event_sentence(chain, names, nullptr) == "Right away, then end the round: the blue team wins.");
	chain.event.flags = bms::EventFlags::PreMission;
	TEST_EXPECT(event_sentence(chain, names, nullptr) == "At the mission's start, then end the round: the blue team wins.");
	chain.event.flags = bms::EventFlags::ResetAfter;
	chain.event.delay = 5;
	chain.event.reset_after = 10;
	chain.triggers = {trigger(M::Single, 5, 10034)};
	const EventWords words = event_words(chain, names, nullptr);
	TEST_EXPECT(words.when == "When Hostage #10034 is alive" && words.delay == "after 5.1 s" &&
	            words.repeat == "Checked again 10.2 s after it fires.");
	TEST_EXPECT(words.sentence ==
	            "When Hostage #10034 is alive, then, after 5.1 s, end the round: the blue team wins. Checked again 10.2 s after it "
	            "fires.");
	TEST_EXPECT(logic_units_seconds(1) == 1.024);
	return 0;
}

std::unique_ptr<Document> open(const std::vector<uint8_t> &bytes, const char *name) {
	const DocumentType *type = document_type_for(AssetKind::Mission);
	if (!type) return nullptr;
	std::unique_ptr<Document> document = records_of(type->make());
	Diagnostic error;
	if (!document || !document->load_bytes(bytes, name, AssetKind::Mission, "jo", error)) return nullptr;
	return document;
}

// The minted mission's titles: event 1 the walker inside zone 20 re-arming event 2, event 2 event 1
// having fired killing group 2 after its delay of 5 units; the walker by its kind and SSN.
int test_document_titles() {
	std::unique_ptr<Document> document =
	        open(test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_logic.bms"),
	             "synth_logic.bms");
	TEST_EXPECT(document);
	const MissionDocument &m = dynamic_cast<const MissionDocument &>(*document);
	const std::vector<const Node *> events = m.rows_of(MissionKind::Event), organics = m.rows_of(MissionKind::Organic);
	TEST_EXPECT(events.size() == 2 && organics.size() == 2);
	const int32_t walker = static_cast<const EntityRow &>(*organics[0]).native.id;
	const NodeAddress first{events[0]->id, k(MissionKind::Event), 0}, second{events[1]->id, k(MissionKind::Event), 0};
	TEST_EXPECT(m.record_title(first) ==
	            "When Organic " + std::to_string(walker) + " is in Zone 20, then re-arm event 2.");
	TEST_EXPECT(m.record_title(second) == "When event 1 has fired, then, after 5.1 s, kill group 2.");
	// The record's name is still its kind and place (the graph's, Problems', the wire's).
	TEST_EXPECT(m.record_name(first) == "Event 1");
	// A trigger and an action by their words.
	const std::vector<Document::Collection> lists = m.collections_of(first);
	TEST_EXPECT(lists.size() == 2 && lists[0].ids.size() == 1 && lists[1].ids.size() == 1);
	TEST_EXPECT(m.record_title({first.row, k(MissionKind::Trigger), lists[0].ids[0]}) ==
	            "Organic " + std::to_string(walker) + " is in Zone 20");
	TEST_EXPECT(m.record_title({first.row, k(MissionKind::Action), lists[1].ids[0]}) == "re-arm event 2");
	// The document's names: what no record holds said so.
	const DocumentMissionNames names(m);
	TEST_EXPECT(same(names.entity(99999), "SSN 99999 (no entity has it)"));
	TEST_EXPECT(same(names.zone(7), "zone 7 (no area has it)"));
	TEST_EXPECT(same(names.event(5), "event 6 (the mission has no such event)"));
	TEST_EXPECT(names.entity(10000) == "the player");
	// An edit moves the words: zone 30 named instead.
	Edit edit;
	edit.operation = EditOperation::Set;
	edit.address = {first.row, k(MissionKind::Trigger), lists[0].ids[0]};
	edit.field = "param2";
	edit.value = int64_t(30);
	Diagnostic error;
	TEST_EXPECT(document->apply({edit}, error));
	TEST_EXPECT(m.record_title(first) == "When Organic " + std::to_string(walker) + " is in Zone 30, then re-arm event 2.");
	return 0;
}

// The words on the wire and in Problems: over a project holding the minted mission with its first
// event's trigger naming an SSN no entity has, the `record` query's title of the event (its sentence)
// and of the trigger (its words) beside their names, and the `problems` query's row of the trigger's
// missing SSN naming the trigger in words (`record_title`) while the mission is open.
int test_wire_and_problems() {
	editor_test::TempProjectDir dir("opennova_mission_sentence_wire");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Words"));
	editor_test::create_missing_files(session);
	const std::string root = session.view().project.root;
	{
		const std::vector<uint8_t> bytes =
		        test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/bms/synth_logic.bms");
		bms::File file;
		std::string message;
		TEST_EXPECT(bms::parse(bytes.data(), bytes.size(), file, message));
		file.triggers[0].param1 = 4321;
		std::vector<uint8_t> out;
		TEST_EXPECT(bms::write(file, out, message));
		TEST_EXPECT(editor_test::write_bytes(root + "/missions/words.bms", out));
	}
	editor_test::handle_to_end(session, request::rescan());
	editor_test::handle_to_end(session, request::open_document("missions/words.bms"));
	const Document *document = session.document_for("missions/words.bms");
	TEST_EXPECT(document);
	if (!document) return 1;
	const MissionDocument &m = dynamic_cast<const MissionDocument &>(*document);
	const Node *event = m.rows_of(MissionKind::Event)[0];
	const NodeId trigger = m.collections_of({event->id, event->kind, 0})[0].ids[0];
	const auto ask = [&](const char *name, opennova::io::JsonValue args) {
		std::string error;
		opennova::io::JsonValue answer = session.query(name, args, error);
		if (!error.empty()) std::printf("  %s refused: %s\n", name, error.c_str());
		return answer;
	};
	opennova::io::JsonValue args = opennova::io::JsonValue::make_object();
	args.set("path", opennova::io::json_string("missions/words.bms"));
	args.set("id", opennova::io::JsonValue::make_number(double(event->id)));
	opennova::io::JsonValue answer = ask("record", args);
	TEST_EXPECT(same(answer.get_string("name", ""), "Event 1"));
	TEST_EXPECT(same(answer.get_string("title", ""), "When SSN 4321 (no entity has it) is in Zone 20, then re-arm event 2."));
	args.set("id", opennova::io::JsonValue::make_number(double(trigger)));
	answer = ask("record", args);
	TEST_EXPECT(same(answer.get_string("title", ""), "SSN 4321 (no entity has it) is in Zone 20"));
	opennova::io::JsonValue problems_args = opennova::io::JsonValue::make_object();
	problems_args.set("text", opennova::io::json_string("4321"));
	const opennova::io::JsonValue problems = ask("problems", problems_args);
	const opennova::io::JsonValue *rows = problems.get("problems");
	size_t worded = 0;
	for (const opennova::io::JsonValue &row : rows ? rows->array : std::vector<opennova::io::JsonValue>()) {
		if (row.get_string("code", "") != "reference.missing" || row.get_string("field", "") != "param1") continue;
		TEST_EXPECT(row.get_string("record", "") == "Event 1/Trigger 1");
		TEST_EXPECT(same(row.get_string("record_title", ""), "SSN 4321 (no entity has it) is in Zone 20"));
		++worded;
	}
	TEST_EXPECT(worded == 1);
	return 0;
}

// The types "Add trigger" and "Add action" offer: each a title, a group and a line; every trigger
// sub-type and action type mission_params has a row for among them (the AI actions by command).
int test_types() {
	const std::vector<LogicType> &triggers = logic_types(false), &actions = logic_types(true);
	std::set<std::pair<int32_t, int32_t>> offered;
	for (const LogicType &type : triggers) {
		TEST_EXPECT(!type.action && *type.title && *type.group && *type.tip);
		TEST_EXPECT(offered.insert({type.type, type.sub}).second);
	}
	// Every sub-type the evaluator has a case for, per its main type.
	for (const mission::MissionChoice &main : [] {
		     const mission::MissionChoices all = mission::trigger_main_types();
		     return std::vector<mission::MissionChoice>(all.rows, all.rows + all.count);
	     }()) {
		const mission::MissionChoices subs = mission::trigger_sub_types(int32_t(main.value));
		for (size_t i = 0; i < subs.count; ++i) {
			const int32_t sub = int32_t(subs.rows[i].value);
			if (sub == 0 && mission::trigger_reads_sub_type(int32_t(main.value))) continue; // "Null": false
			TEST_EXPECT(logic_type(false, int32_t(main.value), sub) != nullptr);
		}
	}
	for (const LogicType &type : actions) TEST_EXPECT(type.action && *type.title && *type.group && *type.tip);
	// Every action type the dispatcher names; one whose sub-type selects what it does (an AI action's
	// command, a variable's change, a special, a teammate call) by each sub-type the dispatcher has an
	// arm for, its sub-type 0 none (it does nothing) and the teammates' 3 none (no arm).
	const mission::MissionChoices types = mission::action_types();
	for (size_t i = 0; i < types.count; ++i) {
		const int32_t type = int32_t(types.rows[i].value);
		const mission::MissionChoices subs = mission::action_sub_types(type);
		if (subs.count == 1) {
			TEST_EXPECT(logic_type(true, type, 0) != nullptr);
			continue;
		}
		for (size_t j = 0; j < subs.count; ++j) {
			const int32_t sub = int32_t(subs.rows[j].value);
			if (sub == 0) continue;
			TEST_EXPECT(logic_type(true, type, sub) != nullptr ||
			            (type == int32_t(bms::ActionType::Teammates) && sub == 3));
		}
		TEST_EXPECT(logic_type(true, type, 0) == nullptr);
	}
	const LogicType *red = logic_type(true, int32_t(bms::ActionType::ChangeGroupAI), 5);
	TEST_EXPECT(red && same(red->title, "Group AI: Red alert") && same(red->group, "AI"));
	const LogicType *set = logic_type(true, int32_t(bms::ActionType::MisvarChange), 1);
	TEST_EXPECT(set && same(set->title, "Set a variable"));
	// The groups stand together, in order.
	std::set<std::string> closed;
	std::string current;
	for (const LogicType &type : actions) {
		if (current == type.group) continue;
		TEST_EXPECT(!closed.count(type.group));
		if (!current.empty()) closed.insert(current);
		current = type.group;
	}
	return 0;
}

// Every event of every shipped mission (OPENNOVA_JO_DIR, base and each expansion through the VFS),
// worded by its document: none of a trigger or an action type no row names (the shipped missions use
// only witnessed types), the time it took.
int test_retail() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every shipped event in words)");
	std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
	expansions.insert(expansions.begin(), std::string());
	std::set<std::string> seen;
	size_t missions = 0, events = 0, triggers = 0, actions = 0, unknown = 0, longest = 0;
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
			const MissionDocument &m = dynamic_cast<const MissionDocument &>(*document);
			const auto started = std::chrono::steady_clock::now();
			for (const Node *row : m.rows_of(MissionKind::Event)) {
				++events;
				const std::string sentence = m.record_title({row->id, row->kind, 0});
				longest = std::max(longest, sentence.size());
				const auto &chain = static_cast<const MissionRecordRow<mission::EventChain> &>(*row).native;
				for (const bms::Trigger &t : chain.triggers) {
					++triggers;
					if (!logic_type(false, int32_t(t.main_type), t.sub_type)) {
						++unknown;
						std::printf("  %s: trigger %d/%d\n", file.logical_name.c_str(), int(t.main_type), t.sub_type);
					}
				}
				for (const bms::Action &a : chain.actions) {
					++actions;
					if (!logic_type(true, int32_t(a.action_type), a.action_sub_type)) {
						++unknown;
						std::printf("  %s: action %d/%d\n", file.logical_name.c_str(), int(a.action_type), a.action_sub_type);
					}
				}
				TEST_EXPECT(sentence.find("unknown type") == std::string::npos);
			}
			took_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		}
	}
	if (missions == 0) return retail::skip_leg("OPENNOVA_JO_DIR with the game's missions in its archives");
	std::printf("retail: %zu missions, %zu events (%zu triggers, %zu actions) worded in %.1f ms, the longest sentence %zu "
	            "characters, %zu of a type no row names\n",
	            missions, events, triggers, actions, took_ms, longest, unknown);
	TEST_EXPECT(missions == 115 && unknown == 0);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (test_words() != 0) return 1;
	if (test_sentences() != 0) return 1;
	if (test_document_titles() != 0) return 1;
	if (test_wire_and_problems() != 0) return 1;
	if (test_types() != 0) return 1;
	return test_retail();
}
