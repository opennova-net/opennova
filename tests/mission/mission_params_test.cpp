// What each of a trigger's and an action's parameters is, as rows (formats/mission/mission_params.h):
// every trigger sub-type and every action type the format names has a row; a type and sub-type has one
// row, never two; the sub-type lists hold each value once and are the names the typed views give; the
// references the rows make are the witnessed ones (a zone at the three kinds of site, an event at the
// two, a waypoint list's command making a Redirect's third parameter an entity); the value domains
// name what they take.
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission_chains.h>
#include <formats/mission/mission_field.h>
#include <formats/mission/mission_params.h>

#include "common/test_expect.h"

namespace bms = opennova::bms;
using namespace opennova::mission;

namespace {

using K = ParamKind;

bool kinds(const ParamRow *row, K p1, K p2, K p3, K p4) {
	return row && row->p[0] == p1 && row->p[1] == p2 && row->p[2] == p3 && row->p[3] == p4;
}

int test_triggers() {
	const MissionChoices mains = trigger_main_types();
	TEST_EXPECT(mains.count == 7);
	size_t named = 0;
	for (size_t m = 0; m < mains.count; ++m) {
		const int32_t main = int32_t(mains.rows[m].value);
		const MissionChoices subs = trigger_sub_types(main);
		TEST_EXPECT(subs.count > 0);
		std::set<int64_t> seen;
		for (size_t s = 0; s < subs.count; ++s) {
			TEST_EXPECT(seen.insert(subs.rows[s].value).second);
			TEST_EXPECT(trigger_params(main, int32_t(subs.rows[s].value)) != nullptr);
			++named;
		}
		// A sub-type the main type's jump table lacks reads no parameter.
		const ParamRow *other = trigger_params(main, 9999);
		TEST_EXPECT(other != nullptr && other->sub == kAnySubType);
	}
	// 17 group, 21 single, 5 mission variable, 3 teammate, 23 player, and the one of the two types
	// whose sub-type selects nothing.
	TEST_EXPECT(named == 17 + 21 + 5 + 3 + 23 + 2);
	TEST_EXPECT(trigger_params(0, 0) == nullptr && trigger_params(8, 1) == nullptr);
	TEST_EXPECT(trigger_sub_types(0).count == 0);

	const int32_t group = int32_t(bms::TriggerMainType::Group), single = int32_t(bms::TriggerMainType::Single);
	const int32_t player = int32_t(bms::TriggerMainType::Player);
	TEST_EXPECT(kinds(trigger_params(group, 10), K::Group, K::Zone, K::Unused, K::Unused));
	TEST_EXPECT(kinds(trigger_params(single, 10), K::Entity, K::Zone, K::Unused, K::Unused));
	TEST_EXPECT(kinds(trigger_params(player, 37), K::Zone, K::Unused, K::Unused, K::Unused));
	TEST_EXPECT(kinds(trigger_params(int32_t(bms::TriggerMainType::Event), 0), K::Event, K::Unused, K::Unused, K::Unused));
	TEST_EXPECT(kinds(trigger_params(group, 7), K::Group, K::Path, K::PathNode, K::Unused));
	TEST_EXPECT(kinds(trigger_params(single, 43), K::Entity, K::Entity, K::DistanceM, K::Unused));
	TEST_EXPECT(kinds(trigger_params(group, 16), K::Group, K::Entity, K::Unused, K::Unused));
	TEST_EXPECT(kinds(trigger_params(int32_t(bms::TriggerMainType::MissionVariable), 3), K::MissionVar, K::Raw, K::Unused,
	                  K::Unused));
	TEST_EXPECT(kinds(trigger_params(player, 19), K::Unused, K::Unused, K::Unused, K::Unused));
	TEST_EXPECT(kinds(trigger_params(player, 31), K::Unused, K::Unused, K::Unused, K::Unused)); // absent from the table

	bms::Trigger trigger = {};
	trigger.main_type = bms::TriggerMainType::Single;
	trigger.sub_type = 10;
	TEST_EXPECT(trigger_param_kind(trigger, 0) == K::Entity && trigger_param_kind(trigger, 1) == K::Zone);
	TEST_EXPECT(std::string(trigger_param_label(trigger, 1)) == "Zone" && std::string(trigger_param_label(trigger, 3)).empty());
	TEST_EXPECT(trigger_param_kind(trigger, 4) == K::Unused && trigger_param_kind(trigger, -1) == K::Unused);
	trigger.main_type = static_cast<bms::TriggerMainType>(9);
	TEST_EXPECT(trigger_param_kind(trigger, 0) == K::Raw && std::string(trigger_param_label(trigger, 0)) == "Value");
	return 0;
}

int test_actions() {
	const MissionChoices types = action_types();
	TEST_EXPECT(types.count == 49); // 0..49 but 29, which the dispatcher has no case for
	std::set<int64_t> seen;
	for (size_t t = 0; t < types.count; ++t) {
		const int32_t type = int32_t(types.rows[t].value);
		TEST_EXPECT(seen.insert(type).second);
		TEST_EXPECT(action_params(type, 0) != nullptr);
		const MissionChoices subs = action_sub_types(type);
		TEST_EXPECT(subs.count > 0);
		std::set<int64_t> sub_seen;
		for (size_t s = 0; s < subs.count; ++s) {
			TEST_EXPECT(sub_seen.insert(subs.rows[s].value).second);
			TEST_EXPECT(action_params(type, int32_t(subs.rows[s].value)) != nullptr);
		}
	}
	TEST_EXPECT(action_params(29, 0) == nullptr && action_params(50, 0) == nullptr);

	const auto type = [](bms::ActionType value) { return int32_t(value); };
	using A = bms::ActionType;
	TEST_EXPECT(kinds(action_params(type(A::ResetEvent), 0), K::Event, K::Unused, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::AreaAiRed), 0), K::Zone, K::Raw, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::AreaAiBlue), 42), K::Zone, K::DistanceM, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::ChangeGroupAI), 42), K::Group, K::DistanceM, K::DistanceM, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::ChangeSingleAI), 44), K::Entity, K::Entity, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::ChangeSingleAI), 5), K::Entity, K::Unused, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::ChangeGroupAI), 7), K::Group, K::Raw, K::Raw, K::Raw)); // an arm with no token
	TEST_EXPECT(kinds(action_params(type(A::RedirectSingleTo), 0), K::Entity, K::Path, K::PathNode, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::PlayWavList), 0), K::Dialog, K::Bool, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::ChangeSteamAction), 0), K::Entity, K::Team, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::ShowWinSubgoal), 0), K::SubGoal, K::Bool, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::SpecialSubType), 37), K::HudTimer, K::Raw, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::SpecialSubType), 38), K::Unused, K::Unused, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::MisvarChange), 1), K::MissionVar, K::Raw, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::MisvarChange), 4), K::MissionVar, K::Unused, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::GroupTargetSsnPri), 0), K::Group, K::Entity, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::SsnTargetGroupExc), 0), K::Entity, K::Group, K::Unused, K::Unused));
	// The teammate operations: the patient's SSN and the marker's number for the medevac and the
	// flyover, nothing read under any other sub-type; ExecuteWac, which the dispatcher has no arm for,
	// reads nothing.
	TEST_EXPECT(kinds(action_params(type(A::Teammates), 1), K::Entity, K::WpNumber, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::Teammates), 2), K::Entity, K::WpNumber, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::Teammates), 3), K::Unused, K::Unused, K::Unused, K::Unused));
	TEST_EXPECT(kinds(action_params(type(A::ExecuteWac), 0), K::Unused, K::Unused, K::Unused, K::Unused));
	{
		bms::Action medevac = {};
		medevac.action_type = A::Teammates;
		medevac.action_sub_type = 1;
		TEST_EXPECT(std::string(action_param_label(medevac, 0)) == "Patient" &&
		            std::string(action_param_label(medevac, 1)) == "Marker number");
	}

	// A Redirect's third parameter: a stop's number, or the entity to go to where the waypoint list is
	// a command 123..125.
	bms::Action action = {};
	action.action_type = A::RedirectGroupTo;
	action.param2 = 7;
	TEST_EXPECT(action_param_kind(action, 2) == K::PathNode && std::string(action_param_label(action, 2)) == "Waypoint number");
	for (const int32_t command : {123, 124, 125}) {
		action.param2 = command;
		TEST_EXPECT(action_param_kind(action, 2) == K::Entity && std::string(action_param_label(action, 2)) == "Entity");
	}
	action.param2 = 126;
	TEST_EXPECT(action_param_kind(action, 2) == K::PathNode);
	TEST_EXPECT(path_command_names_entity(123) && path_command_names_entity(125) && !path_command_names_entity(126) &&
	            !path_command_names_entity(122));
	action.action_type = static_cast<A>(29);
	TEST_EXPECT(action_param_kind(action, 0) == K::Raw);
	action.action_type = A::ChangeGroupAI;
	action.action_sub_type = 42;
	TEST_EXPECT(std::string(action_param_label(action, 1)) == "Minimum distance" &&
	            std::string(action_param_label(action, 2)) == "Maximum distance" &&
	            std::string(action_param_label(action, 0)) == "Group");
	return 0;
}

