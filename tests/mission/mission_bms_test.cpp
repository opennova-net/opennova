#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/def/reserved_items.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_field.h>
#include <formats/mission/mission_mis.h>

#include "common/file_io.h"

namespace {

std::string fixture_path() {
	const std::string root = test_paths_repo_root(__FILE__);
	return root + "/fixtures/bms/synth_dense.bms";
}

using test_io::read_file;

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

// The document legs the retired facade carried: parse bytes into a synced
// file; sync + write a file to bytes.
bool load_document(const std::vector<uint8_t> &bytes, opennova::bms::File &out) {
	std::string error;
	if (!opennova::bms::parse(bytes.data(), bytes.size(), out, error)) return false;
	opennova::mission::sync_counts(out);
	return true;
}

bool write_document(opennova::bms::File &file, std::vector<uint8_t> &out) {
	std::string error;
	opennova::mission::sync_counts(file);
	return opennova::bms::write(file, out, error);
}

// Each event's runs as the records they hold: what an edit of another event's chain keeps.
std::vector<std::vector<int32_t>> chains_of(const opennova::bms::File &file) {
	std::vector<std::vector<int32_t>> out;
	for (const opennova::bms::Event &event : file.events) {
		std::vector<int32_t> chain;
		for (int i = 0; i < event.trigger_count; ++i) chain.push_back(file.triggers[size_t(event.trigger_index + i)].param1);
		chain.push_back(-999);
		for (int i = 0; i < event.action_count; ++i) chain.push_back(file.actions[size_t(event.action_index + i)].param2);
		out.push_back(chain);
	}
	return out;
}

// An event's chain edited in the middle of the file's tables (S13 D10's review: the chain edits keep
// every other event's runs on their records): a trigger put in mid-chain moves the later runs up one
// and taken out gives them back; an event removed mid-list takes its chain, and the later events' runs
// still hold their own records.
int chain_ranges() {
	using namespace opennova::mission;
	opennova::bms::File file;
	TEST_EXPECT(load_document(read_file(fixture_path()), file));
	// The event whose chain is edited: the first holding two triggers or more.
	size_t edited = 0;
	while (edited < file.events.size() && file.events[edited].trigger_count < 2) ++edited;
	TEST_EXPECT(file.events.size() >= 3 && edited < file.events.size());
	// Mark every trigger and action by its place, so a run is known by the records it holds (an action's
	// second parameter: a removal repairs a ResetEvent action's first).
	for (size_t i = 0; i < file.triggers.size(); ++i) file.triggers[i].param1 = int32_t(1000 + i);
	for (size_t i = 0; i < file.actions.size(); ++i) file.actions[i].param2 = int32_t(2000 + i);
	const std::vector<std::vector<int32_t>> before = chains_of(file);
	const std::vector<opennova::bms::Event> events = file.events;
	std::string error;
	MissionTriggerRecord added;
	added.param1 = 7777;
	TEST_EXPECT(insert_event_trigger(file, edited, 1, added, error));
	std::vector<std::vector<int32_t>> after = chains_of(file);
	TEST_EXPECT(after[edited].size() == before[edited].size() + 1 && after[edited][1] == 7777);
	size_t moved = 0;
	for (size_t e = 0; e < file.events.size(); ++e) {
		if (e == edited) continue;
		TEST_EXPECT(after[e] == before[e]);
		if (events[e].trigger_count && events[e].trigger_index > events[edited].trigger_index) {
			TEST_EXPECT(file.events[e].trigger_index == events[e].trigger_index + 1);
			++moved;
		}
	}
	TEST_EXPECT(remove_event_trigger(file, edited, 1, error) && chains_of(file) == before);
	for (size_t e = 0; e < file.events.size(); ++e) TEST_EXPECT(file.events[e].trigger_index == events[e].trigger_index);
	TEST_EXPECT(moved > 0); // a run after the edited one moved, and held its records
	// An event out of the middle: its chain goes with it, the rest keep theirs.
	TEST_EXPECT(remove_event(file, 1, error) && file.events.size() == events.size() - 1);
	after = chains_of(file);
	TEST_EXPECT(after[0] == before[0]);
	for (size_t e = 1; e < file.events.size(); ++e) TEST_EXPECT(after[e] == before[e + 1]);
	TEST_EXPECT(file.triggers.size() == size_t(file.trigger_count) && file.actions.size() == size_t(file.action_count));
	std::vector<uint8_t> bytes;
	opennova::bms::File back;
	TEST_EXPECT(write_document(file, bytes) && load_document(bytes, back) && chains_of(back) == after);
	return 0;
}

// The sections a written mission lays out, in the loader's order (bms::first_differing_section): the
// same bytes differ nowhere; a byte changed in a pool names the pool, one in the header the header,
// but not the count and chunk length words the writer derives again; bytes cut short name the section
// they end in, bytes added past the last the length. The player's route (the first PlayerRoute path,
// its stops capped at 128, a count the signed compare reads negative walking none) and an event's
// step timing (64 ticks a step, past 512 steps the countdown wraps).
int sections_and_route() {
	namespace bms = opennova::bms;
	const std::vector<uint8_t> original = read_file(fixture_path());
	bms::File file;
	std::string error;
	TEST_EXPECT(bms::parse(original.data(), original.size(), file, error));
	TEST_EXPECT(bms::first_differing_section(file, original, original).empty());
	std::vector<uint8_t> changed = original;
	changed[576] ^= 1;
	changed[583] ^= 1;
	TEST_EXPECT(bms::first_differing_section(file, original, changed).empty());
	changed[10] ^= 1;
	TEST_EXPECT(bms::first_differing_section(file, original, changed) == "header");
	// The first pool holding a record, past the header and the two chunks.
	size_t at = bms::kHeaderSize + read_u16_le(original, 578) + read_u16_le(original, 582);
	const std::pair<const char *, size_t> pools[] = {{"items", file.items.size()},
			{"buildings", file.buildings.size()}, {"markers", file.markers.size()},
			{"organics", file.organics.size()}};
	const char *pool = nullptr;
	for (const auto &[name, count] : pools) {
		if (count) {
			pool = name;
			break;
		}
		at += count * bms::kEntitySize;
	}
	TEST_EXPECT(pool != nullptr);
	changed = original;
	changed[at + 3] ^= 1;
	TEST_EXPECT(bms::first_differing_section(file, original, changed) == pool);
	changed = original;
	changed.resize(bms::kHeaderSize + 1);
	TEST_EXPECT(bms::first_differing_section(file, original, changed) ==
	            std::string(read_u16_le(original, 578) ? "loadout chunk" : read_u16_le(original, 582) ? "availability chunk" : pool));
	changed = original;
	changed.push_back(0);
	TEST_EXPECT(bms::first_differing_section(file, original, changed) == "length");

	bms::WaypointRecord route{};
	TEST_EXPECT(!bms::is_player_route(route));
	route.flags = bms::WaypointFlags::DoesNotLoop | bms::WaypointFlags::PlayerRoute;
	route.marker_count = 5;
	route.waypoint_numbers = {4, 1, 7};
	TEST_EXPECT(bms::is_player_route(route) && bms::player_route_stop_count(route) == 3);
	route.waypoint_numbers.assign(200, 0);
	TEST_EXPECT(bms::player_route_stop_count(route) == 5);
	route.marker_count = 1000;
	TEST_EXPECT(bms::player_route_stop_count(route) == bms::kPlayerRouteMaxStops && bms::kPlayerRouteMaxStops == 128);
	route.marker_count = 0x80000000u;
	TEST_EXPECT(bms::player_route_stop_count(route) == 0);

	TEST_EXPECT(bms::kEventStepTicks == 64 && (1 << bms::kEventStepShift) == bms::kEventStepTicks);
	TEST_EXPECT(bms::event_steps_wrap(513) && !bms::event_steps_wrap(512) && !bms::event_steps_wrap(0));
	// The first step count that wraps is the first whose reload reads negative as a signed 16-bit word
	// after one decrement.
	TEST_EXPECT(int16_t(uint16_t((bms::kEventStepsUnwrapped + 1) << bms::kEventStepShift) - bms::kEventStepTicks) < 0 &&
	            int16_t(uint16_t(bms::kEventStepsUnwrapped << bms::kEventStepShift) - bms::kEventStepTicks) > 0);
	return 0;
}

} // namespace

