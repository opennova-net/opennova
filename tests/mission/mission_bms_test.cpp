#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include "mission/bms.h"
#include "mission/mission.h"

namespace {

std::string fixture_path() {
	const std::string root = test_paths_repo_root(__FILE__);
	return root + "/fixtures/bms/ash_i5b.reference.bms";
}

std::vector<uint8_t> read_file(const std::string &path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file.good()) {
		return {};
	}
	const std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);
	std::vector<uint8_t> data(static_cast<size_t>(size));
	file.read(reinterpret_cast<char *>(data.data()), size);
	if (!file.good()) {
		return {};
	}
	return data;
}

} // namespace

int main() {
	const std::vector<uint8_t> original = read_file(fixture_path());
	TEST_EXPECT(!original.empty());
	TEST_EXPECT(opennova::bms::is_bms(original.data(), original.size()));

	opennova::bms::File bms_file;
	std::string error;
	TEST_EXPECT(opennova::bms::parse(original.data(), original.size(), bms_file, error));
	TEST_EXPECT(bms_file.items.size() == bms_file.header.num_items);
	TEST_EXPECT(bms_file.buildings.size() == bms_file.header.num_buildings);
	TEST_EXPECT(bms_file.markers.size() == bms_file.header.num_markers);
	TEST_EXPECT(bms_file.organics.size() == bms_file.header.num_people);
	TEST_EXPECT(bms_file.waypoint_records.size() == opennova::bms::kWaypointRecordCount);
	TEST_EXPECT(bms_file.group_records.size() == opennova::bms::kGroupRecordCount);
	TEST_EXPECT(bms_file.layer_records.size() == opennova::bms::kLayerRecordCount);

	std::vector<uint8_t> encoded;
	TEST_EXPECT(opennova::bms::write(bms_file, encoded, error));
	TEST_EXPECT(encoded.size() == original.size());
	TEST_EXPECT(std::memcmp(encoded.data(), original.data(), original.size()) == 0);

	opennova::mission::MissionDocument document;
	TEST_EXPECT(document.load_bms_bytes(original.data(), original.size()));
	TEST_EXPECT(document.is_loaded());
	TEST_EXPECT(!document.info().mission_name.empty());

	const size_t original_item_count = document.entity_count(opennova::mission::EntityKind::Item);
	TEST_EXPECT(original_item_count > 0);

	opennova::mission::EntityRecord first_item;
	TEST_EXPECT(document.get_entity(opennova::mission::EntityKind::Item, 0, first_item));
	opennova::mission::EntityTransform edited = first_item.transform;
	edited.x += 12.5f;
	edited.y -= 3.0f;
	edited.z += 8.25f;
	edited.pitch += 1;
	edited.yaw += 2;
	edited.roll += 3;
	TEST_EXPECT(document.set_entity_transform(opennova::mission::EntityKind::Item, 0, edited));

	opennova::mission::EntityRecord reread;
	TEST_EXPECT(document.get_entity(opennova::mission::EntityKind::Item, 0, reread));
	TEST_EXPECT(reread.transform.x == edited.x);
	TEST_EXPECT(reread.transform.y == edited.y);
	TEST_EXPECT(reread.transform.z == edited.z);
	TEST_EXPECT(reread.transform.pitch == edited.pitch);
	TEST_EXPECT(reread.transform.yaw == edited.yaw);
	TEST_EXPECT(reread.transform.roll == edited.roll);

	opennova::mission::EntityProperties properties;
	properties.group_id = 7;
	properties.waypoint_id = 3;
	properties.wp_number = 12;
	properties.team = 2;
	properties.ai_flags = static_cast<int>(opennova::bms::BmsiAttributeFlags::Blind) |
	                      static_cast<int>(opennova::bms::BmsiAttributeFlags::NoShadow) |
	                      (1 << 29);
	properties.perception = 88;
	properties.accuracy = 66;
	properties.alert_state = 4;
	properties.min_engagement_distance = 30;
	properties.max_engagement_distance = 333;
	properties.max_attack_distance = 444;
	properties.spawn_count = 5;
	properties.max_simultaneous = 2;
	opennova::mission::EntityRecord property_updated;
	TEST_EXPECT(document.set_entity_properties(opennova::mission::EntityKind::Item, 0, properties, &property_updated));
	TEST_EXPECT(property_updated.bms_id == first_item.bms_id);
	TEST_EXPECT(property_updated.item_id == first_item.item_id);
	TEST_EXPECT(property_updated.transform.x == edited.x);
	TEST_EXPECT(property_updated.transform.y == edited.y);
	TEST_EXPECT(property_updated.transform.z == edited.z);
	TEST_EXPECT(property_updated.group_id == properties.group_id);
	TEST_EXPECT(property_updated.waypoint_id == properties.waypoint_id);
	TEST_EXPECT(property_updated.wp_number == properties.wp_number);
	TEST_EXPECT(property_updated.team == properties.team);
	TEST_EXPECT(property_updated.ai_flags == properties.ai_flags);
	TEST_EXPECT(property_updated.perception == properties.perception);
	TEST_EXPECT(property_updated.accuracy == properties.accuracy);
	TEST_EXPECT(property_updated.alert_state == properties.alert_state);
	TEST_EXPECT(property_updated.min_engagement_distance == properties.min_engagement_distance);
	TEST_EXPECT(property_updated.max_engagement_distance == properties.max_engagement_distance);
	TEST_EXPECT(property_updated.max_attack_distance == properties.max_attack_distance);
	TEST_EXPECT(property_updated.spawn_count == properties.spawn_count);
	TEST_EXPECT(property_updated.max_simultaneous == properties.max_simultaneous);

	opennova::mission::EntityTransform placed;
	placed.x = 1.0f;
	placed.y = 2.0f;
	placed.z = 3.0f;
	placed.yaw = 90;
	opennova::mission::EntityRecord added;
	TEST_EXPECT(document.add_entity(opennova::mission::EntityKind::Item, 101291, placed, &added));
	TEST_EXPECT(document.entity_count(opennova::mission::EntityKind::Item) == original_item_count + 1);
	TEST_EXPECT(added.item_id == 101291);
	TEST_EXPECT(added.bms_type_id == 1291);

	std::vector<uint8_t> edited_bytes;
	TEST_EXPECT(document.write_bms_bytes(edited_bytes));
	opennova::mission::MissionDocument reparsed;
	TEST_EXPECT(reparsed.load_bms_bytes(edited_bytes.data(), edited_bytes.size()));
	TEST_EXPECT(reparsed.entity_count(opennova::mission::EntityKind::Item) == original_item_count + 1);
	opennova::mission::EntityRecord reparsed_first_item;
	TEST_EXPECT(reparsed.get_entity(opennova::mission::EntityKind::Item, 0, reparsed_first_item));
	TEST_EXPECT(reparsed_first_item.group_id == properties.group_id);
	TEST_EXPECT(reparsed_first_item.waypoint_id == properties.waypoint_id);
	TEST_EXPECT(reparsed_first_item.wp_number == properties.wp_number);
	TEST_EXPECT(reparsed_first_item.team == properties.team);
	TEST_EXPECT(reparsed_first_item.ai_flags == properties.ai_flags);
	TEST_EXPECT(reparsed_first_item.perception == properties.perception);
	TEST_EXPECT(reparsed_first_item.accuracy == properties.accuracy);
	TEST_EXPECT(reparsed_first_item.alert_state == properties.alert_state);
	TEST_EXPECT(reparsed_first_item.min_engagement_distance == properties.min_engagement_distance);
	TEST_EXPECT(reparsed_first_item.max_engagement_distance == properties.max_engagement_distance);
	TEST_EXPECT(reparsed_first_item.max_attack_distance == properties.max_attack_distance);
	TEST_EXPECT(reparsed_first_item.spawn_count == properties.spawn_count);
	TEST_EXPECT(reparsed_first_item.max_simultaneous == properties.max_simultaneous);
	const std::vector<opennova::mission::WaypointSummary> waypoint_summaries = reparsed.waypoint_summaries();
	TEST_EXPECT(waypoint_summaries.size() == opennova::bms::kWaypointRecordCount);
	TEST_EXPECT(std::any_of(waypoint_summaries.begin(), waypoint_summaries.end(), [](const opennova::mission::WaypointSummary &summary) {
		return summary.marker_count > 0;
	}));
	TEST_EXPECT(reparsed.waypoint_path_count() == opennova::bms::kWaypointRecordCount);
	opennova::mission::WaypointPath path_zero;
	TEST_EXPECT(reparsed.get_waypoint_path(0, path_zero));
	TEST_EXPECT(path_zero.index == 0);

	const size_t original_marker_count = reparsed.entity_count(opennova::mission::EntityKind::Marker);
	opennova::mission::WaypointPath cleared_path;
	TEST_EXPECT(reparsed.clear_waypoint_path(1, &cleared_path));
	TEST_EXPECT(cleared_path.index == 1);
	TEST_EXPECT(cleared_path.marker_indices.empty());

	opennova::mission::EntityTransform waypoint_transform;
	waypoint_transform.x = 40.0f;
	waypoint_transform.y = 41.0f;
	waypoint_transform.z = 42.0f;
	opennova::mission::EntityRecord first_waypoint_marker;
	opennova::mission::WaypointPath edited_path;
	TEST_EXPECT(reparsed.add_waypoint_marker(1, 100001, waypoint_transform, -1, &first_waypoint_marker, &edited_path));
	TEST_EXPECT(reparsed.entity_count(opennova::mission::EntityKind::Marker) == original_marker_count + 1);
	TEST_EXPECT(first_waypoint_marker.kind == opennova::mission::EntityKind::Marker);
	TEST_EXPECT(edited_path.marker_indices.size() == 1);
	TEST_EXPECT(edited_path.marker_indices[0] == static_cast<int>(first_waypoint_marker.index));

	const int waypoint_flags = static_cast<int>(opennova::bms::WaypointFlags::DoesNotLoop) |
	                           static_cast<int>(opennova::bms::WaypointFlags::BlueTeam);
	std::vector<int> marker_order = {static_cast<int>(first_waypoint_marker.index)};
	TEST_EXPECT(reparsed.set_waypoint_path(1, marker_order, waypoint_flags, &edited_path));
	TEST_EXPECT(edited_path.flags == waypoint_flags);
	TEST_EXPECT(edited_path.marker_indices == marker_order);

	opennova::mission::EntityTransform inserted_transform;
	inserted_transform.x = 50.0f;
	inserted_transform.y = 51.0f;
	inserted_transform.z = 52.0f;
	opennova::mission::EntityRecord inserted_waypoint_marker;
	TEST_EXPECT(reparsed.add_waypoint_marker(1, 100001, inserted_transform, 0, &inserted_waypoint_marker, &edited_path));
	TEST_EXPECT(reparsed.entity_count(opennova::mission::EntityKind::Marker) == original_marker_count + 2);
	TEST_EXPECT(edited_path.marker_indices.size() == 2);
	TEST_EXPECT(edited_path.marker_indices[0] == static_cast<int>(inserted_waypoint_marker.index));
	TEST_EXPECT(edited_path.marker_indices[1] == static_cast<int>(first_waypoint_marker.index));

	TEST_EXPECT(reparsed.remove_entity(opennova::mission::EntityKind::Marker, first_waypoint_marker.index));
	opennova::mission::WaypointPath repaired_path;
	TEST_EXPECT(reparsed.get_waypoint_path(1, repaired_path));
	TEST_EXPECT(repaired_path.marker_indices.size() == 1);
	TEST_EXPECT(repaired_path.marker_indices[0] == static_cast<int>(inserted_waypoint_marker.index - 1));

	std::vector<uint8_t> waypoint_bytes;
	TEST_EXPECT(reparsed.write_bms_bytes(waypoint_bytes));
	opennova::mission::MissionDocument waypoint_roundtrip;
	TEST_EXPECT(waypoint_roundtrip.load_bms_bytes(waypoint_bytes.data(), waypoint_bytes.size()));
	opennova::mission::WaypointPath roundtrip_path;
	TEST_EXPECT(waypoint_roundtrip.get_waypoint_path(1, roundtrip_path));
	TEST_EXPECT(roundtrip_path.flags == waypoint_flags);
	TEST_EXPECT(roundtrip_path.marker_indices.size() == 1);
	TEST_EXPECT(roundtrip_path.marker_indices[0] == static_cast<int>(inserted_waypoint_marker.index - 1));

	TEST_EXPECT(waypoint_roundtrip.area_trigger_count() == bms_file.area_triggers.size());
	TEST_EXPECT(waypoint_roundtrip.event_count() == bms_file.events.size());
	TEST_EXPECT(waypoint_roundtrip.trigger_count() == bms_file.triggers.size());
	TEST_EXPECT(waypoint_roundtrip.action_count() == bms_file.actions.size());
	TEST_EXPECT(waypoint_roundtrip.event_count() > 0);
	TEST_EXPECT(waypoint_roundtrip.trigger_count() > 0);
	TEST_EXPECT(waypoint_roundtrip.action_count() > 0);
	opennova::mission::MissionLogicSummary logic_summary = waypoint_roundtrip.logic_summary();
	TEST_EXPECT(logic_summary.event_count == waypoint_roundtrip.event_count());
	TEST_EXPECT(logic_summary.trigger_count == waypoint_roundtrip.trigger_count());
	TEST_EXPECT(logic_summary.action_count == waypoint_roundtrip.action_count());
	TEST_EXPECT(logic_summary.area_trigger_count == waypoint_roundtrip.area_trigger_count());

	opennova::mission::MissionEventRecord first_event;
	TEST_EXPECT(waypoint_roundtrip.get_event(0, first_event));
	TEST_EXPECT(first_event.index == 0);
	opennova::mission::MissionEventChain first_chain;
	TEST_EXPECT(waypoint_roundtrip.get_event_chain(0, first_chain));
	TEST_EXPECT(first_chain.event.index == 0);
	TEST_EXPECT(first_chain.triggers.size() == static_cast<size_t>(first_chain.event.trigger_count));
	TEST_EXPECT(first_chain.actions.size() == static_cast<size_t>(first_chain.event.action_count));
	TEST_EXPECT(first_chain.references.size() >= first_chain.triggers.size() + first_chain.actions.size());
	if (!first_chain.triggers.empty()) {
		TEST_EXPECT(!first_chain.triggers[0].main_type_name.empty());
		TEST_EXPECT(!first_chain.triggers[0].sub_type_name.empty());
	}
	if (!first_chain.actions.empty()) {
		TEST_EXPECT(!first_chain.actions[0].action_type_name.empty());
		TEST_EXPECT(!first_chain.actions[0].action_sub_type_name.empty());
	}

	size_t mutable_event_index = 0;
	opennova::mission::MissionEventChain mutable_chain;
	bool found_mutable_chain = false;
	for (size_t i = 0; i < waypoint_roundtrip.event_count(); ++i) {
		if (waypoint_roundtrip.get_event_chain(i, mutable_chain) &&
		    mutable_chain.event.trigger_count < 20 &&
		    mutable_chain.event.action_count < 20) {
			mutable_event_index = i;
			found_mutable_chain = true;
			break;
		}
	}
	TEST_EXPECT(found_mutable_chain);
	opennova::mission::MissionEventRecord edited_event = mutable_chain.event;
	edited_event.flags |= static_cast<int>(opennova::bms::EventFlags::PreMission);
	edited_event.delay += 7;
	opennova::mission::MissionEventRecord reread_event;
	TEST_EXPECT(waypoint_roundtrip.set_event(mutable_event_index, edited_event, &reread_event));
	TEST_EXPECT((reread_event.flags & static_cast<int>(opennova::bms::EventFlags::PreMission)) != 0);
	TEST_EXPECT(reread_event.delay == edited_event.delay);

	const size_t before_trigger_count = waypoint_roundtrip.trigger_count();
	opennova::mission::MissionTriggerRecord new_trigger;
	new_trigger.condition_flags = 0;
	new_trigger.main_type = static_cast<int>(opennova::bms::TriggerMainType::MissionVariable);
	new_trigger.sub_type = static_cast<int>(opennova::bms::MissionVariableTriggerType::MissionVariableIsGreaterThan);
	new_trigger.param1 = 3;
	new_trigger.param2 = 9;
	const size_t trigger_insert_index = mutable_chain.triggers.size();
	opennova::mission::MissionEventChain trigger_insert_chain;
	TEST_EXPECT(waypoint_roundtrip.insert_event_trigger(mutable_event_index, trigger_insert_index, new_trigger, &trigger_insert_chain));
	TEST_EXPECT(waypoint_roundtrip.trigger_count() == before_trigger_count + 1);
	TEST_EXPECT(trigger_insert_chain.triggers.size() == mutable_chain.triggers.size() + 1);
	opennova::mission::MissionTriggerRecord edited_trigger = trigger_insert_chain.triggers.back();
	edited_trigger.param2 = 11;
	opennova::mission::MissionTriggerRecord reread_trigger;
	TEST_EXPECT(waypoint_roundtrip.set_trigger(edited_trigger.index, edited_trigger, &reread_trigger));
	TEST_EXPECT(reread_trigger.param2 == 11);
	TEST_EXPECT(waypoint_roundtrip.remove_event_trigger(mutable_event_index, trigger_insert_index, &mutable_chain));
	TEST_EXPECT(waypoint_roundtrip.trigger_count() == before_trigger_count);

	const size_t before_action_count = waypoint_roundtrip.action_count();
	opennova::mission::MissionActionRecord new_action;
	new_action.action_type = static_cast<int>(opennova::bms::ActionType::ResetEvent);
	new_action.param1 = static_cast<int>(mutable_event_index);
	const size_t action_insert_index = mutable_chain.actions.size();
	opennova::mission::MissionEventChain action_insert_chain;
	TEST_EXPECT(waypoint_roundtrip.insert_event_action(mutable_event_index, action_insert_index, new_action, &action_insert_chain));
	TEST_EXPECT(waypoint_roundtrip.action_count() == before_action_count + 1);
	TEST_EXPECT(action_insert_chain.actions.size() == mutable_chain.actions.size() + 1);
	opennova::mission::MissionActionRecord edited_action = action_insert_chain.actions.back();
	edited_action.param1 = 0;
	opennova::mission::MissionActionRecord reread_action;
	TEST_EXPECT(waypoint_roundtrip.set_action(edited_action.index, edited_action, &reread_action));
	TEST_EXPECT(reread_action.param1 == 0);
	TEST_EXPECT(waypoint_roundtrip.remove_event_action(mutable_event_index, action_insert_index, &mutable_chain));
	TEST_EXPECT(waypoint_roundtrip.action_count() == before_action_count);

	opennova::bms::Event invalid_event = {};
	invalid_event.trigger_index = static_cast<int32_t>(waypoint_roundtrip.trigger_count() + 10);
	invalid_event.trigger_count = 1;
	waypoint_roundtrip.bms_file().events.push_back(invalid_event);
	waypoint_roundtrip.sync_counts();
	opennova::mission::MissionEventChain invalid_chain;
	TEST_EXPECT(waypoint_roundtrip.get_event_chain(waypoint_roundtrip.event_count() - 1, invalid_chain));
	TEST_EXPECT(!invalid_chain.diagnostics.empty());

	TEST_EXPECT(reparsed.remove_entity(opennova::mission::EntityKind::Item, original_item_count));
	TEST_EXPECT(reparsed.entity_count(opennova::mission::EntityKind::Item) == original_item_count);

	std::string mis_text;
	TEST_EXPECT(reparsed.write_mis_text(mis_text));
	TEST_EXPECT(!mis_text.empty());
	TEST_EXPECT(mis_text.find("\r\n") != std::string::npos);
	for (size_t i = 0; i < mis_text.size(); ++i) {
		if (mis_text[i] == '\n') {
			TEST_EXPECT(i > 0);
			TEST_EXPECT(mis_text[i - 1] == '\r');
		}
	}
	TEST_EXPECT(mis_text.find("begin general_information\r\n") != std::string::npos);
	TEST_EXPECT(mis_text.find("  lowest_elev 0\r\n") != std::string::npos);
	TEST_EXPECT(mis_text.find("  wp_names_blue 0 0 0 0 0 0 0 0\r\n") != std::string::npos);
	TEST_EXPECT(mis_text.find("  default_primary   WPN_M16M203BURST\r\n") != std::string::npos);
	TEST_EXPECT(mis_text.find("begin weapon_availability\r\n") != std::string::npos);
	TEST_EXPECT(mis_text.find("begin item 0\r\n") != std::string::npos);
	TEST_EXPECT(mis_text.find("  type_id ") != std::string::npos);
	TEST_EXPECT(mis_text.find("  position ") != std::string::npos);
	TEST_EXPECT(mis_text.find("begin event 0\r\n") != std::string::npos);
	TEST_EXPECT(mis_text.find("//bms") == std::string::npos);

	return 0;
}