int test_names_and_domains() {
	// The typed views name a sub-type from the rows.
	bms::File file;
	make_default(file);
	MissionEventRecord event;
	add_event(file, event);
	std::string error;
	MissionTriggerRecord trigger;
	trigger.main_type = int(bms::TriggerMainType::Player);
	trigger.sub_type = 37;
	TEST_EXPECT(insert_event_trigger(file, 0, 0, trigger, error));
	MissionActionRecord action;
	action.action_type = int(bms::ActionType::AreaAiRed);
	action.action_sub_type = 44;
	TEST_EXPECT(insert_event_action(file, 0, 0, action, error));
	MissionTriggerRecord read_trigger;
	MissionActionRecord read_action;
	TEST_EXPECT(opennova::mission::trigger(file, 0, read_trigger) && opennova::mission::action(file, 0, read_action));
	TEST_EXPECT(read_trigger.main_type_name == "Player" && read_trigger.sub_type_name == "PlayerSatchel");
	TEST_EXPECT(read_action.action_type_name == "AreaAiRed" && read_action.action_sub_type_name == "TargetSsn");
	const MissionChoices player = trigger_sub_types(int32_t(bms::TriggerMainType::Player));
	TEST_EXPECT(std::string(mission_choice_name(player.rows, player.count, 37)) == read_trigger.sub_type_name);
	const MissionChoices ai = action_sub_types(int32_t(bms::ActionType::ChangeGroupAI));
	TEST_EXPECT(ai.count == 27 && std::string(mission_choice_name(ai.rows, ai.count, 44)) == "TargetSsn");
	TEST_EXPECT(action_sub_types(int32_t(bms::ActionType::KillGroup)).count == 1);

	TEST_EXPECT(param_choices(K::Team).count == 3 && param_choices(K::Bool).count == 2 && param_choices(K::SubGoal).count == 8);
	TEST_EXPECT(param_choices(K::Group).count == 0 && param_choices(K::Raw).count == 0);
	TEST_EXPECT(std::string(param_kind_label(K::Unused)).empty() && std::string(param_kind_label(K::Path)) == "Waypoint list");
	return 0;
}

