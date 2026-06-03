#include <cstdint>
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

	OpenNovaMissionDocument *document = opennova_mission_create();
	TEST_EXPECT(document != nullptr);
	TEST_EXPECT(opennova_mission_load_bytes(document, original.data(), original.size()) == 1);
	TEST_EXPECT(opennova_mission_is_loaded(document) == 1);

	OpenNovaMissionInfo info = {};
	TEST_EXPECT(opennova_mission_get_info(document, &info) == 1);
	TEST_EXPECT(info.mission_name[0] != '\0');

	const size_t item_count = opennova_mission_entity_count(document, 1);
	TEST_EXPECT(item_count > 0);

	OpenNovaMissionEntityRecord entity = {};
	TEST_EXPECT(opennova_mission_get_entity(document, 1, 0, &entity) == 1);
	OpenNovaMissionEntityTransform transform = entity.transform;
	transform.x += 4.0f;
	transform.y += 5.0f;
	transform.z += 6.0f;
	transform.yaw += 15;
	TEST_EXPECT(opennova_mission_set_entity_transform(document, 1, 0, &transform) == 1);

	OpenNovaMissionEntityRecord edited = {};
	TEST_EXPECT(opennova_mission_get_entity(document, 1, 0, &edited) == 1);
	TEST_EXPECT(edited.transform.x == transform.x);
	TEST_EXPECT(edited.transform.y == transform.y);
	TEST_EXPECT(edited.transform.z == transform.z);
	TEST_EXPECT(edited.transform.yaw == transform.yaw);

	OpenNovaMissionEntityProperties properties = {};
	properties.group_id = 6;
	properties.waypoint_id = 4;
	properties.wp_number = 9;
	properties.team = 3;
	properties.ai_flags = static_cast<int>(opennova::bms::BmsiAttributeFlags::Guarding) | (1 << 28);
	properties.perception = 77;
	properties.accuracy = 63;
	properties.alert_state = 5;
	properties.min_engagement_distance = 40;
	properties.max_engagement_distance = 260;
	properties.max_attack_distance = 380;
	properties.spawn_count = 4;
	properties.max_simultaneous = 2;
	OpenNovaMissionEntityRecord property_updated = {};
	TEST_EXPECT(opennova_mission_set_entity_properties(document, 1, 0, &properties, &property_updated) == 1);
	TEST_EXPECT(property_updated.bms_id == entity.bms_id);
	TEST_EXPECT(property_updated.item_id == entity.item_id);
	TEST_EXPECT(property_updated.transform.x == transform.x);
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

	TEST_EXPECT(opennova_mission_waypoint_summary_count(document) == opennova::bms::kWaypointRecordCount);
	OpenNovaMissionWaypointSummary waypoint_summary = {};
	TEST_EXPECT(opennova_mission_get_waypoint_summary(document, 0, &waypoint_summary) == 1);
	TEST_EXPECT(waypoint_summary.index == 0);
	TEST_EXPECT(opennova_mission_waypoint_path_count(document) == opennova::bms::kWaypointRecordCount);
	OpenNovaMissionWaypointPath waypoint_path = {};
	TEST_EXPECT(opennova_mission_get_waypoint_path(document, 0, &waypoint_path) == 1);
	TEST_EXPECT(waypoint_path.index == 0);
	TEST_EXPECT(opennova_mission_clear_waypoint_path(document, 1, &waypoint_path) == 1);
	TEST_EXPECT(waypoint_path.index == 1);
	TEST_EXPECT(waypoint_path.marker_count == 0);

	const size_t marker_count = opennova_mission_entity_count(document, 0);
	OpenNovaMissionEntityTransform waypoint_transform = {};
	waypoint_transform.x = 11.0f;
	waypoint_transform.y = 12.0f;
	waypoint_transform.z = 13.0f;
	OpenNovaMissionEntityRecord waypoint_marker = {};
	TEST_EXPECT(opennova_mission_add_waypoint_marker(document, 1, 100001, &waypoint_transform, -1, &waypoint_marker, &waypoint_path) == 1);
	TEST_EXPECT(opennova_mission_entity_count(document, 0) == marker_count + 1);
	TEST_EXPECT(waypoint_marker.kind == 0);
	TEST_EXPECT(waypoint_path.marker_count == 1);
	TEST_EXPECT(waypoint_path.marker_indices[0] == waypoint_marker.index);
	const uint32_t marker_indices[] = {static_cast<uint32_t>(waypoint_marker.index)};
	const int waypoint_flags = static_cast<int>(opennova::bms::WaypointFlags::RedTeam);
	TEST_EXPECT(opennova_mission_set_waypoint_path(document, 1, marker_indices, 1, waypoint_flags, &waypoint_path) == 1);
	TEST_EXPECT(waypoint_path.flags == waypoint_flags);
	TEST_EXPECT(waypoint_path.marker_count == 1);
	TEST_EXPECT(waypoint_path.marker_indices[0] == waypoint_marker.index);

	const size_t area_count = opennova_mission_area_trigger_count(document);
	OpenNovaMissionAreaTriggerRecord area = {};
	if (area_count > 0) {
		TEST_EXPECT(opennova_mission_get_area_trigger(document, 0, &area) == 1);
		TEST_EXPECT(area.index == 0);
	}
	TEST_EXPECT(opennova_mission_get_area_trigger(document, area_count + 1, &area) == 0);

	// Phase 2: area-trigger add / set / remove round-trip through the typed mutators.
	OpenNovaMissionAreaTriggerRecord new_zone = {};
	new_zone.wp_number = 7;
	new_zone.min_x = -10.0f;
	new_zone.max_x = 10.0f;
	new_zone.min_y = -20.0f;
	new_zone.max_y = 20.0f;
	new_zone.min_z = -5.0f;
	new_zone.max_z = 5.0f;
	new_zone.active = 1;
	new_zone.constrain_z = 1;
	OpenNovaMissionAreaTriggerRecord added_zone = {};
	TEST_EXPECT(opennova_mission_add_area_trigger(document, &new_zone, &added_zone) == 1);
	TEST_EXPECT(opennova_mission_area_trigger_count(document) == area_count + 1);
	TEST_EXPECT(added_zone.index == area_count);
	TEST_EXPECT(added_zone.wp_number == 7);
	TEST_EXPECT(added_zone.active == 1);
	TEST_EXPECT(added_zone.constrain_z == 1);
	// Fixed-point 16.16 round-trips the bounds exactly for these whole-unit values.
	TEST_EXPECT(added_zone.min_x == -10.0f && added_zone.max_x == 10.0f);
	TEST_EXPECT(added_zone.min_z == -5.0f && added_zone.max_z == 5.0f);
	// Edit it: clear constrain_z, move max_x.
	added_zone.constrain_z = 0;
	added_zone.max_x = 25.0f;
	OpenNovaMissionAreaTriggerRecord edited_zone = {};
	TEST_EXPECT(opennova_mission_set_area_trigger(document, added_zone.index, &added_zone, &edited_zone) == 1);
	TEST_EXPECT(edited_zone.constrain_z == 0);
	TEST_EXPECT(edited_zone.active == 1);
	TEST_EXPECT(edited_zone.max_x == 25.0f);
	// Remove it: count returns to baseline.
	TEST_EXPECT(opennova_mission_remove_area_trigger(document, added_zone.index) == 1);
	TEST_EXPECT(opennova_mission_area_trigger_count(document) == area_count);
	// Out-of-range guards.
	TEST_EXPECT(opennova_mission_set_area_trigger(document, area_count + 99, &new_zone, nullptr) == 0);
	TEST_EXPECT(opennova_mission_remove_area_trigger(document, area_count + 99) == 0);

	TEST_EXPECT(opennova_mission_event_count(document) > 0);
	TEST_EXPECT(opennova_mission_trigger_count(document) > 0);
	TEST_EXPECT(opennova_mission_action_count(document) > 0);
	OpenNovaMissionEventRecord event = {};
	TEST_EXPECT(opennova_mission_get_event(document, 0, &event) == 1);
	TEST_EXPECT(event.index == 0);
	OpenNovaMissionTriggerRecord trigger = {};
	TEST_EXPECT(opennova_mission_get_trigger(document, 0, &trigger) == 1);
	TEST_EXPECT(trigger.index == 0);
	TEST_EXPECT(trigger.main_type_name[0] != '\0');
	TEST_EXPECT(trigger.sub_type_name[0] != '\0');
	OpenNovaMissionActionRecord action = {};
	TEST_EXPECT(opennova_mission_get_action(document, 0, &action) == 1);
	TEST_EXPECT(action.index == 0);
	TEST_EXPECT(action.action_type_name[0] != '\0');
	TEST_EXPECT(action.action_sub_type_name[0] != '\0');
	OpenNovaMissionLogicSummary logic_summary = {};
	TEST_EXPECT(opennova_mission_get_logic_summary(document, &logic_summary) == 1);
	TEST_EXPECT(logic_summary.event_count == opennova_mission_event_count(document));
	TEST_EXPECT(logic_summary.trigger_count == opennova_mission_trigger_count(document));
	TEST_EXPECT(logic_summary.action_count == opennova_mission_action_count(document));
	TEST_EXPECT(logic_summary.area_trigger_count == opennova_mission_area_trigger_count(document));

	OpenNovaMissionEntityTransform placed = {};
	placed.x = 7.0f;
	placed.y = 8.0f;
	placed.z = 9.0f;
	OpenNovaMissionEntityRecord added = {};
	TEST_EXPECT(opennova_mission_add_entity(document, 1, 101291, &placed, &added) == 1);
	TEST_EXPECT(added.item_id == 101291);
	TEST_EXPECT(opennova_mission_entity_count(document, 1) == item_count + 1);

	OpenNovaMissionBytes bytes = {};
	TEST_EXPECT(opennova_mission_write_bytes(document, &bytes) == 1);
	TEST_EXPECT(bytes.data != nullptr);
	TEST_EXPECT(bytes.size > original.size());
	opennova_mission_free_bytes(&bytes);
	TEST_EXPECT(bytes.data == nullptr);
	TEST_EXPECT(bytes.size == 0);

	OpenNovaMissionBytes mis = {};
	TEST_EXPECT(opennova_mission_write_mis_text(document, &mis) == 1);
	TEST_EXPECT(mis.data != nullptr);
	TEST_EXPECT(mis.size > 0);
	const std::string mis_text(reinterpret_cast<const char *>(mis.data), mis.size);
	TEST_EXPECT(mis_text.find("begin general_information\r\n") != std::string::npos);
	TEST_EXPECT(mis_text.find("  default_secondary WPN_colt45\r\n") != std::string::npos);
	TEST_EXPECT(mis_text.find("begin item 0\r\n") != std::string::npos);
	TEST_EXPECT(mis_text.find("//bms") == std::string::npos);
	opennova_mission_free_bytes(&mis);
	TEST_EXPECT(mis.data == nullptr);
	TEST_EXPECT(mis.size == 0);

	opennova_mission_destroy(document);
	return 0;
}
