#include <algorithm>
#include <cstddef>
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

	// --- Phase 0: version gate. Engine rejects magic[3] < 19 (@0x40f5aa); the magic sniff
	// (is_bms) still passes because the gate lives in the loader, mirroring the engine. ---
	{
		std::vector<uint8_t> bad_version(original);
		bad_version[3] = 18;
		opennova::bms::File rejected;
		std::string gate_error;
		TEST_EXPECT(opennova::bms::is_bms(bad_version.data(), bad_version.size()));
		TEST_EXPECT(!opennova::bms::parse(bad_version.data(), bad_version.size(), rejected, gate_error));
		TEST_EXPECT(!gate_error.empty());
		bad_version[3] = 19;
		opennova::bms::File accepted;
		TEST_EXPECT(opennova::bms::parse(bad_version.data(), bad_version.size(), accepted, gate_error));
	}

	// --- Phase 0: the second loadout chunk (word_A76416 @hdr+0x246) is consumed, keeping every
	// later section aligned. The fixture has none, so inject one and confirm round-trip + alignment. ---
	{
		opennova::bms::File with_chunk;
		std::string chunk_error;
		TEST_EXPECT(opennova::bms::parse(original.data(), original.size(), with_chunk, chunk_error));
		TEST_EXPECT(with_chunk.secondary_chunk.empty());
		with_chunk.secondary_chunk = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
		with_chunk.header.secondary_chunk_len = static_cast<uint16_t>(with_chunk.secondary_chunk.size());
		std::vector<uint8_t> chunk_bytes;
		TEST_EXPECT(opennova::bms::write(with_chunk, chunk_bytes, chunk_error));
		opennova::bms::File chunk_reparsed;
		TEST_EXPECT(opennova::bms::parse(chunk_bytes.data(), chunk_bytes.size(), chunk_reparsed, chunk_error));
		TEST_EXPECT(chunk_reparsed.secondary_chunk == with_chunk.secondary_chunk);
		// Sections after the chunk stay aligned: counts and the first item match the no-chunk parse.
		TEST_EXPECT(chunk_reparsed.items.size() == with_chunk.items.size());
		TEST_EXPECT(chunk_reparsed.markers.size() == with_chunk.markers.size());
		TEST_EXPECT(chunk_reparsed.area_triggers.size() == with_chunk.area_triggers.size());
		TEST_EXPECT(chunk_reparsed.events.size() == with_chunk.events.size());
		if (!with_chunk.items.empty()) {
			TEST_EXPECT(chunk_reparsed.items[0].type_id == with_chunk.items[0].type_id);
			TEST_EXPECT(chunk_reparsed.items[0].x == with_chunk.items[0].x);
		}
	}

	// --- Phase 0: area-trigger 32-byte record is interleaved per axis (x_min,x_max,y_min,y_max,
	// z_min,z_max,flags), NOT min-triple/max-triple, and there is no Y/Z swap (@0x43c75c). ---
	{
		opennova::bms::File at_file;
		std::string at_error;
		TEST_EXPECT(opennova::bms::parse(original.data(), original.size(), at_file, at_error));
		opennova::bms::AreaTrigger trig{};
		trig.id = 7;
		trig.x_min = 100; trig.x_max = 200;
		trig.y_min = 300; trig.y_max = 400;
		trig.z_min = 500; trig.z_max = 600;
		trig.flags = 0x3; // active + constrain-Z
		at_file.area_triggers.push_back(trig);
		at_file.header.area_trigger_count = static_cast<int16_t>(at_file.area_triggers.size());
		std::vector<uint8_t> at_bytes;
		TEST_EXPECT(opennova::bms::write(at_file, at_bytes, at_error));
		opennova::bms::File at_reparsed;
		TEST_EXPECT(opennova::bms::parse(at_bytes.data(), at_bytes.size(), at_reparsed, at_error));
		TEST_EXPECT(at_reparsed.area_triggers.size() == 1);
		const opennova::bms::AreaTrigger &rt = at_reparsed.area_triggers[0];
		TEST_EXPECT(rt.id == 7);
		TEST_EXPECT(rt.x_min == 100 && rt.x_max == 200);
		TEST_EXPECT(rt.y_min == 300 && rt.y_max == 400);
		TEST_EXPECT(rt.z_min == 500 && rt.z_max == 600);
		TEST_EXPECT(rt.flags == 0x3u);
		TEST_EXPECT(rt.is_active());
		TEST_EXPECT(rt.constrains_z());
		// Per-axis min<max preserved (a wrong min-triple/max-triple layout would scramble these).
		TEST_EXPECT(rt.get_x_min() < rt.get_x_max());
		TEST_EXPECT(rt.get_y_min() < rt.get_y_max());
		TEST_EXPECT(rt.get_z_min() < rt.get_z_max());
		// Events after the area-trigger section are still aligned.
		TEST_EXPECT(at_reparsed.events.size() == at_file.events.size());
	}

	// --- Phase 1: mission-header editing round-trips through save/reload ---
	{
		opennova::mission::MissionDocument hdr;
		TEST_EXPECT(hdr.load_bms_bytes(original.data(), original.size()));
		TEST_EXPECT(hdr.set_header_string("mission_name", "Grill Test"));
		TEST_EXPECT(hdr.set_header_int("climate", 2));
		TEST_EXPECT(hdr.set_header_int("minutes_per_day", 1234));
		const int coop_bit = 0x1000000; // ATTRIB_COOP
		TEST_EXPECT(hdr.set_header_flag(coop_bit, true));
		TEST_EXPECT(!hdr.set_header_string("nonexistent_field", "x")); // unknown rejected
		std::vector<uint8_t> hdr_bytes;
		TEST_EXPECT(hdr.write_bms_bytes(hdr_bytes));
		opennova::mission::MissionDocument hdr_reload;
		TEST_EXPECT(hdr_reload.load_bms_bytes(hdr_bytes.data(), hdr_bytes.size()));
		const opennova::mission::MissionInfo reread_info = hdr_reload.info();
		TEST_EXPECT(reread_info.mission_name == "Grill Test");
		TEST_EXPECT(reread_info.climate == 2);
		TEST_EXPECT(reread_info.minutes_per_day == 1234);
		TEST_EXPECT((reread_info.attrib_flags & coop_bit) != 0);
	}

	// --- Phase 1: hidden entity fields (name1/name2/no_less_than/map_symbol) round-trip,
	// and the names use the format's full 8-byte slot (a name longer than 8 is cut to 8). ---
	{
		opennova::mission::MissionDocument doc;
		TEST_EXPECT(doc.load_bms_bytes(original.data(), original.size()));
		opennova::mission::EntityRecord rec;
		TEST_EXPECT(doc.get_entity(opennova::mission::EntityKind::Item, 0, rec));
		opennova::mission::EntityProperties props; // seed from current record (overwrites all)
		props.group_id = rec.group_id;
		props.waypoint_id = rec.waypoint_id;
		props.wp_number = rec.wp_number;
		props.team = rec.team;
		props.ai_flags = rec.ai_flags;
		props.perception = rec.perception;
		props.accuracy = rec.accuracy;
		props.alert_state = rec.alert_state;
		props.min_engagement_distance = rec.min_engagement_distance;
		props.max_engagement_distance = rec.max_engagement_distance;
		props.max_attack_distance = rec.max_attack_distance;
		props.spawn_count = rec.spawn_count;
		props.max_simultaneous = rec.max_simultaneous;
		props.no_less_than = 9;
		props.map_symbol = 17;
		props.name1 = "rifle";
		props.name2 = "patrol";
		TEST_EXPECT(doc.set_entity_properties(opennova::mission::EntityKind::Item, 0, props, nullptr));
		std::vector<uint8_t> bytes;
		TEST_EXPECT(doc.write_bms_bytes(bytes));
		opennova::mission::MissionDocument reload;
		TEST_EXPECT(reload.load_bms_bytes(bytes.data(), bytes.size()));
		opennova::mission::EntityRecord rec2;
		TEST_EXPECT(reload.get_entity(opennova::mission::EntityKind::Item, 0, rec2));
		TEST_EXPECT(rec2.no_less_than == 9);
		TEST_EXPECT(rec2.map_symbol == 17);
		TEST_EXPECT(rec2.name1 == "rifle");
		TEST_EXPECT(rec2.name2 == "patrol");
		TEST_EXPECT(rec2.max_simultaneous == rec.max_simultaneous); // no_more_than preserved
		// An exactly-8-char name keeps all 8 bytes: copy_cstr used to force a NUL into byte 7 and
		// drop the 8th char, silently corrupting an unedited AI class name on every property edit.
		props.name1 = "rifleman"; // 8 chars
		TEST_EXPECT(doc.set_entity_properties(opennova::mission::EntityKind::Item, 0, props, nullptr));
		opennova::mission::EntityRecord rec_full;
		TEST_EXPECT(doc.get_entity(opennova::mission::EntityKind::Item, 0, rec_full));
		TEST_EXPECT(rec_full.name1 == "rifleman");
		props.name1 = "verylongname"; // > 8 chars -> cut to the 8-byte slot
		TEST_EXPECT(doc.set_entity_properties(opennova::mission::EntityKind::Item, 0, props, nullptr));
		opennova::mission::EntityRecord rec3;
		TEST_EXPECT(doc.get_entity(opennova::mission::EntityKind::Item, 0, rec3));
		TEST_EXPECT(rec3.name1 == "verylong");
		TEST_EXPECT(rec3.name1.size() == 8);
	}

	// --- Regression (review): event reset_after/delay round-trip across the full 0..1023 range.
	// The values pack into the upper 10 bits; the old signed `value << 22` was UB for value >= 512
	// (1023 << 22 overflows INT32_MAX) and the signed `>> 22` read sign-extended to a negative. ---
	{
		opennova::bms::File f;
		std::string err;
		TEST_EXPECT(opennova::bms::parse(original.data(), original.size(), f, err));
		if (f.events.empty()) {
			f.events.push_back(opennova::bms::Event{});
			f.events_count = 1;
		}
		f.events[0].reset_after = 1023;
		f.events[0].delay = 600;
		std::vector<uint8_t> bytes;
		TEST_EXPECT(opennova::bms::write(f, bytes, err));
		opennova::bms::File reparsed;
		TEST_EXPECT(opennova::bms::parse(bytes.data(), bytes.size(), reparsed, err));
		TEST_EXPECT(!reparsed.events.empty());
		TEST_EXPECT(reparsed.events[0].reset_after == 1023);
		TEST_EXPECT(reparsed.events[0].delay == 600);
	}

	// --- Regression (review): a corrupt pool count fails the parse cleanly instead of crashing. A
	// huge num_items would otherwise make vector::resize() throw an uncaught length_error/bad_alloc. ---
	{
		std::vector<uint8_t> corrupt = original;
		const size_t off = offsetof(opennova::bms::Header, num_items);
		corrupt[off + 0] = 0xFF;
		corrupt[off + 1] = 0xFF;
		corrupt[off + 2] = 0xFF;
		corrupt[off + 3] = 0xFF;
		opennova::bms::File f;
		std::string err;
		TEST_EXPECT(!opennova::bms::parse(corrupt.data(), corrupt.size(), f, err));
	}

	// --- Regression (review): an empty-name loadout entry is skipped, not serialized as a leading
	// NUL the loader reads as the chunk terminator (which would drop it AND every later entry). ---
	{
		opennova::mission::MissionDocument doc;
		TEST_EXPECT(doc.load_bms_bytes(original.data(), original.size()));
		std::vector<opennova::mission::WeaponLoadoutEntry> entries;
		entries.push_back({"WPN_A", "-1", "-1"});
		entries.push_back({"", "-1", "-1"}); // empty name -> skipped, must not truncate the list
		entries.push_back({"WPN_B", "-1", "-1"});
		TEST_EXPECT(doc.set_weapon_loadout(entries));
		const std::vector<opennova::mission::WeaponLoadoutEntry> reread = doc.weapon_loadout();
		TEST_EXPECT(reread.size() == 2);
		TEST_EXPECT(reread[0].name == "WPN_A");
		TEST_EXPECT(reread[1].name == "WPN_B");
	}

	// --- From-scratch: create_default() builds a valid, empty mission that round-trips. This is
	// the first path that writes freshly-resized waypoint records, so it guards the sync_counts
	// padding backfill (without it the writer emits 8-byte waypoint records that fail to reparse). ---
	{
		opennova::mission::MissionDocument fresh;
		fresh.create_default();
		TEST_EXPECT(fresh.is_loaded());
		TEST_EXPECT(fresh.source_path().empty());
		TEST_EXPECT(fresh.entity_count(opennova::mission::EntityKind::Item) == 0);
		TEST_EXPECT(fresh.entity_count(opennova::mission::EntityKind::Marker) == 0);
		std::vector<uint8_t> fresh_bytes;
		TEST_EXPECT(fresh.write_bms_bytes(fresh_bytes));
		TEST_EXPECT(opennova::bms::is_bms(fresh_bytes.data(), fresh_bytes.size()));
		opennova::bms::File fresh_parsed;
		std::string fresh_err;
		TEST_EXPECT(opennova::bms::parse(fresh_bytes.data(), fresh_bytes.size(), fresh_parsed, fresh_err));
		TEST_EXPECT(fresh_parsed.waypoint_records.size() == opennova::bms::kWaypointRecordCount);
		TEST_EXPECT(fresh_parsed.group_records.size() == opennova::bms::kGroupRecordCount);
		TEST_EXPECT(fresh_parsed.layer_records.size() == opennova::bms::kLayerRecordCount);
		TEST_EXPECT(fresh_parsed.header.num_items == 0);
		TEST_EXPECT(fresh_parsed.events.empty());
		// Reparse + rewrite is byte-stable (the snapshot/restore parity the editor relies on).
		opennova::mission::MissionDocument fresh_round;
		TEST_EXPECT(fresh_round.load_bms_bytes(fresh_bytes.data(), fresh_bytes.size()));
		std::vector<uint8_t> rewritten;
		TEST_EXPECT(fresh_round.write_bms_bytes(rewritten));
		TEST_EXPECT(rewritten == fresh_bytes);
		// A from-scratch mission can adopt a terrain ref and round-trip it.
		TEST_EXPECT(fresh.set_header_string("terrain", "dvxi5"));
		std::vector<uint8_t> ref_bytes;
		TEST_EXPECT(fresh.write_bms_bytes(ref_bytes));
		opennova::mission::MissionDocument ref_reload;
		TEST_EXPECT(ref_reload.load_bms_bytes(ref_bytes.data(), ref_bytes.size()));
		TEST_EXPECT(ref_reload.info().terrain == "dvxi5");
	}

	// --- bms::equal: the editor's undo / dirty change-detection. Identity holds across a copy; a
	// scalar, header, structural, or waypoint-record difference is detected; it agrees with
	// serialized-bytes equality; two independently-built defaults compare equal. ---
	{
		opennova::bms::File base;
		std::string err;
		TEST_EXPECT(opennova::bms::parse(original.data(), original.size(), base, err));
		opennova::bms::File copy = base;
		TEST_EXPECT(opennova::bms::equal(base, copy));
		if (!copy.items.empty()) {
			copy.items[0].x += 1;
			TEST_EXPECT(!opennova::bms::equal(base, copy));
			copy.items[0].x -= 1;
			TEST_EXPECT(opennova::bms::equal(base, copy)); // restored -> equal again
		}
		opennova::bms::File hdr_changed = base;
		hdr_changed.header.minutes_per_day = static_cast<uint16_t>(hdr_changed.header.minutes_per_day + 1);
		TEST_EXPECT(!opennova::bms::equal(base, hdr_changed));
		opennova::bms::File grew = base;
		grew.items.push_back(opennova::bms::Entity{});
		TEST_EXPECT(!opennova::bms::equal(base, grew));
		opennova::bms::File wp_changed = base;
		TEST_EXPECT(!wp_changed.waypoint_records.empty());
		wp_changed.waypoint_records[0].flags = static_cast<opennova::bms::WaypointFlags>(
			static_cast<int>(wp_changed.waypoint_records[0].flags) ^ 1);
		TEST_EXPECT(!opennova::bms::equal(base, wp_changed));
		// equal() agrees with serialized-bytes equality.
		std::vector<uint8_t> base_bytes, grew_bytes;
		TEST_EXPECT(opennova::bms::write(base, base_bytes, err));
		TEST_EXPECT(opennova::bms::write(grew, grew_bytes, err));
		TEST_EXPECT((base_bytes == grew_bytes) == opennova::bms::equal(base, grew));
		// Two independently-built defaults serialize identically, so they compare equal.
		opennova::mission::MissionDocument d1;
		opennova::mission::MissionDocument d2;
		d1.create_default();
		d2.create_default();
		TEST_EXPECT(opennova::bms::equal(d1.bms_file(), d2.bms_file()));
	}

	return 0;
}