// The record values that name nothing and the rows the lookups key: a waypoint list's 0 and its
// commands 123..127 (by the original editor's names), group 0, the player's SSN; the relation records'
// SSN rows (0..127) a trigger's SSN slot keys; the pools each narrowed SSN lookup scans.
int test_named_values() {
	TEST_EXPECT(path_names_none(0) && path_names_none(123) && path_names_none(127) && !path_names_none(1) &&
	            !path_names_none(122) && !path_names_none(128) && !path_names_none(-1));
	TEST_EXPECT(std::string(path_command_editor_name(123)) == "Goto SSN (not driver, gunner)" &&
	            std::string(path_command_editor_name(124)) == "Goto SSN (not driver)" &&
	            std::string(path_command_editor_name(125)) == "Goto SSN (any)" &&
	            std::string(path_command_editor_name(126)) == "Goto group" &&
	            std::string(path_command_editor_name(127)) == "Goto player");
	TEST_EXPECT(!path_command_editor_name(0) && !path_command_editor_name(122) && !path_command_editor_name(128));
	TEST_EXPECT(group_names_none(0) && !group_names_none(1) && !group_names_none(63));
	TEST_EXPECT(kPlayerSsn == 10000 && kRelationSsnRows == 128);

	const auto trigger_of = [](bms::TriggerMainType main, int32_t sub, int32_t p1 = 0, int32_t p2 = 0) {
		bms::Trigger out{};
		out.main_type = main;
		out.sub_type = sub;
		out.param1 = p1;
		out.param2 = p2;
		return out;
	};
	using M = bms::TriggerMainType;
	// A group's sees-single keys its entity (param2); a destroyed test keys no relation record.
	TEST_EXPECT(trigger_ssn_unrecorded(trigger_of(M::Group, 16, 4, 200), 1) &&
	            !trigger_ssn_unrecorded(trigger_of(M::Group, 16, 4, 20), 1) &&
	            !trigger_ssn_unrecorded(trigger_of(M::Single, 4, 10034), 0));
	// A single's relation subs key both slots, its sees/targeted/shot-group and visited subs param1 alone.
	TEST_EXPECT(trigger_ssn_unrecorded(trigger_of(M::Single, 16, kPlayerSsn, 12), 0) &&
	            trigger_ssn_unrecorded(trigger_of(M::Single, 15, 12, 128), 1) &&
	            !trigger_ssn_unrecorded(trigger_of(M::Single, 17, 127, 0), 0) &&
	            trigger_ssn_unrecorded(trigger_of(M::Single, 7, -1, 5), 0) &&
	            !trigger_ssn_unrecorded(trigger_of(M::Single, 7, 12, 500), 1) &&
	            !trigger_ssn_unrecorded(trigger_of(M::Group, 16, 400, 20), 0));

	TEST_EXPECT(kAllPools == (kOrganicPool | kItemPool | kBuildingPool | kMarkerPool) && kAllPools == 15);
	TEST_EXPECT(trigger_ssn_pools(trigger_of(M::Single, 4)) == (kOrganicPool | kItemPool | kBuildingPool) &&
	            trigger_ssn_pools(trigger_of(M::Single, 5)) == (kOrganicPool | kItemPool | kBuildingPool));
	for (const int32_t sub : {3, 14, 6, 9, 12, 10}) TEST_EXPECT(trigger_ssn_pools(trigger_of(M::Single, sub)) == (kOrganicPool | kItemPool));
	TEST_EXPECT(trigger_ssn_pools(trigger_of(M::Single, 11)) == kOrganicPool);
	TEST_EXPECT(trigger_ssn_pools(trigger_of(M::Single, 16)) == 0 && trigger_ssn_pools(trigger_of(M::Group, 4)) == 0);
	bms::Action action{};
	using A = bms::ActionType;
	for (const A type : {A::ChangeSingleAI, A::ChangeSteamAction, A::SingleChangeGroup, A::SingleTeleportAction}) {
		action.action_type = type;
		TEST_EXPECT(action_ssn_pools(action) == (kOrganicPool | kItemPool | kBuildingPool));
	}
	action.action_type = A::Teammates;
	action.action_sub_type = 1;
	TEST_EXPECT(action_ssn_pools(action) == kOrganicPool);
	action.action_sub_type = 3;
	TEST_EXPECT(action_ssn_pools(action) == 0);
	action.action_type = A::KillSingle;
	action.action_sub_type = 0;
	TEST_EXPECT(action_ssn_pools(action) == 0);
	return 0;
}

