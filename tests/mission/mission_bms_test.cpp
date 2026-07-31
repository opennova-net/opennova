#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
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

void write_u16_le(std::vector<uint8_t> &bytes, size_t offset, uint16_t value) {
	bytes[offset] = static_cast<uint8_t>(value & 0xFF);
	bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

uint16_t read_u16_le(const std::vector<uint8_t> &bytes, size_t offset) {
	return static_cast<uint16_t>(bytes[offset]) |
	       (static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

void write_u32_le(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
	bytes[offset] = static_cast<uint8_t>(value & 0xFF);
	bytes[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
	bytes[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
	bytes[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
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
	opennova::bms::File encoded_file;
	TEST_EXPECT(opennova::bms::parse(encoded.data(), encoded.size(), encoded_file, error));
	TEST_EXPECT(opennova::bms::equal(bms_file, encoded_file));
	std::vector<uint8_t> encoded2;
	TEST_EXPECT(opennova::bms::write(encoded_file, encoded2, error));
	TEST_EXPECT(encoded2 == encoded);

	opennova::mission::MissionDocument document;
	TEST_EXPECT(document.load_bms_bytes(original.data(), original.size()));
	TEST_EXPECT(document.is_loaded());
	TEST_EXPECT(!document.info().mission_name.empty());
	const uint16_t original_loadout_len = read_u16_le(original, offsetof(opennova::bms::Header, weapon_loadout_chunk_len));
	const uint16_t original_secondary_len = read_u16_le(original, offsetof(opennova::bms::Header, secondary_chunk_len));

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
	                      static_cast<int>(opennova::bms::BmsiAttributeFlags::NoShadow);
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

	// --- Modeling policy: the second loadout chunk is an item-availability list, not opaque bytes.
	// Arbitrary bytes must fail load instead of being preserved. ---
	{
		opennova::bms::File with_chunk;
		std::string chunk_error;
		TEST_EXPECT(opennova::bms::parse(original.data(), original.size(), with_chunk, chunk_error));
		TEST_EXPECT(with_chunk.item_availability.empty());
		std::vector<uint8_t> chunk_bytes = original;
		const size_t loadout_end = opennova::bms::kHeaderSize + original_loadout_len;
		const uint8_t invalid_availability[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01};
		chunk_bytes.insert(chunk_bytes.begin() + static_cast<std::ptrdiff_t>(loadout_end),
		                   std::begin(invalid_availability), std::end(invalid_availability));
		write_u16_le(chunk_bytes, offsetof(opennova::bms::Header, secondary_chunk_len),
		             static_cast<uint16_t>(sizeof(invalid_availability)));
		opennova::bms::File chunk_reparsed;
		TEST_EXPECT(!opennova::bms::parse(chunk_bytes.data(), chunk_bytes.size(), chunk_reparsed, chunk_error));
		TEST_EXPECT(chunk_error.find("item availability") != std::string::npos);
	}

	// --- Modeling policy: group records are flags/value/constant records. Offset 4 and bytes 16..31
	// are canonical zero, and offset 12 is the writer-confirmed literal 10. ---
	{
		std::string group_error;
		std::vector<uint8_t> group_bytes = original;
		const size_t entity_count = bms_file.header.num_items + bms_file.header.num_buildings +
		                            bms_file.header.num_markers + bms_file.header.num_people;
		const size_t group_offset = opennova::bms::kHeaderSize +
		                            original_loadout_len +
		                            original_secondary_len +
		                            entity_count * opennova::bms::kEntitySize +
		                            opennova::bms::kWaypointRecordCount * opennova::bms::kWaypointRecordSize;
		group_bytes[group_offset + 4] = 1;
		opennova::bms::File rejected_group;
		TEST_EXPECT(!opennova::bms::parse(group_bytes.data(), group_bytes.size(), rejected_group, group_error));
		TEST_EXPECT(group_error.find("group") != std::string::npos);
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

	// --- Modeling policy: event/trigger/action reserved slots and unsupported event flag bits
	// fail load. They are not raw data carried for preservation. ---
	{
		const size_t entity_count = bms_file.header.num_items + bms_file.header.num_buildings +
		                            bms_file.header.num_markers + bms_file.header.num_people;
		const size_t event_block = opennova::bms::kHeaderSize +
		                           original_loadout_len +
		                           original_secondary_len +
		                           entity_count * opennova::bms::kEntitySize +
		                           opennova::bms::kWaypointRecordCount * opennova::bms::kWaypointRecordSize +
		                           opennova::bms::kGroupRecordCount * opennova::bms::kGroupRecordSize +
		                           opennova::bms::kLayerRecordCount * opennova::bms::kLayerRecordSize +
		                           bms_file.header.area_trigger_count * opennova::bms::kAreaTriggerSize;
		const int32_t events = static_cast<int32_t>(bms_file.events.size());
		const int32_t triggers = static_cast<int32_t>(bms_file.triggers.size());
		const size_t first_event = event_block + 12;
		const size_t first_trigger = first_event + events * opennova::bms::kEventSize;
		const size_t first_action = first_trigger + triggers * opennova::bms::kTriggerSize;

		std::string strict_error;
		opennova::bms::File rejected;
		std::vector<uint8_t> bad_event_flags = original;
		write_u32_le(bad_event_flags, first_event, static_cast<uint32_t>(bms_file.events[0].flags) | 0x08u);
		TEST_EXPECT(!opennova::bms::parse(bad_event_flags.data(), bad_event_flags.size(), rejected, strict_error));
		TEST_EXPECT(strict_error.find("event") != std::string::npos);

		std::vector<uint8_t> bad_event_reserved = original;
		bad_event_reserved[first_event + 23] = 1;
		TEST_EXPECT(!opennova::bms::parse(bad_event_reserved.data(), bad_event_reserved.size(), rejected, strict_error));
		TEST_EXPECT(strict_error.find("event") != std::string::npos);

		std::vector<uint8_t> bad_trigger_reserved = original;
		write_u32_le(bad_trigger_reserved, first_trigger + 28, 1);
		TEST_EXPECT(!opennova::bms::parse(bad_trigger_reserved.data(), bad_trigger_reserved.size(), rejected, strict_error));
		TEST_EXPECT(strict_error.find("trigger") != std::string::npos);

		std::vector<uint8_t> bad_action_reserved = original;
		write_u32_le(bad_action_reserved, first_action, 1);
		TEST_EXPECT(!opennova::bms::parse(bad_action_reserved.data(), bad_action_reserved.size(), rejected, strict_error));
		TEST_EXPECT(strict_error.find("action") != std::string::npos);
	}

	// --- Modeling policy: entity AI flags must be a known BmsiAttributeFlags combination. ---
	{
		std::vector<uint8_t> bad_ai_flags = original;
		const size_t first_item_ai_flags = opennova::bms::kHeaderSize +
		                                   original_loadout_len +
		                                   original_secondary_len + 12;
		write_u32_le(bad_ai_flags, first_item_ai_flags, 1u << 29);
		opennova::bms::File rejected;
		std::string ai_error;
		TEST_EXPECT(!opennova::bms::parse(bad_ai_flags.data(), bad_ai_flags.size(), rejected, ai_error));
		TEST_EXPECT(ai_error.find("AI") != std::string::npos);
	}

	// --- Modeling policy: editor-reserved entity fields are canonical zero. ---
	{
		const size_t first_item = opennova::bms::kHeaderSize +
		                          original_loadout_len +
		                          original_secondary_len;
		std::vector<uint8_t> bad_entity_reserved = original;
		bad_entity_reserved[first_item + 82] = 1;
		opennova::bms::File rejected;
		std::string entity_error;
		TEST_EXPECT(!opennova::bms::parse(bad_entity_reserved.data(), bad_entity_reserved.size(), rejected, entity_error));
		TEST_EXPECT(entity_error.find("entity") != std::string::npos);

		std::vector<uint8_t> bad_entity_tail = original;
		bad_entity_tail[first_item + 168] = 1;
		TEST_EXPECT(!opennova::bms::parse(bad_entity_tail.data(), bad_entity_tail.size(), rejected, entity_error));
		TEST_EXPECT(entity_error.find("entity") != std::string::npos);
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

	// --- Regression (review): the terrain slot is a fixed 16-byte field (first of the three
	// 16-byte slots in header.terrain[48]: terrain / cnv_file / tt_file). copy_cstr used to force a
	// NUL into byte 15 and truncate a full 16-char terrain name on every edit; copy_fixed_field
	// keeps all 16, and the info()/get_terrain reads are bounded to 16 so a full slot does not bleed
	// into cnv_file. Editing terrain must also leave cnv_file / tt_file untouched. ---
	{
		opennova::mission::MissionDocument doc;
		TEST_EXPECT(doc.load_bms_bytes(original.data(), original.size()));
		// Seed cnv_file (@+16) and tt_file (@+32) so we can prove a terrain edit preserves them.
		char *terrain_region = doc.bms_file().header.terrain;
		std::memcpy(terrain_region + 16, "convert.cnv", 11);
		terrain_region[16 + 11] = '\0';
		std::memcpy(terrain_region + 32, "tiles.tt", 8);
		terrain_region[32 + 8] = '\0';

		const std::string full16 = "sixteen_char_ter"; // exactly 16 chars, fills the slot
		TEST_EXPECT(full16.size() == 16);
		TEST_EXPECT(doc.set_header_string("terrain", full16));

		std::vector<uint8_t> bytes;
		TEST_EXPECT(doc.write_bms_bytes(bytes));
		opennova::mission::MissionDocument reload;
		TEST_EXPECT(reload.load_bms_bytes(bytes.data(), bytes.size()));
		// All 16 chars survive and do not run on into cnv_file.
		TEST_EXPECT(reload.info().terrain == full16);
		TEST_EXPECT(reload.bms_file().get_terrain() == full16);
		// cnv_file / tt_file are untouched by the terrain edit.
		const char *reload_region = reload.bms_file().header.terrain;
		TEST_EXPECT(std::string(reload_region + 16) == "convert.cnv");
		TEST_EXPECT(std::string(reload_region + 32) == "tiles.tt");

		// A name longer than 16 is cut to the slot; a shorter name still round-trips.
		TEST_EXPECT(doc.set_header_string("terrain", "way_too_long_terrain_name"));
		TEST_EXPECT(doc.info().terrain == "way_too_long_ter"); // 16 chars
		TEST_EXPECT(doc.set_header_string("terrain", "short"));
		TEST_EXPECT(doc.info().terrain == "short");
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

	// --- Modeling policy: gen_string is a 31-byte string plus named option bytes at offsets
	// 152/154/155. The intervening bytes 151 and 153 are reserved zero and fail load if nonzero. ---
	{
		opennova::bms::File f;
		std::string err;
		TEST_EXPECT(opennova::bms::parse(original.data(), original.size(), f, err));
		TEST_EXPECT(!f.items.empty());
		std::memset(f.items[0].gen_string, 0, sizeof(f.items[0].gen_string));
		std::memcpy(f.items[0].gen_string, "generator", 9);
		f.items[0].grenades = 4;
		f.items[0].mission_critical = 1;
		f.items[0].lfp_group = 7;
		std::vector<uint8_t> bytes;
		TEST_EXPECT(opennova::bms::write(f, bytes, err));
		opennova::bms::File reparsed;
		TEST_EXPECT(opennova::bms::parse(bytes.data(), bytes.size(), reparsed, err));
		TEST_EXPECT(std::string(reparsed.items[0].gen_string) == "generator");
		TEST_EXPECT(reparsed.items[0].grenades == 4);
		TEST_EXPECT(reparsed.items[0].mission_critical == 1);
		TEST_EXPECT(reparsed.items[0].lfp_group == 7);

		const size_t first_item_gen = opennova::bms::kHeaderSize +
		                              original_loadout_len +
		                              original_secondary_len + 120;
		std::vector<uint8_t> bad_reserved = original;
		bad_reserved[first_item_gen + 31] = 1;
		opennova::bms::File rejected;
		TEST_EXPECT(!opennova::bms::parse(bad_reserved.data(), bad_reserved.size(), rejected, err));
		TEST_EXPECT(err.find("entity") != std::string::npos);
	}

	// --- Regression (review): set_entity_property_int / _string edit one named field and leave the
	// rest intact, the name->member mapping owning the field list in one place (mirrors set_header_*).
	{
		opennova::mission::MissionDocument doc;
		TEST_EXPECT(doc.load_bms_bytes(original.data(), original.size()));
		opennova::mission::EntityRecord before;
		TEST_EXPECT(doc.get_entity(opennova::mission::EntityKind::Item, 0, before));
		// `group` is the one key whose member name differs (group_id).
		TEST_EXPECT(doc.set_entity_property_int(opennova::mission::EntityKind::Item, 0, "group", 5));
		TEST_EXPECT(doc.set_entity_property_int(opennova::mission::EntityKind::Item, 0, "map_symbol", 22));
		TEST_EXPECT(doc.set_entity_property_string(opennova::mission::EntityKind::Item, 0, "name1", "scout"));
		opennova::mission::EntityRecord after;
		TEST_EXPECT(doc.get_entity(opennova::mission::EntityKind::Item, 0, after));
		TEST_EXPECT(after.group_id == 5);
		TEST_EXPECT(after.map_symbol == 22);
		TEST_EXPECT(after.name1 == "scout");
		// Untouched fields are preserved (only the requested members changed).
		TEST_EXPECT(after.team == before.team);
		TEST_EXPECT(after.accuracy == before.accuracy);
		TEST_EXPECT(after.name2 == before.name2);
		TEST_EXPECT(after.max_engagement_distance == before.max_engagement_distance);
		// Unknown names are rejected (not silently ignored), with last_error set.
		TEST_EXPECT(!doc.set_entity_property_int(opennova::mission::EntityKind::Item, 0, "bogus_field", 1));
		TEST_EXPECT(!doc.last_error().empty());
		TEST_EXPECT(!doc.set_entity_property_string(opennova::mission::EntityKind::Item, 0, "name3", "x"));
		// Out-of-range index is rejected.
		TEST_EXPECT(!doc.set_entity_property_int(opennova::mission::EntityKind::Item, 99999, "team", 1));
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

	// --- Regression (quality campaign W2-7): every truncation of a valid mission has to be
	// handled by the byte cursor, not just the lengths the corpus happens to contain. The BMS
	// Reader's bounds check was `pos_ + count <= size_`; that sum wraps for a large count and
	// then admits a read past the buffer. It is now phrased as `count <= size_ - pos_`, which
	// cannot overflow (pos_ <= size_ is an invariant of every mutator). Walking the prefixes is
	// what exercises that boundary — under ASan/UBSan an out-of-range read here is a finding,
	// not a silent pass. Truncated input stays LENIENT by design (io::ByteReader's
	// format-parser contract: a clipped read yields 0 and does not advance), so what is pinned
	// is "handled without reading out of bounds", not "rejected". ---
	{
		for (size_t take = 0; take < original.size(); take += 97) {
			opennova::bms::File truncated;
			std::string err;
			bool threw = false;
			try {
				(void)opennova::bms::parse(original.data(), take, truncated, err);
			} catch (...) {
				threw = true;
			}
			TEST_EXPECT(!threw);
		}
	}

	// --- Regression (review #3): an empty-name loadout entry is REJECTED (set_weapon_loadout returns
	// false and leaves the loadout untouched), not silently dropped. The format serializes an empty name
	// as the chunk terminator, so a nameless weapon cannot be stored; silently skipping the row was itself
	// data loss (blanking a mid-list weapon's name deleted that weapon with no warning). ---
	{
		opennova::mission::MissionDocument doc;
		TEST_EXPECT(doc.load_bms_bytes(original.data(), original.size()));
		// Establish a known-good two-weapon loadout first.
		std::vector<opennova::mission::WeaponLoadoutEntry> good;
		good.push_back({"WPN_A", "-1", "-1", "1"});
		good.push_back({"WPN_B", "-1", "-1", "2"});
		TEST_EXPECT(doc.set_weapon_loadout(good));
		TEST_EXPECT(doc.weapon_loadout().size() == 2);
		// An edit that blanks a mid-list name is rejected; the loadout is left exactly as it was.
		std::vector<opennova::mission::WeaponLoadoutEntry> with_blank;
		with_blank.push_back({"WPN_A", "-1", "-1", "1"});
		with_blank.push_back({"", "-1", "-1", "0"}); // blanked name -> reject the whole edit, no silent drop
		with_blank.push_back({"WPN_B", "-1", "-1", "2"});
		TEST_EXPECT(!doc.set_weapon_loadout(with_blank));
		const std::vector<opennova::mission::WeaponLoadoutEntry> reread = doc.weapon_loadout();
		TEST_EXPECT(reread.size() == 2);
		TEST_EXPECT(reread[0].name == "WPN_A");
		TEST_EXPECT(reread[0].flags == "1");
		TEST_EXPECT(reread[1].name == "WPN_B");
		TEST_EXPECT(reread[1].flags == "2");
		// Deleting every weapon (an empty list) is still valid: the chunk goes to length 0.
		TEST_EXPECT(doc.set_weapon_loadout({}));
		TEST_EXPECT(doc.weapon_loadout().empty());
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

	// --- Regression (review): a waypoint marker_count > 32 (the 32-slot capacity) survives a
	// MissionDocument load/save. parse preserves a shipped over-count (CP19.bms ships 39) verbatim, but
	// sync_counts used to clobber it to the slot count on every load/save, breaking byte-exact round-trip.
	{
		opennova::mission::MissionDocument doc;
		doc.create_default();
		opennova::bms::File &f = doc.bms_file();
		TEST_EXPECT(!f.waypoint_records.empty());
		f.waypoint_records[0].waypoint_numbers.assign(opennova::mission::kMaxWaypointPathMarkers, 7u); // saturate the 32 slots
		f.waypoint_records[0].marker_count = 39;                                                       // the shipped over-count
		std::vector<uint8_t> bytes;
		TEST_EXPECT(doc.write_bms_bytes(bytes)); // runs sync_counts -> must preserve the saturated over-count
		opennova::bms::File reparsed;
		std::string err;
		TEST_EXPECT(opennova::bms::parse(bytes.data(), bytes.size(), reparsed, err));
		TEST_EXPECT(reparsed.waypoint_records[0].marker_count == 39);
		// But a count that drops below the cap is meaningless as an over-count, so it resyncs to the slots.
		doc.bms_file().waypoint_records[0].waypoint_numbers.assign(5, 7u);
		doc.bms_file().waypoint_records[0].marker_count = 39; // stale over-count, now only 5 slots
		std::vector<uint8_t> bytes2;
		TEST_EXPECT(doc.write_bms_bytes(bytes2));
		opennova::bms::File reparsed2;
		TEST_EXPECT(opennova::bms::parse(bytes2.data(), bytes2.size(), reparsed2, err));
		TEST_EXPECT(reparsed2.waypoint_records[0].marker_count == 5);
	}

	// --- Regression (review #1): an AUTHORED waypoint edit that keeps a path saturated at 32 markers must
	// resync a shipped over-count (39) down to 32, NOT preserve it. The pure round-trip above keeps 39 for
	// byte-exactness, but once the marker list is rewritten (reorder / flag-only / set_waypoint_path) the 39
	// no longer describes the data and the engine would walk 7 phantom waypoints. apply_waypoint_path_to_record
	// passes preserve_over_count=false so the count tracks the authored list. ---
	{
		opennova::mission::MissionDocument doc;
		doc.create_default();
		opennova::bms::File &f = doc.bms_file();
		TEST_EXPECT(!f.waypoint_records.empty());
		// 32 real markers so a full 32-index path validates.
		f.markers.assign(opennova::mission::kMaxWaypointPathMarkers, opennova::bms::Entity{});
		// Saturate path 0 at 32 slots and stamp the shipped over-count.
		f.waypoint_records[0].waypoint_numbers.assign(opennova::mission::kMaxWaypointPathMarkers, 0u);
		f.waypoint_records[0].marker_count = 39;
		// An authored edit re-applies a (still 32-marker) list, e.g. a flag-only change.
		std::vector<int> indices;
		for (int i = 0; i < static_cast<int>(opennova::mission::kMaxWaypointPathMarkers); ++i) {
			indices.push_back(i);
		}
		TEST_EXPECT(doc.set_waypoint_path(0, indices, 1, nullptr));
		// Resynced in memory immediately, and it stays resynced through save/reparse (no over-count revival).
		TEST_EXPECT(doc.bms_file().waypoint_records[0].marker_count == opennova::mission::kMaxWaypointPathMarkers);
		std::vector<uint8_t> wbytes;
		TEST_EXPECT(doc.write_bms_bytes(wbytes));
		opennova::bms::File wreparsed;
		std::string werr;
		TEST_EXPECT(opennova::bms::parse(wbytes.data(), wbytes.size(), wreparsed, werr));
		TEST_EXPECT(wreparsed.waypoint_records[0].marker_count == opennova::mission::kMaxWaypointPathMarkers);
	}

	// --- Regression (review): set_event / add_event clamp reset_after & delay to 0..1023 (their packed
	// 10-bit range) at the library boundary, so an out-of-range value can't wrap on serialize. ---
	{
		opennova::mission::MissionDocument doc;
		doc.create_default();
		opennova::mission::MissionEventRecord rec; // zero-initialized
		rec.delay = 2000;        // > 1023: would pack as (uint32)2000 << 22 (truncates) and reparse as 976
		rec.reset_after = 5000;  // > 1023
		opennova::mission::MissionEventRecord out;
		TEST_EXPECT(doc.add_event(rec, &out));
		TEST_EXPECT(out.delay == 1023);
		TEST_EXPECT(out.reset_after == 1023);
	}

	// --- Regression (review): a secondary-chunk length larger than the bytes remaining fails the parse
	// cleanly (count_fits guard) instead of silently zero-filling + mis-aligning every later section. ---
	{
		std::string err;
		std::vector<uint8_t> bytes = original;
		write_u16_le(bytes, offsetof(opennova::bms::Header, secondary_chunk_len), 0xFFFF);
		opennova::bms::File reparsed;
		TEST_EXPECT(!opennova::bms::parse(bytes.data(), bytes.size(), reparsed, err));
	}

	// --- Modeling policy: the loader sanitizes the loadout chunk into canonical four-string
	// records. Bytes after the empty-name terminator are ignored by the retail sanitizer and are
	// dropped by the canonical writer. ---
	{
		std::string err;
		std::vector<uint8_t> bytes = original;
		const size_t loadout_end = opennova::bms::kHeaderSize + original_loadout_len;
		const uint8_t trailing[] = {0xAB, 0xCD};
		bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(loadout_end), std::begin(trailing), std::end(trailing));
		write_u16_le(bytes, offsetof(opennova::bms::Header, weapon_loadout_chunk_len),
		             static_cast<uint16_t>(original_loadout_len + sizeof(trailing)));
		opennova::bms::File parsed_trailing;
		TEST_EXPECT(opennova::bms::parse(bytes.data(), bytes.size(), parsed_trailing, err));
		std::vector<uint8_t> canonical;
		TEST_EXPECT(opennova::bms::write(parsed_trailing, canonical, err));
		opennova::bms::File reparsed_trailing;
		TEST_EXPECT(opennova::bms::parse(canonical.data(), canonical.size(), reparsed_trailing, err));
		TEST_EXPECT(opennova::bms::equal(parsed_trailing, reparsed_trailing));
	}

	return 0;
}