int main() {
	using namespace opennova::mission;
	std::string error;

	const std::vector<uint8_t> original = read_file(fixture_path());
	TEST_EXPECT(!original.empty());
	TEST_EXPECT(opennova::bms::is_bms(original.data(), original.size()));

	opennova::bms::File bms_file;
	TEST_EXPECT(opennova::bms::parse(original.data(), original.size(), bms_file, error));
	TEST_EXPECT(bms_file.items.size() == bms_file.header.num_items);
	TEST_EXPECT(bms_file.buildings.size() == bms_file.header.num_buildings);
	TEST_EXPECT(bms_file.markers.size() == bms_file.header.num_markers);
	TEST_EXPECT(bms_file.organics.size() == bms_file.header.num_people);
	TEST_EXPECT(bms_file.waypoint_records.size() == opennova::bms::kWaypointRecordCount);
	TEST_EXPECT(bms_file.group_records.size() == opennova::bms::kGroupRecordCount);
	TEST_EXPECT(bms_file.layer_records.size() == opennova::bms::kLayerRecordCount);

	// A Mission box's name over its two words, the second its last four characters: written and read back as
	// it stands, as the game loads a box's 36 bytes whole and reads the word with ref_id [orig:
	// Mission_LoadBMSFile @0x40fcdc; Entity_UpdateInfantryPlayerBody @0x4b60b6..0x4b60c4].
	{
		opennova::bms::File boxed = bms_file;
		opennova::bms::BoundingBox box{};
		box.type = int32_t(opennova::bms::BoundingBoxType::Mission);
		const MissionField *name = find_mission_field(MissionRecord::BoundingBox, "mission");
		TEST_EXPECT(name && name->set(&box, std::string("CP19NEXT"), error) && box.reserved0 != 0);
		boxed.bounding_boxes.push_back(box);
		sync_counts(boxed);
		std::vector<uint8_t> bytes;
		opennova::bms::File back;
		MissionValue read;
		TEST_EXPECT(opennova::bms::write(boxed, bytes, error) && opennova::bms::parse(bytes.data(), bytes.size(), back, error));
		TEST_EXPECT(!back.bounding_boxes.empty() && back.bounding_boxes.back().reserved0 == box.reserved0 &&
		            name && name->get(&back.bounding_boxes.back(), read) && std::get<std::string>(read) == "CP19NEXT");
	}

	std::vector<uint8_t> encoded;
	TEST_EXPECT(opennova::bms::write(bms_file, encoded, error));
	opennova::bms::File encoded_file;
	TEST_EXPECT(opennova::bms::parse(encoded.data(), encoded.size(), encoded_file, error));
	TEST_EXPECT(opennova::bms::equal(bms_file, encoded_file));
	std::vector<uint8_t> encoded2;
	TEST_EXPECT(opennova::bms::write(encoded_file, encoded2, error));
	TEST_EXPECT(encoded2 == encoded);

	opennova::bms::File document;
	TEST_EXPECT(load_document(original, document));
	TEST_EXPECT(!mission_info(document).mission_name.empty());
	const uint16_t original_loadout_len = read_u16_le(original, offsetof(opennova::bms::Header, weapon_loadout_chunk_len));
	const uint16_t original_secondary_len = read_u16_le(original, offsetof(opennova::bms::Header, secondary_chunk_len));

	const size_t original_item_count = entity_count(document, EntityKind::Item);
	TEST_EXPECT(original_item_count > 0);

	const int first_item_bms_id = document.items[0].id;
	const int first_item_item_id = entity_item_id(document.items[0]);
	EntityTransform edited = entity_transform(document.items[0]);
	edited.x += 12.5f;
	edited.y -= 3.0f;
	edited.z += 8.25f;
	edited.pitch += 1;
	edited.yaw += 2;
	edited.roll += 3;
	TEST_EXPECT(set_entity_transform(document, EntityKind::Item, 0, edited, error));

	const EntityTransform reread = entity_transform(document.items[0]);
	TEST_EXPECT(reread.x == edited.x);
	TEST_EXPECT(reread.y == edited.y);
	TEST_EXPECT(reread.z == edited.z);
	TEST_EXPECT(reread.pitch == edited.pitch);
	TEST_EXPECT(reread.yaw == edited.yaw);
	TEST_EXPECT(reread.roll == edited.roll);

	// The editable properties, one named field at a time (the same clamp rules
	// the retired bulk setter applied).
	const int ai_flags = static_cast<int>(opennova::bms::BmsiAttributeFlags::Blind) |
                         static_cast<int>(opennova::bms::BmsiAttributeFlags::MultiplayerOnly) |
	                     static_cast<int>(opennova::bms::BmsiAttributeFlags::NoShadow);
	TEST_EXPECT(set_entity_property_int(document, EntityKind::Item, 0, "group", 7, error));
	TEST_EXPECT(set_entity_property_int(document, EntityKind::Item, 0, "waypoint_id", 3, error));
	TEST_EXPECT(set_entity_property_int(document, EntityKind::Item, 0, "wp_number", 12, error));
	TEST_EXPECT(set_entity_property_int(document, EntityKind::Item, 0, "team", 2, error));
	TEST_EXPECT(set_entity_property_int(document, EntityKind::Item, 0, "ai_flags", ai_flags, error));
	TEST_EXPECT(set_entity_property_int(document, EntityKind::Item, 0, "perception", 88, error));
	TEST_EXPECT(set_entity_property_int(document, EntityKind::Item, 0, "accuracy", 66, error));
	TEST_EXPECT(set_entity_property_int(document, EntityKind::Item, 0, "alert_state", 4, error));
	TEST_EXPECT(set_entity_property_int(document, EntityKind::Item, 0, "min_engagement_distance", 30, error));
	TEST_EXPECT(set_entity_property_int(document, EntityKind::Item, 0, "max_engagement_distance", 333, error));
	TEST_EXPECT(set_entity_property_int(document, EntityKind::Item, 0, "max_attack_distance", 444, error));
	TEST_EXPECT(set_entity_property_int(document, EntityKind::Item, 0, "spawn_count", 5, error));
	TEST_EXPECT(set_entity_property_int(document, EntityKind::Item, 0, "max_simultaneous", 2, error));
	{
		const opennova::bms::Entity &property_updated = document.items[0];
		TEST_EXPECT(property_updated.id == first_item_bms_id);
		TEST_EXPECT(entity_item_id(property_updated) == first_item_item_id);
		const EntityTransform kept = entity_transform(property_updated);
		TEST_EXPECT(kept.x == edited.x);
		TEST_EXPECT(kept.y == edited.y);
		TEST_EXPECT(kept.z == edited.z);
		TEST_EXPECT(property_updated.group_id == 7);
		TEST_EXPECT(property_updated.waypoint_id == 3);
		TEST_EXPECT(property_updated.wp_number == 12);
		TEST_EXPECT(property_updated.team == 2);
		TEST_EXPECT(property_updated.bmsi_attributes == static_cast<uint32_t>(ai_flags));
		TEST_EXPECT(property_updated.perception2 == 88);
		TEST_EXPECT(property_updated.w_accuracy1 == 66);
		TEST_EXPECT(property_updated.alert_state == 4);
		TEST_EXPECT(property_updated.min_engagement_distance == 30);
		TEST_EXPECT(property_updated.max_engagement_distance == 333);
		TEST_EXPECT(property_updated.max_attack_distance == 444);
		TEST_EXPECT(property_updated.spawns == 5);
		TEST_EXPECT(property_updated.no_more_than == 2);
	}

	EntityTransform placed;
	placed.x = 1.0f;
	placed.y = 2.0f;
	placed.z = 3.0f;
	placed.yaw = 90;
	const size_t added = add_entity(document, EntityKind::Item, 101291, placed);
	TEST_EXPECT(entity_count(document, EntityKind::Item) == original_item_count + 1);
	TEST_EXPECT(entity_item_id(document.items[added]) == 101291);
	TEST_EXPECT(document.items[added].type_id == 1291);
	// A new record holds what the original editor's initializer gives a placed item (new_entity, D-MIS-10;
	// mission_corpus's retail leg holds each to the corpus), its SSN one past the file's largest.
	{
		const opennova::bms::Entity &made = document.items[added];
		int largest = 0;
		for (EntityKind kind : {EntityKind::Item, EntityKind::Building, EntityKind::Marker, EntityKind::Organic})
			for (const opennova::bms::Entity &other : *entities(document, kind))
				if (&other != &made) largest = std::max(largest, other.id);
		TEST_EXPECT(made.id == largest + 1 && next_entity_ssn(document) == made.id + 1);
		TEST_EXPECT(made.wp_distance == 10 && made.perception2 == 100 && made.perfectionist2 == 100);
		TEST_EXPECT(made.min_engagement_distance == 16 && made.max_engagement_distance == 320 &&
		            made.max_attack_distance == 16);
		TEST_EXPECT(made.w_accuracy1 == 100 && made.w_accuracy2 == 100 && made.spawns == 0 && made.no_more_than == 0);
		TEST_EXPECT(made.crouch_timer == 3 && made.unk15a == 0 && made.shoot_timer == 5 && made.wp_adv_trigger == -1);
		TEST_EXPECT(made.attention == 30 && made.obliqueness == 15 && made.advancetimer == 10 && made.map_symbol == 0);
		TEST_EXPECT(std::string(made.gen_string) == "null" && made.name1[0] == 0 && made.name2[0] == 0);
		TEST_EXPECT(made.mis_height_lock == 1 && made.get_x() == 1.0f && made.yaw == 90);
		const opennova::bms::Entity marker = new_entity(EntityKind::Marker, 100001, 77);
		TEST_EXPECT(marker.type == opennova::bms::ItemType::Marker && marker.id == 77 && marker.type_id == 1 &&
		            marker.x == 0 && marker.attention == 30);
	}
	// A new entity never takes a player's net id: past a largest of 9999 the next is 10256, past the 256
	// slots' 10000..10255.
	{
		opennova::bms::File below_player;
		make_default(below_player);
		below_player.organics.push_back(new_entity(EntityKind::Organic, 100001, 9999));
		sync_counts(below_player);
		TEST_EXPECT(next_entity_ssn(below_player) == 10256);
		const size_t at = add_entity(below_player, EntityKind::Item, 101291, placed);
		TEST_EXPECT(below_player.items[at].id == 10256 && next_entity_ssn(below_player) == 10257);
	}
	// A blank mission: named, on its terrain and under its environment, the header values the shipped
	// missions hold in common, no record of any pool; a name past its slot is refused, the file as it was.
	{
		opennova::bms::File blank;
		std::string blank_error;
		TEST_EXPECT(make_blank(blank, {"New mission", "A designer", "Tmap", "synth_full"}, blank_error));
		const MissionInfo info = mission_info(blank);
		TEST_EXPECT(info.mission_name == "New mission" && info.designer == "A designer" && info.terrain == "Tmap" &&
		            info.environment == "synth_full" && info.tile_set.empty());
		TEST_EXPECT(std::string(blank.header.default_str) == "Default" &&
		            blank.header.mission_type == opennova::bms::MissionType::NormalMission);
		TEST_EXPECT(blank.header.bonus_expiration == 10 && blank.header.max_saves == 3 && blank.header.map_zoom == 0.5f &&
		            blank.header.minutes_per_day == 1440 && blank.header.start_time == 3840);
		for (int i = 0; i < 8; ++i)
			TEST_EXPECT(blank.header.win_conditions[i] == 255 && blank.header.lose_conditions[i] == 255);
		TEST_EXPECT(blank.items.empty() && blank.buildings.empty() && blank.markers.empty() && blank.organics.empty() &&
		            blank.area_triggers.empty() && blank.events.empty() && blank.loadout.entries.empty());
		std::vector<uint8_t> blank_bytes;
		opennova::bms::File blank_back;
		TEST_EXPECT(write_document(blank, blank_bytes) && load_document(blank_bytes, blank_back) &&
		            opennova::bms::equal(blank, blank_back));
		const opennova::bms::File kept = blank;
		TEST_EXPECT(!make_blank(blank, {"New mission", "", std::string(17, 't'), "synth_full"}, blank_error) &&
		            !blank_error.empty() && opennova::bms::equal(blank, kept));
		TEST_EXPECT(!make_blank(blank, {std::string(33, 'n'), "", "Tmap", "synth_full"}, blank_error) &&
		            opennova::bms::equal(blank, kept));
	}

	std::vector<uint8_t> edited_bytes;
	TEST_EXPECT(write_document(document, edited_bytes));
	opennova::bms::File reparsed;
	TEST_EXPECT(load_document(edited_bytes, reparsed));
	TEST_EXPECT(entity_count(reparsed, EntityKind::Item) == original_item_count + 1);
	{
		const opennova::bms::Entity &reparsed_first_item = reparsed.items[0];
		TEST_EXPECT(reparsed_first_item.group_id == 7);
		TEST_EXPECT(reparsed_first_item.waypoint_id == 3);
		TEST_EXPECT(reparsed_first_item.wp_number == 12);
		TEST_EXPECT(reparsed_first_item.team == 2);
		TEST_EXPECT(reparsed_first_item.bmsi_attributes == static_cast<uint32_t>(ai_flags));
		TEST_EXPECT(reparsed_first_item.perception2 == 88);
		TEST_EXPECT(reparsed_first_item.w_accuracy1 == 66);
		TEST_EXPECT(reparsed_first_item.alert_state == 4);
		TEST_EXPECT(reparsed_first_item.min_engagement_distance == 30);
		TEST_EXPECT(reparsed_first_item.max_engagement_distance == 333);
		TEST_EXPECT(reparsed_first_item.max_attack_distance == 444);
		TEST_EXPECT(reparsed_first_item.spawns == 5);
		TEST_EXPECT(reparsed_first_item.no_more_than == 2);
	}
	const std::vector<WaypointSummary> summaries = waypoint_summaries(reparsed);
	TEST_EXPECT(summaries.size() == opennova::bms::kWaypointRecordCount);
	TEST_EXPECT(std::any_of(summaries.begin(), summaries.end(), [](const WaypointSummary &summary) {
		return summary.marker_count > 0;
	}));
	TEST_EXPECT(reparsed.waypoint_records.size() == opennova::bms::kWaypointRecordCount);
	WaypointPath path_zero;
	TEST_EXPECT(waypoint_path(reparsed, 0, path_zero));
	TEST_EXPECT(path_zero.index == 0);

	const size_t original_marker_count = entity_count(reparsed, EntityKind::Marker);
	TEST_EXPECT(clear_waypoint_path(reparsed, 1, error));
	WaypointPath cleared_path;
	TEST_EXPECT(waypoint_path(reparsed, 1, cleared_path));
	TEST_EXPECT(cleared_path.index == 1);
	TEST_EXPECT(cleared_path.marker_indices.empty());

	EntityTransform waypoint_transform;
	waypoint_transform.x = 40.0f;
	waypoint_transform.y = 41.0f;
	waypoint_transform.z = 42.0f;
	size_t first_marker_index = 0;
	WaypointPath edited_path;
	const int waypoint_item = opennova::mission::kItemIdOffset + opennova::def::DEF_TYPE_WAYPOINT;
	TEST_EXPECT(add_waypoint_marker(reparsed, 1, waypoint_item, waypoint_transform, -1, error, &first_marker_index));
	TEST_EXPECT(waypoint_path(reparsed, 1, edited_path));
	TEST_EXPECT(entity_count(reparsed, EntityKind::Marker) == original_marker_count + 1);
	TEST_EXPECT(reparsed.markers[first_marker_index].type == opennova::bms::ItemType::Marker);
	TEST_EXPECT(edited_path.marker_indices.size() == 1);
	TEST_EXPECT(edited_path.marker_indices[0] == static_cast<int>(first_marker_index));

	const int waypoint_flags = static_cast<int>(opennova::bms::WaypointFlags::DoesNotLoop) |
	                           static_cast<int>(opennova::bms::WaypointFlags::PlayerRoute);
	std::vector<int> marker_order = {static_cast<int>(first_marker_index)};
	TEST_EXPECT(set_waypoint_path(reparsed, 1, marker_order, waypoint_flags, error));
	TEST_EXPECT(waypoint_path(reparsed, 1, edited_path));
	TEST_EXPECT(edited_path.flags == waypoint_flags);
	TEST_EXPECT(edited_path.marker_indices == marker_order);

	EntityTransform inserted_transform;
	inserted_transform.x = 50.0f;
	inserted_transform.y = 51.0f;
	inserted_transform.z = 52.0f;
	size_t inserted_marker_index = 0;
	TEST_EXPECT(add_waypoint_marker(reparsed, 1, waypoint_item, inserted_transform, 0, error, &inserted_marker_index));
	TEST_EXPECT(waypoint_path(reparsed, 1, edited_path));
	TEST_EXPECT(entity_count(reparsed, EntityKind::Marker) == original_marker_count + 2);
	TEST_EXPECT(edited_path.marker_indices.size() == 2);
	TEST_EXPECT(edited_path.marker_indices[0] == static_cast<int>(inserted_marker_index));
	TEST_EXPECT(edited_path.marker_indices[1] == static_cast<int>(first_marker_index));

	TEST_EXPECT(remove_entity(reparsed, EntityKind::Marker, first_marker_index, error));
	WaypointPath repaired_path;
	TEST_EXPECT(waypoint_path(reparsed, 1, repaired_path));
	TEST_EXPECT(repaired_path.marker_indices.size() == 1);
	TEST_EXPECT(repaired_path.marker_indices[0] == static_cast<int>(inserted_marker_index - 1));

	std::vector<uint8_t> waypoint_bytes;
	TEST_EXPECT(write_document(reparsed, waypoint_bytes));
	opennova::bms::File waypoint_roundtrip;
	TEST_EXPECT(load_document(waypoint_bytes, waypoint_roundtrip));
	WaypointPath roundtrip_path;
	TEST_EXPECT(waypoint_path(waypoint_roundtrip, 1, roundtrip_path));
	TEST_EXPECT(roundtrip_path.flags == waypoint_flags);
	TEST_EXPECT(roundtrip_path.marker_indices.size() == 1);
	TEST_EXPECT(roundtrip_path.marker_indices[0] == static_cast<int>(inserted_marker_index - 1));

	TEST_EXPECT(waypoint_roundtrip.area_triggers.size() == bms_file.area_triggers.size());
	TEST_EXPECT(waypoint_roundtrip.events.size() == bms_file.events.size());
	TEST_EXPECT(waypoint_roundtrip.triggers.size() == bms_file.triggers.size());
	TEST_EXPECT(waypoint_roundtrip.actions.size() == bms_file.actions.size());
	TEST_EXPECT(!waypoint_roundtrip.events.empty());
	TEST_EXPECT(!waypoint_roundtrip.triggers.empty());
	TEST_EXPECT(!waypoint_roundtrip.actions.empty());
	const MissionLogicSummary summary = logic_summary(waypoint_roundtrip);
	TEST_EXPECT(summary.event_count == waypoint_roundtrip.events.size());
	TEST_EXPECT(summary.trigger_count == waypoint_roundtrip.triggers.size());
	TEST_EXPECT(summary.action_count == waypoint_roundtrip.actions.size());
	TEST_EXPECT(summary.area_trigger_count == waypoint_roundtrip.area_triggers.size());

	MissionEventRecord first_event;
	TEST_EXPECT(event(waypoint_roundtrip, 0, first_event));
	TEST_EXPECT(first_event.index == 0);
	MissionEventChain first_chain;
	TEST_EXPECT(event_chain(waypoint_roundtrip, 0, first_chain));
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
	MissionEventChain mutable_chain;
	bool found_mutable_chain = false;
	for (size_t i = 0; i < waypoint_roundtrip.events.size(); ++i) {
		if (event_chain(waypoint_roundtrip, i, mutable_chain) &&
		    mutable_chain.event.trigger_count < 20 &&
		    mutable_chain.event.action_count < 20) {
			mutable_event_index = i;
			found_mutable_chain = true;
			break;
		}
	}
	TEST_EXPECT(found_mutable_chain);
	MissionEventRecord edited_event = mutable_chain.event;
	edited_event.flags |= static_cast<int>(opennova::bms::EventFlags::PreMission);
	edited_event.delay += 7;
	TEST_EXPECT(set_event(waypoint_roundtrip, mutable_event_index, edited_event, error));
	MissionEventRecord reread_event;
	TEST_EXPECT(event(waypoint_roundtrip, mutable_event_index, reread_event));
	TEST_EXPECT((reread_event.flags & static_cast<int>(opennova::bms::EventFlags::PreMission)) != 0);
	TEST_EXPECT(reread_event.delay == edited_event.delay);

	const size_t before_trigger_count = waypoint_roundtrip.triggers.size();
	MissionTriggerRecord new_trigger;
	new_trigger.condition_flags = 0;
	new_trigger.main_type = static_cast<int>(opennova::bms::TriggerMainType::MissionVariable);
	new_trigger.sub_type = static_cast<int>(opennova::bms::MissionVariableTriggerType::MissionVariableIsGreaterThan);
	new_trigger.param1 = 3;
	new_trigger.param2 = 9;
	const size_t trigger_insert_index = mutable_chain.triggers.size();
	TEST_EXPECT(insert_event_trigger(waypoint_roundtrip, mutable_event_index, trigger_insert_index, new_trigger, error));
	MissionEventChain trigger_insert_chain;
	TEST_EXPECT(event_chain(waypoint_roundtrip, mutable_event_index, trigger_insert_chain));
	TEST_EXPECT(waypoint_roundtrip.triggers.size() == before_trigger_count + 1);
	TEST_EXPECT(trigger_insert_chain.triggers.size() == mutable_chain.triggers.size() + 1);
	MissionTriggerRecord edited_trigger = trigger_insert_chain.triggers.back();
	edited_trigger.param2 = 11;
	TEST_EXPECT(set_trigger(waypoint_roundtrip, edited_trigger.index, edited_trigger, error));
	MissionTriggerRecord reread_trigger;
	TEST_EXPECT(trigger(waypoint_roundtrip, edited_trigger.index, reread_trigger));
	TEST_EXPECT(reread_trigger.param2 == 11);
	TEST_EXPECT(remove_event_trigger(waypoint_roundtrip, mutable_event_index, trigger_insert_index, error));
	TEST_EXPECT(event_chain(waypoint_roundtrip, mutable_event_index, mutable_chain));
	TEST_EXPECT(waypoint_roundtrip.triggers.size() == before_trigger_count);

	const size_t before_action_count = waypoint_roundtrip.actions.size();
	MissionActionRecord new_action;
	new_action.action_type = static_cast<int>(opennova::bms::ActionType::ResetEvent);
	new_action.param1 = static_cast<int>(mutable_event_index);
	const size_t action_insert_index = mutable_chain.actions.size();
	TEST_EXPECT(insert_event_action(waypoint_roundtrip, mutable_event_index, action_insert_index, new_action, error));
	MissionEventChain action_insert_chain;
	TEST_EXPECT(event_chain(waypoint_roundtrip, mutable_event_index, action_insert_chain));
	TEST_EXPECT(waypoint_roundtrip.actions.size() == before_action_count + 1);
	TEST_EXPECT(action_insert_chain.actions.size() == mutable_chain.actions.size() + 1);
	MissionActionRecord edited_action = action_insert_chain.actions.back();
	edited_action.param1 = 0;
	TEST_EXPECT(set_action(waypoint_roundtrip, edited_action.index, edited_action, error));
	MissionActionRecord reread_action;
	TEST_EXPECT(action(waypoint_roundtrip, edited_action.index, reread_action));
	TEST_EXPECT(reread_action.param1 == 0);
	TEST_EXPECT(remove_event_action(waypoint_roundtrip, mutable_event_index, action_insert_index, error));
	TEST_EXPECT(event_chain(waypoint_roundtrip, mutable_event_index, mutable_chain));
	TEST_EXPECT(waypoint_roundtrip.actions.size() == before_action_count);

	opennova::bms::Event invalid_event = {};
	invalid_event.trigger_index = static_cast<int32_t>(waypoint_roundtrip.triggers.size() + 10);
	invalid_event.trigger_count = 1;
	waypoint_roundtrip.events.push_back(invalid_event);
	sync_counts(waypoint_roundtrip);
	MissionEventChain invalid_chain;
	TEST_EXPECT(event_chain(waypoint_roundtrip, waypoint_roundtrip.events.size() - 1, invalid_chain));
	TEST_EXPECT(!invalid_chain.diagnostics.empty());

	TEST_EXPECT(remove_entity(reparsed, EntityKind::Item, original_item_count, error));
	TEST_EXPECT(entity_count(reparsed, EntityKind::Item) == original_item_count);

	std::string mis_text;
	sync_counts(reparsed);
	TEST_EXPECT(write_mis_text(reparsed, mis_text, error));
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
		const size_t entity_total = bms_file.header.num_items + bms_file.header.num_buildings +
		                            bms_file.header.num_markers + bms_file.header.num_people;
		const size_t group_offset = opennova::bms::kHeaderSize +
		                            original_loadout_len +
		                            original_secondary_len +
		                            entity_total * opennova::bms::kEntitySize +
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
		const size_t entity_total = bms_file.header.num_items + bms_file.header.num_buildings +
		                            bms_file.header.num_markers + bms_file.header.num_people;
		const size_t event_block = opennova::bms::kHeaderSize +
		                           original_loadout_len +
		                           original_secondary_len +
		                           entity_total * opennova::bms::kEntitySize +
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
		opennova::bms::File hdr;
		TEST_EXPECT(load_document(original, hdr));
		TEST_EXPECT(set_header_string(hdr, "mission_name", "Grill Test", error));
		TEST_EXPECT(set_header_int(hdr, "climate", 2, error));
		TEST_EXPECT(set_header_int(hdr, "minutes_per_day", 1234, error));
		const int coop_bit = 0x1000000; // ATTRIB_COOP
		set_header_flag(hdr, coop_bit, true);
		std::string unknown_error;
		TEST_EXPECT(!set_header_string(hdr, "nonexistent_field", "x", unknown_error)); // unknown rejected
		TEST_EXPECT(!unknown_error.empty());
		std::vector<uint8_t> hdr_bytes;
		TEST_EXPECT(write_document(hdr, hdr_bytes));
		opennova::bms::File hdr_reload;
		TEST_EXPECT(load_document(hdr_bytes, hdr_reload));
		const MissionInfo reread_info = mission_info(hdr_reload);
		TEST_EXPECT(reread_info.mission_name == "Grill Test");
		TEST_EXPECT(reread_info.climate == 2);
		TEST_EXPECT(reread_info.minutes_per_day == 1234);
		TEST_EXPECT((reread_info.attrib_flags & coop_bit) != 0);
	}

	// --- Regression (review): the terrain slot is a fixed 16-byte field (first of the three
	// 16-byte slots in header.terrain[48]: terrain / cnv_file / tt_file). The old copy used to force a
	// NUL into byte 15 and truncate a full 16-char terrain name on every edit; copy_fixed_field
	// keeps all 16, and the mission_info()/get_terrain reads are bounded to 16 so a full slot does not
	// bleed into cnv_file. Editing terrain must also leave cnv_file / tt_file untouched. ---
	{
		opennova::bms::File doc;
		TEST_EXPECT(load_document(original, doc));
		// Seed cnv_file (@+16) and tt_file (@+32) so we can prove a terrain edit preserves them.
		char *terrain_region = doc.header.terrain;
		std::memcpy(terrain_region + 16, "convert.cnv", 11);
		terrain_region[16 + 11] = '\0';
		std::memcpy(terrain_region + 32, "tiles.tt", 8);
		terrain_region[32 + 8] = '\0';

		const std::string full16 = "sixteen_char_ter"; // exactly 16 chars, fills the slot
		TEST_EXPECT(full16.size() == 16);
		TEST_EXPECT(set_header_string(doc, "terrain", full16, error));

		std::vector<uint8_t> bytes;
		TEST_EXPECT(write_document(doc, bytes));
		opennova::bms::File reload;
		TEST_EXPECT(load_document(bytes, reload));
		// All 16 chars survive and do not run on into cnv_file.
		TEST_EXPECT(mission_info(reload).terrain == full16);
		TEST_EXPECT(reload.get_terrain() == full16);
		// cnv_file / tt_file are untouched by the terrain edit.
		const char *reload_region = reload.header.terrain;
		TEST_EXPECT(std::string(reload_region + 16) == "convert.cnv");
		TEST_EXPECT(std::string(reload_region + 32) == "tiles.tt");

		// A name longer than 16 is cut to the slot; a shorter name still round-trips.
		TEST_EXPECT(set_header_string(doc, "terrain", "way_too_long_terrain_name", error));
		TEST_EXPECT(mission_info(doc).terrain == "way_too_long_ter"); // 16 chars
		TEST_EXPECT(set_header_string(doc, "terrain", "short", error));
		TEST_EXPECT(mission_info(doc).terrain == "short");
	}

	// --- Wire S2C 0x0B header: retail's g_BmsHeaderBlock has its first 4 bytes
	// ZEROED (not "BMS"+version), and the joiner memcpy's it verbatim + reads by
	// offset with no magic/version gate. parse_header_blob must accept that and
	// still recover terrain @+0x44 / env @+0xDC / tile-set @+0x118. Witnessed live
	// 2026-08-31: a genuine .204 host's 0x0B for "ndakotabasestormy.npz.bms" began
	// 00 00 00 00. [orig: NapiNPClientMsg_HandleBMSHeader @0x422660 vs the file
	// gate Mission_LoadBMSFile @0x40f5aa]. ---
	{
		std::vector<uint8_t> blob(opennova::bms::kHeaderSize, 0);
		std::memcpy(blob.data() + 0x04, "North Dakota Stormy", 19);
		std::memcpy(blob.data() + 0x44, "G11.trn", 7);
		std::memcpy(blob.data() + 0xDC, "FULL_07.env", 11);
		std::memcpy(blob.data() + 0x118, "TRNTILEA1.TGA", 13);
		opennova::bms::Header h;
		std::string err;
		TEST_EXPECT(opennova::bms::parse_header_blob(blob.data(), blob.size(), h, err));
		TEST_EXPECT(std::string(h.mission_name) == "North Dakota Stormy");
		TEST_EXPECT(std::string(h.terrain) == "G11.trn");
		TEST_EXPECT(std::string(h.environment) == "FULL_07.env");
		TEST_EXPECT(std::string(h.terrain_tile) == "TRNTILEA1.TGA");
		// Our own host's "BMS"+version form still parses through the same path.
		blob[0] = 'B'; blob[1] = 'M'; blob[2] = 'S'; blob[3] = 25;
		opennova::bms::Header h2;
		std::string err2;
		TEST_EXPECT(opennova::bms::parse_header_blob(blob.data(), blob.size(), h2, err2));
		TEST_EXPECT(std::string(h2.terrain) == "G11.trn");
		// The typed view hands consumers the BASENAME either way: the wire
		// header's extension is dropped, so "G11.trn" and a file's "G11"
		// resolve to the one on-disk name when the consumer appends ".trn".
		blob[0] = 0; blob[1] = 0; blob[2] = 0; blob[3] = 0;
		opennova::bms::File wire_doc;
		TEST_EXPECT(opennova::bms::parse_header_blob(blob.data(), blob.size(), wire_doc.header, err));
		TEST_EXPECT(mission_info(wire_doc).terrain == "G11");
		TEST_EXPECT(mission_info(wire_doc).environment == "FULL_07");
		// The tile-set name keeps its authored extension; the terrain loader
		// replaces it (formats/trn trn_mission_tilestrip).
		TEST_EXPECT(mission_info(wire_doc).tile_set == "TRNTILEA1.TGA");
	}

	// --- Phase 1: hidden entity fields (name1/name2/no_less_than/map_symbol) round-trip,
	// and the names use the format's full 8-byte slot (a name longer than 8 is cut to 8). ---
	{
		opennova::bms::File doc;
		TEST_EXPECT(load_document(original, doc));
		const uint8_t max_simultaneous_before = doc.items[0].no_more_than;
		TEST_EXPECT(set_entity_property_int(doc, EntityKind::Item, 0, "no_less_than", 9, error));
		TEST_EXPECT(set_entity_property_int(doc, EntityKind::Item, 0, "map_symbol", 17, error));
		TEST_EXPECT(set_entity_property_string(doc, EntityKind::Item, 0, "name1", "rifle", error));
		TEST_EXPECT(set_entity_property_string(doc, EntityKind::Item, 0, "name2", "patrol", error));
		std::vector<uint8_t> bytes;
		TEST_EXPECT(write_document(doc, bytes));
		opennova::bms::File reload;
		TEST_EXPECT(load_document(bytes, reload));
		const opennova::bms::Entity &rec2 = reload.items[0];
		TEST_EXPECT(rec2.no_less_than == 9);
		TEST_EXPECT(rec2.map_symbol == 17);
		TEST_EXPECT(entity_name1(rec2) == "rifle");
		TEST_EXPECT(entity_name2(rec2) == "patrol");
		TEST_EXPECT(rec2.no_more_than == max_simultaneous_before); // no_more_than preserved
		// An exactly-8-char name keeps all 8 bytes: the old copy used to force a NUL into byte 7 and
		// drop the 8th char, silently corrupting an unedited AI class name on every property edit.
		TEST_EXPECT(set_entity_property_string(doc, EntityKind::Item, 0, "name1", "rifleman", error)); // 8 chars
		TEST_EXPECT(entity_name1(doc.items[0]) == "rifleman");
		TEST_EXPECT(set_entity_property_string(doc, EntityKind::Item, 0, "name1", "verylongname", error)); // > 8 chars -> cut to the 8-byte slot
		TEST_EXPECT(entity_name1(doc.items[0]) == "verylong");
		TEST_EXPECT(entity_name1(doc.items[0]).size() == 8);
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
		opennova::bms::File gen_reparsed;
		TEST_EXPECT(opennova::bms::parse(bytes.data(), bytes.size(), gen_reparsed, err));
		TEST_EXPECT(std::string(gen_reparsed.items[0].gen_string) == "generator");
		TEST_EXPECT(gen_reparsed.items[0].grenades == 4);
		TEST_EXPECT(gen_reparsed.items[0].mission_critical == 1);
		TEST_EXPECT(gen_reparsed.items[0].lfp_group == 7);

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
		opennova::bms::File doc;
		TEST_EXPECT(load_document(original, doc));
		const opennova::bms::Entity before = doc.items[0];
		// `group` is the one key whose member name differs (group_id).
		TEST_EXPECT(set_entity_property_int(doc, EntityKind::Item, 0, "group", 5, error));
		TEST_EXPECT(set_entity_property_int(doc, EntityKind::Item, 0, "map_symbol", 22, error));
		TEST_EXPECT(set_entity_property_string(doc, EntityKind::Item, 0, "name1", "scout", error));
		const opennova::bms::Entity &after = doc.items[0];
		TEST_EXPECT(after.group_id == 5);
		TEST_EXPECT(after.map_symbol == 22);
		TEST_EXPECT(entity_name1(after) == "scout");
		// Untouched fields are preserved (only the requested members changed).
		TEST_EXPECT(after.team == before.team);
		TEST_EXPECT(after.w_accuracy1 == before.w_accuracy1);
		TEST_EXPECT(entity_name2(after) == entity_name2(before));
		TEST_EXPECT(after.max_engagement_distance == before.max_engagement_distance);
		// Unknown names are rejected (not silently ignored), with the error set.
		std::string bogus_error;
		TEST_EXPECT(!set_entity_property_int(doc, EntityKind::Item, 0, "bogus_field", 1, bogus_error));
		TEST_EXPECT(!bogus_error.empty());
		TEST_EXPECT(!set_entity_property_string(doc, EntityKind::Item, 0, "name3", "x", bogus_error));
		// Out-of-range index is rejected.
		TEST_EXPECT(!set_entity_property_int(doc, EntityKind::Item, 99999, "team", 1, bogus_error));
		// AI flags outside the known attribute mask are rejected.
		TEST_EXPECT(!set_entity_property_int(doc, EntityKind::Item, 0, "ai_flags", 1 << 29, bogus_error));
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
		opennova::bms::File range_reparsed;
		TEST_EXPECT(opennova::bms::parse(bytes.data(), bytes.size(), range_reparsed, err));
		TEST_EXPECT(!range_reparsed.events.empty());
		TEST_EXPECT(range_reparsed.events[0].reset_after == 1023);
		TEST_EXPECT(range_reparsed.events[0].delay == 600);
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
		opennova::bms::File doc;
		TEST_EXPECT(load_document(original, doc));
		// Establish a known-good two-weapon loadout first.
		std::vector<WeaponLoadoutEntry> good;
		good.push_back({"WPN_A", "-1", "-1", "1"});
		good.push_back({"WPN_B", "-1", "-1", "2"});
		TEST_EXPECT(set_weapon_loadout(doc, good, error));
		TEST_EXPECT(weapon_loadout(doc).size() == 2);
		// An edit that blanks a mid-list name is rejected; the loadout is left exactly as it was.
		std::vector<WeaponLoadoutEntry> with_blank;
		with_blank.push_back({"WPN_A", "-1", "-1", "1"});
		with_blank.push_back({"", "-1", "-1", "0"}); // blanked name -> reject the whole edit, no silent drop
		with_blank.push_back({"WPN_B", "-1", "-1", "2"});
		std::string blank_error;
		TEST_EXPECT(!set_weapon_loadout(doc, with_blank, blank_error));
		const std::vector<WeaponLoadoutEntry> reread_kit = weapon_loadout(doc);
		TEST_EXPECT(reread_kit.size() == 2);
		TEST_EXPECT(reread_kit[0].name == "WPN_A");
		TEST_EXPECT(reread_kit[0].flags == "1");
		TEST_EXPECT(reread_kit[1].name == "WPN_B");
		TEST_EXPECT(reread_kit[1].flags == "2");
		// Deleting every weapon (an empty list) is still valid: the chunk goes to length 0.
		TEST_EXPECT(set_weapon_loadout(doc, {}, error));
		TEST_EXPECT(weapon_loadout(doc).empty());
	}

	// --- From-scratch: make_default() builds a valid, empty mission that round-trips. This is
	// the first path that writes freshly-resized waypoint records, so it guards the sync_counts
	// padding backfill (without it the writer emits 8-byte waypoint records that fail to reparse). ---
	{
		opennova::bms::File fresh;
		make_default(fresh);
		TEST_EXPECT(entity_count(fresh, EntityKind::Item) == 0);
		TEST_EXPECT(entity_count(fresh, EntityKind::Marker) == 0);
		std::vector<uint8_t> fresh_bytes;
		TEST_EXPECT(write_document(fresh, fresh_bytes));
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
		opennova::bms::File fresh_round;
		TEST_EXPECT(load_document(fresh_bytes, fresh_round));
		std::vector<uint8_t> rewritten;
		TEST_EXPECT(write_document(fresh_round, rewritten));
		TEST_EXPECT(rewritten == fresh_bytes);
		// A from-scratch mission can adopt a terrain ref and round-trip it.
		TEST_EXPECT(set_header_string(fresh, "terrain", "dvxi5", error));
		std::vector<uint8_t> ref_bytes;
		TEST_EXPECT(write_document(fresh, ref_bytes));
		opennova::bms::File ref_reload;
		TEST_EXPECT(load_document(ref_bytes, ref_reload));
		TEST_EXPECT(mission_info(ref_reload).terrain == "dvxi5");
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
		opennova::bms::File d1;
		opennova::bms::File d2;
		make_default(d1);
		make_default(d2);
		TEST_EXPECT(opennova::bms::equal(d1, d2));
	}

	// --- Regression (review): a waypoint marker_count > 32 (the 32-slot capacity) survives a
	// document load/save. parse preserves a shipped over-count (CP19.bms ships 39) verbatim, but
	// sync_counts used to clobber it to the slot count on every load/save, breaking byte-exact round-trip.
	{
		opennova::bms::File f;
		make_default(f);
		TEST_EXPECT(!f.waypoint_records.empty());
		f.waypoint_records[0].waypoint_numbers.assign(kMaxWaypointPathMarkers, 7u); // saturate the 32 slots
		f.waypoint_records[0].marker_count = 39;                                    // the shipped over-count
		std::vector<uint8_t> bytes;
		TEST_EXPECT(write_document(f, bytes)); // runs sync_counts -> must preserve the saturated over-count
		opennova::bms::File over_reparsed;
		std::string err;
		TEST_EXPECT(opennova::bms::parse(bytes.data(), bytes.size(), over_reparsed, err));
		TEST_EXPECT(over_reparsed.waypoint_records[0].marker_count == 39);
		// But a count that drops below the cap is meaningless as an over-count, so it resyncs to the slots.
		f.waypoint_records[0].waypoint_numbers.assign(5, 7u);
		f.waypoint_records[0].marker_count = 39; // stale over-count, now only 5 slots
		std::vector<uint8_t> bytes2;
		TEST_EXPECT(write_document(f, bytes2));
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
		opennova::bms::File f;
		make_default(f);
		TEST_EXPECT(!f.waypoint_records.empty());
		// 32 waypoint markers so a full 32-index path validates.
		opennova::bms::Entity waypoint{};
		waypoint.type = opennova::bms::ItemType::Marker;
		waypoint.type_id = opennova::def::DEF_TYPE_WAYPOINT;
		f.markers.assign(kMaxWaypointPathMarkers, waypoint);
		// Saturate path 1 at 32 slots and stamp the shipped over-count.
		f.waypoint_records[1].waypoint_numbers.assign(kMaxWaypointPathMarkers, 0u);
		f.waypoint_records[1].marker_count = 39;
		// An authored edit re-applies a (still 32-marker) list, e.g. a flag-only change.
		std::vector<int> indices;
		for (int i = 0; i < static_cast<int>(kMaxWaypointPathMarkers); ++i) {
			indices.push_back(i);
		}
		TEST_EXPECT(set_waypoint_path(f, 1, indices, 1, error));
		// Laid out from its markers at once, and it stays so through save/reparse (no over-count revival).
		TEST_EXPECT(f.waypoint_records[1].marker_count == kMaxWaypointPathMarkers);
		std::vector<uint8_t> wbytes;
		TEST_EXPECT(write_document(f, wbytes));
		opennova::bms::File wreparsed;
		std::string werr;
		TEST_EXPECT(opennova::bms::parse(wbytes.data(), wbytes.size(), wreparsed, werr));
		TEST_EXPECT(wreparsed.waypoint_records[1].marker_count == kMaxWaypointPathMarkers);
	}

	// --- D-MIS-6: a path's stops are its markers' (the original editor's model): an edit sets each marker's
	// waypoint_id and wp_number and lays the path out from them, its count the markers that carry it, its 32
	// slots the first of them [orig: JOTACmed.exe sub_44CFD0 @ 0x44cfd0, sub_44F920 @ 0x44f920]. ---
	{
		opennova::bms::File f;
		make_default(f);
		opennova::bms::Entity waypoint{};
		waypoint.type = opennova::bms::ItemType::Marker;
		waypoint.type_id = opennova::def::DEF_TYPE_WAYPOINT;
		f.markers.assign(40, waypoint);
		std::vector<int> stops;
		for (int i = 39; i >= 0; --i) stops.push_back(i); // 40 stops, the last marker first
		TEST_EXPECT(set_waypoint_path(f, 5, stops, 0, error));
		TEST_EXPECT(f.waypoint_records[5].marker_count == 40 && f.waypoint_records[5].waypoint_numbers.size() == 32);
		TEST_EXPECT(f.waypoint_records[5].waypoint_numbers[0] == 39 && f.waypoint_records[5].waypoint_numbers[31] == 8);
		TEST_EXPECT(f.markers[39].waypoint_id == 5 && f.markers[39].wp_number == 0 && f.markers[0].wp_number == 39);
		WaypointPath forty;
		TEST_EXPECT(waypoint_path(f, 5, forty) && forty.marker_indices == stops);
		// A stop taken off: its marker carries no path; the path counts 39.
		TEST_EXPECT(erase_waypoint_stop(f, 5, 0, error));
		TEST_EXPECT(f.markers[39].waypoint_id == 0 && f.markers[39].wp_number == 0 && f.waypoint_records[5].marker_count == 39);
		TEST_EXPECT(f.markers[38].wp_number == 0 && f.waypoint_records[5].waypoint_numbers[0] == 38);
		// A marker put on another path leaves this one, both laid out again.
		TEST_EXPECT(insert_waypoint_stop(f, 7, 0, 38, error));
		TEST_EXPECT(f.markers[38].waypoint_id == 7 && f.waypoint_records[7].marker_count == 1 &&
		            f.waypoint_records[7].waypoint_numbers == std::vector<uint32_t>({38u}));
		TEST_EXPECT(f.waypoint_records[5].marker_count == 38 && f.waypoint_records[5].waypoint_numbers[0] == 37);
		// A marker one of the path's stops already, or named twice, no waypoint marker, or on path 0, is refused.
		TEST_EXPECT(!insert_waypoint_stop(f, 7, 1, 38, error));
		TEST_EXPECT(!set_waypoint_path(f, 7, {1, 1}, 0, error));
		TEST_EXPECT(!set_waypoint_path(f, 0, {1}, 0, error));
		f.markers[1].type_id = 6001;
		TEST_EXPECT(!insert_waypoint_stop(f, 7, 1, 1, error));
		f.markers[1].type_id = opennova::def::DEF_TYPE_WAYPOINT;
		// A marker removed: the markers past it move down one, every path laid out again from its markers.
		TEST_EXPECT(remove_entity(f, EntityKind::Marker, 0, error));
		TEST_EXPECT(f.waypoint_records[7].waypoint_numbers == std::vector<uint32_t>({37u}));
		TEST_EXPECT(f.waypoint_records[5].marker_count == 37 && f.waypoint_records[5].waypoint_numbers[0] == 36);
		// Written and read again, the markers still carry the paths their records hold.
		std::vector<uint8_t> bytes;
		TEST_EXPECT(write_document(f, bytes));
		opennova::bms::File back;
		std::string err;
		TEST_EXPECT(opennova::bms::parse(bytes.data(), bytes.size(), back, err));
		TEST_EXPECT(back.waypoint_records[5].marker_count == 37 && waypoint_path_markers(back, 5).size() == 37 &&
		            waypoint_path_markers(back, 7) == std::vector<int>({37}));
	}

	// --- Regression (review): set_event / add_event clamp reset_after & delay to 0..1023 (their packed
	// 10-bit range) at the library boundary, so an out-of-range value can't wrap on serialize. ---
	{
		opennova::bms::File doc;
		make_default(doc);
		MissionEventRecord rec; // zero-initialized
		rec.delay = 2000;        // > 1023: would pack as (uint32)2000 << 22 (truncates) and reparse as 976
		rec.reset_after = 5000;  // > 1023
		const size_t index = add_event(doc, rec);
		MissionEventRecord out;
		TEST_EXPECT(event(doc, index, out));
		TEST_EXPECT(out.delay == 1023);
		TEST_EXPECT(out.reset_after == 1023);
	}

	// --- Regression (review): a secondary-chunk length larger than the bytes remaining fails the parse
	// cleanly (count_fits guard) instead of silently zero-filling + mis-aligning every later section. ---
	{
		std::string err;
		std::vector<uint8_t> bytes = original;
		write_u16_le(bytes, offsetof(opennova::bms::Header, secondary_chunk_len), 0xFFFF);
		opennova::bms::File chunk_reparsed;
		TEST_EXPECT(!opennova::bms::parse(bytes.data(), bytes.size(), chunk_reparsed, err));
	}

	// [orig: AIProfile_SanitizeConfigData @ 0x40cfe0] The sanitizer walks records,
	// preserves the first three strings and accepts nonzero atoi prefixes verbatim.
	// A missing fourth field leaves the following name unconsumed.
	{
		const auto parse_loadout = [&](const std::vector<std::string> &fields,
		                               opennova::bms::File &parsed) {
			std::vector<uint8_t> chunk;
			for (const std::string &field : fields) {
				chunk.insert(chunk.end(), field.begin(), field.end());
				chunk.push_back(0);
			}
			chunk.push_back(0);
			std::vector<uint8_t> bytes = original;
			const auto begin = bytes.begin() + opennova::bms::kHeaderSize;
			bytes.erase(begin, begin + original_loadout_len);
			bytes.insert(bytes.begin() + opennova::bms::kHeaderSize, chunk.begin(), chunk.end());
			write_u16_le(bytes, offsetof(opennova::bms::Header, weapon_loadout_chunk_len),
			             static_cast<uint16_t>(chunk.size()));
			std::string err;
			return opennova::bms::parse(bytes.data(), bytes.size(), parsed, err);
		};
		opennova::bms::File parsed;
		TEST_EXPECT(parse_loadout({"CUSTOM_KIT", "abc", "4suffix", "2damage",
		                           "WPN_EMPTY", "", "-ammo", "+0.5",
		                           "WPN_THREE", "-1", "0",
		                           "WPN_LAST", "WPN_ammo", "", "--"}, parsed));
		const auto &kit = parsed.loadout.entries;
		TEST_EXPECT(kit.size() == 4);
		TEST_EXPECT(kit[0].name == "CUSTOM_KIT" && kit[0].ammo_primary == "abc" &&
		            kit[0].ammo_secondary == "4suffix" && kit[0].flags == "2damage");
		TEST_EXPECT(kit[1].ammo_primary.empty() && kit[1].ammo_secondary == "-ammo" &&
		            kit[1].flags == "+0.5");
		TEST_EXPECT(kit[2].name == "WPN_THREE" && kit[2].flags == "-1");
		TEST_EXPECT(kit[3].name == "WPN_LAST" && kit[3].ammo_primary == "WPN_ammo" &&
		            kit[3].ammo_secondary.empty() && kit[3].flags == "--");
		std::vector<uint8_t> canonical;
		std::string err;
		TEST_EXPECT(opennova::bms::write(parsed, canonical, err));
		opennova::bms::File round_trip;
		TEST_EXPECT(opennova::bms::parse(canonical.data(), canonical.size(), round_trip, err));
		TEST_EXPECT(opennova::bms::equal(parsed, round_trip));
		TEST_EXPECT(parse_loadout({"LAST_THREE", "+", "-1"}, parsed));
		TEST_EXPECT(parsed.loadout.entries.size() == 1 && parsed.loadout.entries[0].flags == "-1");
		TEST_EXPECT(parse_loadout({"WPN_FIRST", "0", "1", "0suffix", "2", "3", "4"}, parsed));
		TEST_EXPECT(parsed.loadout.entries.size() == 2 && parsed.loadout.entries[0].flags == "-1" &&
		            parsed.loadout.entries[1].name == "0suffix");
		TEST_EXPECT(parse_loadout({"WPN_SHORT"}, parsed));
		TEST_EXPECT(parsed.loadout.entries.size() == 1 &&
		            parsed.loadout.entries[0].ammo_primary.empty() &&
		            parsed.loadout.entries[0].ammo_secondary.empty() &&
		            parsed.loadout.entries[0].flags == "-1");

		// A record that wrote three strings writes three (has_flags, bms.h): the chunk comes back as
		// its own bytes, where the writer once gave every record a fourth. flags still reads the "-1"
		// the sanitizer inserts; a Set of it through its field row writes the fourth string.
		const std::vector<std::string> mixed = {"WPN_FOUR", "6", "-1", "1", "WPN_THREE", "-1", "0",
		                                        "WPN_ALSO", "2", "3"};
		TEST_EXPECT(parse_loadout(mixed, parsed));
		TEST_EXPECT(parsed.loadout.entries.size() == 3);
		TEST_EXPECT(parsed.loadout.entries[0].has_flags && !parsed.loadout.entries[1].has_flags &&
		            !parsed.loadout.entries[2].has_flags);
		TEST_EXPECT(parsed.loadout.entries[1].flags == "-1" && parsed.loadout.entries[2].flags == "-1");
		std::vector<uint8_t> chunk;
		for (const std::string &field : mixed) {
			chunk.insert(chunk.end(), field.begin(), field.end());
			chunk.push_back(0);
		}
		chunk.push_back(0);
		const auto chunk_of = [](const std::vector<uint8_t> &bytes) {
			const size_t length = read_u16_le(bytes, offsetof(opennova::bms::Header, weapon_loadout_chunk_len));
			return std::vector<uint8_t>(bytes.begin() + opennova::bms::kHeaderSize,
			                            bytes.begin() + opennova::bms::kHeaderSize + static_cast<std::ptrdiff_t>(length));
		};
		TEST_EXPECT(opennova::bms::write(parsed, canonical, err));
		TEST_EXPECT(chunk_of(canonical) == chunk);
		TEST_EXPECT(parsed.header.weapon_loadout_chunk_len == chunk.size());
		opennova::mission::sync_counts(parsed);
		TEST_EXPECT(parsed.header.weapon_loadout_chunk_len == chunk.size());
		TEST_EXPECT(opennova::bms::parse(canonical.data(), canonical.size(), round_trip, err));
		TEST_EXPECT(opennova::bms::equal(parsed, round_trip));
		// The typed view carries it, so a loadout read and set again writes the same chunk.
		auto entries = weapon_loadout(parsed);
		TEST_EXPECT(entries.size() == 3 && entries[0].has_flags && !entries[1].has_flags);
		TEST_EXPECT(set_weapon_loadout(parsed, entries, err));
		TEST_EXPECT(opennova::bms::write(parsed, canonical, err) && chunk_of(canonical) == chunk);
		opennova::bms::File changed = parsed;
		changed.loadout.entries[1].has_flags = true;
		TEST_EXPECT(!opennova::bms::equal(parsed, changed));
		// A fourth string given to a record that had none is written: through the typed view...
		opennova::bms::File through_view = parsed;
		auto given = entries;
		given[1].flags = "2";
		given[1].has_flags = true;
		TEST_EXPECT(set_weapon_loadout(through_view, given, err));
		std::vector<uint8_t> through_view_bytes;
		TEST_EXPECT(opennova::bms::write(through_view, through_view_bytes, err));
		// ...and through its field row (a Set of flags).
		const opennova::mission::MissionField *flags =
		        opennova::mission::find_mission_field(opennova::mission::MissionRecord::Loadout, "flags");
		TEST_EXPECT(flags != nullptr && flags->set(&parsed.loadout.entries[1], std::string("2"), err));
		TEST_EXPECT(parsed.loadout.entries[1].has_flags && parsed.loadout.entries[1].flags == "2");
		TEST_EXPECT(opennova::bms::write(parsed, canonical, err));
		const std::vector<std::string> set = {"WPN_FOUR", "6", "-1", "1", "WPN_THREE", "-1", "0", "2",
		                                      "WPN_ALSO", "2", "3"};
		chunk.clear();
		for (const std::string &field : set) {
			chunk.insert(chunk.end(), field.begin(), field.end());
			chunk.push_back(0);
		}
		chunk.push_back(0);
		TEST_EXPECT(chunk_of(canonical) == chunk);
		TEST_EXPECT(chunk_of(through_view_bytes) == chunk);
	}

	// --- Modeling policy: the loader sanitizes the loadout chunk into the records it reads.
	// Bytes after the empty-name terminator are ignored by the retail sanitizer and are
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
		const std::vector<uint8_t> source_header(
				bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(opennova::bms::kHeaderSize));
		const uint16_t canonical_loadout_len = parsed_trailing.header.weapon_loadout_chunk_len;
		std::vector<uint8_t> canonical_header;
		TEST_EXPECT(opennova::bms::encode_header_blob(parsed_trailing, canonical_header, err));
		TEST_EXPECT(read_u16_le(canonical_header,
		                        offsetof(opennova::bms::Header, weapon_loadout_chunk_len)) ==
		            canonical_loadout_len);
		TEST_EXPECT(canonical_loadout_len != original_loadout_len + sizeof(trailing));
		std::vector<uint8_t> loaded_header;
		TEST_EXPECT(opennova::bms::encode_loaded_header_blob(parsed_trailing, loaded_header, err));
		TEST_EXPECT(loaded_header == source_header);
		TEST_EXPECT(read_u16_le(loaded_header,
		                        offsetof(opennova::bms::Header, weapon_loadout_chunk_len)) ==
		            original_loadout_len + sizeof(trailing));
		std::vector<uint8_t> canonical;
		TEST_EXPECT(opennova::bms::write(parsed_trailing, canonical, err));
		TEST_EXPECT(std::equal(canonical_header.begin(), canonical_header.end(), canonical.begin()));
		opennova::bms::File reparsed_trailing;
		TEST_EXPECT(opennova::bms::parse(canonical.data(), canonical.size(), reparsed_trailing, err));
		TEST_EXPECT(opennova::bms::equal(parsed_trailing, reparsed_trailing));

		// Parsed-header provenance is usable only while the current canonical header
		// projection still matches the load-time snapshot. A direct edit must force
		// the network encoder onto fresh canonical bytes rather than stale source bytes.
		parsed_trailing.header.mission_name[0] ^= 0x01;
		std::vector<uint8_t> edited_canonical_header;
		TEST_EXPECT(opennova::bms::encode_header_blob(
				parsed_trailing, edited_canonical_header, err));
		std::vector<uint8_t> edited_loaded_header;
		TEST_EXPECT(opennova::bms::encode_loaded_header_blob(
				parsed_trailing, edited_loaded_header, err));
		TEST_EXPECT(edited_loaded_header == edited_canonical_header);
		TEST_EXPECT(edited_loaded_header != source_header);
	}

	// --- Wire join: retail sends only the exact 0x268-byte BMS header in S2C 0x0B.
	// A client must be able to build the read-only mission metadata view from that
	// header without opening (or even having) the host's complete .bms locally. ---
	{
		const std::vector<uint8_t> wire_header(
				original.begin(),
				original.begin() + static_cast<std::ptrdiff_t>(opennova::bms::kHeaderSize));
		opennova::bms::File wire_document;
		std::string wire_error;
		TEST_EXPECT(opennova::bms::parse_header_blob(
				wire_header.data(), wire_header.size(), wire_document.header, wire_error));
		TEST_EXPECT(mission_info(wire_document).mission_name == mission_info(document).mission_name);
		TEST_EXPECT(mission_info(wire_document).terrain == mission_info(document).terrain);
		TEST_EXPECT(mission_info(wire_document).environment == mission_info(document).environment);
		TEST_EXPECT(entity_count(wire_document, EntityKind::Item) == 0);
		TEST_EXPECT(entity_count(wire_document, EntityKind::Building) == 0);
		TEST_EXPECT(entity_count(wire_document, EntityKind::Marker) == 0);
		TEST_EXPECT(entity_count(wire_document, EntityKind::Organic) == 0);
		// (A wire-header view is intentionally not a synthesizable authoring document;
		// the binding refuses to save one -- writing it would silently turn the host's
		// non-empty map into an empty BMS.)

		opennova::bms::Header short_header;
		TEST_EXPECT(!opennova::bms::parse_header_blob(
				wire_header.data(), wire_header.size() - 1, short_header, wire_error));
		std::vector<uint8_t> oversized_header = wire_header;
		oversized_header.push_back(0);
		TEST_EXPECT(!opennova::bms::parse_header_blob(
				oversized_header.data(), oversized_header.size(), short_header, wire_error));
	}

	// E1 record-wrapper retirement: inspect and edit the native document
	// directly, then verify the authored values through a serialized reload.
	{
		opennova::bms::File doc;
		TEST_EXPECT(load_document(original, doc));
		TEST_EXPECT(groups(doc).size() == 64);
		const auto neighbor = doc.group_records[4];
		TEST_EXPECT(set_group(doc, 3, 3, 5678, 10, error));
		TEST_EXPECT(doc.group_records[4].flags == neighbor.flags &&
				doc.group_records[4].value == neighbor.value);
		GroupFields fields;
		TEST_EXPECT(group(doc, 3, fields));
		TEST_EXPECT(fields.index == 3 && fields.field0 == 3 &&
				fields.field8 == 5678 && fields.field12 == 10);
		TEST_EXPECT(!group(doc, 999, fields));
		TEST_EXPECT(!set_group(doc, 999, 1, 2, 3, error));
		TEST_EXPECT(!set_group(doc, 3, 4, 2, 10, error));
		TEST_EXPECT(!set_group(doc, 3, 3, 2, 11, error));

		const auto original_zones = doc.area_triggers.size();
		AreaTriggerRecord zone;
		zone.wp_number = 3;
		zone.min_x = -10; zone.min_y = -20; zone.min_z = -8;
		zone.max_x = 10; zone.max_y = 20; zone.max_z = 8;
		zone.active = true;
		zone.constrain_z = true;
		zone.reserved = 0x40; // unknown bits survive the typed view and edit
		const auto zone_index = add_area_trigger(doc, zone);
		TEST_EXPECT(zone_index == original_zones);
		TEST_EXPECT(area_triggers(doc).size() == original_zones + 1);
		TEST_EXPECT(area_trigger(doc, zone_index, zone));
		TEST_EXPECT(zone.index == zone_index && zone.wp_number == 3);
		TEST_EXPECT(zone.active && zone.constrain_z);
		zone.max_x = 30;
		zone.constrain_z = false;
		TEST_EXPECT(set_area_trigger(doc, zone_index, zone, error));
		TEST_EXPECT(!set_area_trigger(doc, 999999, zone, error));

		auto kit = weapon_loadout(doc);
		TEST_EXPECT(kit.size() == 4);
		TEST_EXPECT(kit[0].name == "WPN_M4AUTO" && kit[0].ammo_primary == "6");
		TEST_EXPECT(kit[0].flags == "-1");
		kit[0].ammo_primary = "5";
		kit[0].flags = "1";
		kit.push_back({"WPN_TEST", "1", "2", "2"});
		TEST_EXPECT(set_weapon_loadout(doc, kit, error));

		std::vector<uint8_t> bytes;
		TEST_EXPECT(write_document(doc, bytes));
		opennova::bms::File reloaded;
		TEST_EXPECT(load_document(bytes, reloaded));
		TEST_EXPECT(group(reloaded, 3, fields) && fields.field8 == 5678);
		TEST_EXPECT(area_trigger(reloaded, zone_index, zone));
		TEST_EXPECT(zone.max_x == 30 && zone.active && !zone.constrain_z);
		TEST_EXPECT((zone.reserved & 0x40) != 0);
		const auto reloaded_kit = weapon_loadout(reloaded);
		TEST_EXPECT(reloaded_kit.size() == 5);
		TEST_EXPECT(reloaded_kit[0].ammo_primary == "5" && reloaded_kit[0].flags == "1");
		TEST_EXPECT(reloaded_kit[4].name == "WPN_TEST" && reloaded_kit[4].flags == "2");
		TEST_EXPECT(remove_area_trigger(reloaded, zone_index, error));
		TEST_EXPECT(reloaded.area_triggers.size() == original_zones);
		TEST_EXPECT(!area_trigger(reloaded, zone_index, zone));
		TEST_EXPECT(!remove_area_trigger(reloaded, 999999, error));
	}

	// A zone parameter is the area trigger's ID in the file, never its index (the game remaps it at
	// mission start [orig: EventTrigger_ResolveZoneTriggerRefs @0x453000]): removing an area trigger
	// rewrites no parameter, the ones naming another zone keep naming it, and the ones naming the
	// removed zone name none (flagged, as the game neuters them).
	{
		opennova::bms::File doc;
		make_default(doc);
		for (const int id : {7, 3, 9}) {
			AreaTriggerRecord zone;
			zone.wp_number = id;
			zone.max_x = 10;
			zone.max_y = 10;
			add_area_trigger(doc, zone);
		}
		MissionEventRecord seed;
		TEST_EXPECT(add_event(doc, seed) == 0);
		MissionTriggerRecord within;
		within.main_type = static_cast<int>(opennova::bms::TriggerMainType::Group);
		within.sub_type = static_cast<int>(opennova::bms::GroupTriggerType::GroupIsWithinArea);
		within.param1 = 5;
		within.param2 = 3; // the zone whose id is 3 (at index 1)
		TEST_EXPECT(insert_event_trigger(doc, 0, 0, within, error));
		MissionTriggerRecord satchel;
		satchel.main_type = static_cast<int>(opennova::bms::TriggerMainType::Player);
		satchel.sub_type = static_cast<int>(opennova::bms::PlayerTriggerType::PlayerSatchel);
		satchel.param1 = 9; // the zone whose id is 9 (at index 2)
		TEST_EXPECT(insert_event_trigger(doc, 0, 1, satchel, error));
		MissionActionRecord area_ai;
		area_ai.action_type = static_cast<int>(opennova::bms::ActionType::AreaAiRed);
		area_ai.param1 = 9;
		TEST_EXPECT(insert_event_action(doc, 0, 0, area_ai, error));
		MissionEventChain chain;
		TEST_EXPECT(event_chain(doc, 0, chain) && chain.diagnostics.empty());
		const auto area_of = [&chain](int slot) {
			for (const MissionLogicReference &reference : chain.references)
				if (reference.target_kind == "area_trigger" && reference.param_slot == slot) return reference.target_index;
			return -99;
		};
		TEST_EXPECT(area_of(2) == 1 && area_of(1) == 2);
		// The first area trigger (id 7) out: the others' indexes move, their ids and every parameter stay.
		TEST_EXPECT(remove_area_trigger(doc, 0, error) && doc.area_triggers.size() == 2);
		TEST_EXPECT(doc.triggers[0].param2 == 3 && doc.triggers[1].param1 == 9 && doc.actions[0].param1 == 9);
		TEST_EXPECT(event_chain(doc, 0, chain) && chain.diagnostics.empty() && area_of(2) == 0 && area_of(1) == 1);
		// The zone a trigger names out: the trigger keeps its id and names none.
		TEST_EXPECT(remove_area_trigger(doc, 0, error) && doc.triggers[0].param2 == 3);
		TEST_EXPECT(event_chain(doc, 0, chain) && chain.diagnostics.size() == 1 &&
		            chain.diagnostics[0].code == "logic.area_reference_out_of_range" && area_of(2) == -1 && area_of(1) == 0);
		// A zone whose box is degenerate (x_min == x_max) is one the game neuters a trigger naming
		// [orig: EventTrigger_ResolveZoneTriggerRefs @0x453000, the test @0x453093]: flagged, its index kept.
		AreaTriggerRecord flat;
		TEST_EXPECT(area_trigger(doc, 0, flat));
		flat.max_x = flat.min_x;
		TEST_EXPECT(set_area_trigger(doc, 0, flat, error));
		TEST_EXPECT(event_chain(doc, 0, chain) && chain.diagnostics.size() == 2 && area_of(1) == 0);
	}

	// What names an event by its index: an Event trigger's first parameter [orig:
	// EventTrigger_EvaluateCondition @0x453620, main type 3 reads events[p1]] and a ResetEvent action's
	// [orig: EventAction_Dispatch @0x4542e0, case 34]. Removing an event moves those past the hole down
	// by one and leaves the ones naming it dangling (-1).
	{
		opennova::bms::File doc;
		make_default(doc);
		MissionEventRecord seed;
		for (int i = 0; i < 4; ++i) TEST_EXPECT(add_event(doc, seed) == static_cast<size_t>(i));
		MissionTriggerRecord fired;
		fired.main_type = static_cast<int>(opennova::bms::TriggerMainType::Event);
		for (const int named : {0, 1, 2}) {
			fired.param1 = named;
			TEST_EXPECT(insert_event_trigger(doc, 3, static_cast<size_t>(named), fired, error));
		}
		MissionActionRecord reset;
		reset.action_type = static_cast<int>(opennova::bms::ActionType::ResetEvent);
		reset.param1 = 2;
		TEST_EXPECT(insert_event_action(doc, 3, 0, reset, error));
		TEST_EXPECT(remove_event(doc, 1, error) && doc.events.size() == 3);
		TEST_EXPECT(doc.triggers.size() == 3 && doc.triggers[0].param1 == 0 && doc.triggers[1].param1 == -1 &&
		            doc.triggers[2].param1 == 1);
		TEST_EXPECT(doc.actions.size() == 1 && doc.actions[0].param1 == 1);
		// event_chain reports both the same way: each Event trigger's and each ResetEvent action's
		// event, the dangling one flagged.
		MissionEventChain chain;
		TEST_EXPECT(event_chain(doc, 2, chain));
		std::vector<std::pair<std::string, int>> named;
		for (const MissionLogicReference &reference : chain.references)
			if (reference.target_kind == "event") named.push_back({reference.source_kind, reference.target_index});
		const std::vector<std::pair<std::string, int>> expected = {
				{"trigger", 0}, {"trigger", -1}, {"trigger", 1}, {"action", 1}};
		TEST_EXPECT(named == expected);
		TEST_EXPECT(chain.diagnostics.size() == 1 && chain.diagnostics[0].code == "logic.event_reference_out_of_range" &&
		            chain.diagnostics[0].subject_kind == "trigger" && chain.diagnostics[0].subject_index == 1);
	}

	// E1: entity-property inspection is portable. A group-only edit must
	// preserve the complete serialized document for every entity kind.
	for (const auto kind : {EntityKind::Marker, EntityKind::Item,
			EntityKind::Building, EntityKind::Organic}) {
		opennova::bms::File doc;
		TEST_EXPECT(load_document(original, doc));
		const auto *rows = entities(doc, kind);
		TEST_EXPECT(rows != nullptr && !rows->empty());
		auto expected = doc;
		auto *expected_rows = entities(expected, kind);
		const int new_group = rows->front().group_id + 1;
		expected_rows->front().group_id = new_group;
		TEST_EXPECT(set_entity_property_int(doc, kind, 0, "group", new_group, error));
		std::vector<uint8_t> actual_bytes, expected_bytes;
		TEST_EXPECT(write_document(doc, actual_bytes));
		TEST_EXPECT(write_document(expected, expected_bytes));
		TEST_EXPECT(actual_bytes == expected_bytes);
	}

	if (chain_ranges() != 0) return 1;
	if (sections_and_route() != 0) return 1;
	return 0;
}