// The SSN a new entity takes skips the players' net ids; the pools load and scan organics, items,
// buildings, markers; a zone id resolves to the first area of the id unless its box is flat.
int test_ssns_pools_and_zones() {
	// Past every player's net id, 10000 plus the slot byte, by its low 16 bits [orig: PlayerClass_InitEntity
	// @0x4b1149..0x4b1173; EntityPool_FindByNetId @0x4f0a20].
	TEST_EXPECT(ssn_after(0) == 1 && ssn_after(41) == 42);
	TEST_EXPECT(ssn_after(kPlayerSsn - 1) == kPlayerSsn + kPlayerSsnSlots && ssn_after(kPlayerSsn + 7) == kPlayerSsn + kPlayerSsnSlots);
	TEST_EXPECT(ssn_after(kPlayerSsn + kPlayerSsnSlots) == kPlayerSsn + kPlayerSsnSlots + 1);
	TEST_EXPECT(ssn_after(0x10000 + kPlayerSsn - 1) == 0x10000 + kPlayerSsn + kPlayerSsnSlots);
	TEST_EXPECT(ssn_is_players(kPlayerSsn + 255) && !ssn_is_players(kPlayerSsn + 256) && ssn_is_players(0x20000 + kPlayerSsn));

	using opennova::mission::EntityKind;
	TEST_EXPECT(entity_pool(EntityKind::Organic) == 0 && entity_pool(EntityKind::Item) == 1 &&
	            entity_pool(EntityKind::Building) == 2 && entity_pool(EntityKind::Marker) == 3);
	TEST_EXPECT(kOrganicPool == 1u << entity_pool(EntityKind::Organic) && kItemPool == 1u << entity_pool(EntityKind::Item) &&
	            kBuildingPool == 1u << entity_pool(EntityKind::Building) &&
	            kMarkerPool == 1u << entity_pool(EntityKind::Marker));

	const auto area = [](int32_t id, int32_t x_min, int32_t x_max, int32_t y_min, int32_t y_max) {
		bms::AreaTrigger a{};
		a.id = id;
		a.x_min = x_min;
		a.x_max = x_max;
		a.y_min = y_min;
		a.y_max = y_max;
		return a;
	};
	// Zone 6 twice (the first is the one found), zone 7 flat on x, zone 8 flat on y; the words
	// compare raw, so two corners one 16.16 step apart are no flat box.
	const std::vector<bms::AreaTrigger> areas = {
			area(6, 0, 65536, 0, 65536), area(6, 0, 0, 0, 65536), area(7, 1 << 24, 1 << 24, 0, 65536),
			area(8, 0, 65536, -65536, -65536), area(9, 300 << 16, (300 << 16) + 1, 0, 1),
	};
	TEST_EXPECT(zone_area_index(areas, 6) == 0 && zone_area_index(areas, 9) == 4 && zone_area_index(areas, 5) == -1);
	TEST_EXPECT(!zone_box_flat(areas[0]) && zone_box_flat(areas[1]) && zone_box_flat(areas[2]) &&
	            zone_box_flat(areas[3]) && !zone_box_flat(areas[4]));
	TEST_EXPECT(zone_resolves(areas, 6) && !zone_resolves(areas, 7) && !zone_resolves(areas, 8) &&
	            zone_resolves(areas, 9) && !zone_resolves(areas, 5));
	return 0;
}

} // namespace

int main() {
	if (test_triggers() != 0) return 1;
	if (test_actions() != 0) return 1;
	if (test_named_values() != 0) return 1;
	if (test_ssns_pools_and_zones() != 0) return 1;
	return test_names_and_domains();
}
