#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include "mission/bms.h"
#include "mission/mission.h"
#include "mission/mission_capi.h"

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

std::string temp_path(const char *name) {
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/build/test-output";
	std::filesystem::create_directories(dir);
	return dir + "/" + name;
}

// Direct C++ exercise of the whole-event CRUD (add_event / remove_event) and the enum reflectors,
// the genuinely new mission-scripting logic. Uses opennova::mission::MissionDocument straight (these
// have no C-ABI wrapper: the Godot editor calls the C++ document directly), and round-trips the bytes.
// Returns 0 on success; TEST_EXPECT returns 1 from here on the first failed expectation.
int test_event_scripting_cpp(const std::vector<uint8_t> &original) {
	using namespace opennova::mission;
	namespace bms = opennova::bms;
	MissionDocument doc;
	TEST_EXPECT(doc.load_bms_bytes(original.data(), original.size()));

	const size_t base_events = doc.event_count();
	const size_t base_triggers = doc.trigger_count();
	const size_t base_actions = doc.action_count();
	TEST_EXPECT(base_events > 0);

	// add_event appends an empty event carrying only the editable attributes by default.
	MissionEventRecord seed;
	seed.flags = static_cast<int>(bms::EventFlags::ResetAfter);
	seed.reset_after = 7;
	seed.delay = 3;
	MissionEventRecord added;
	TEST_EXPECT(doc.add_event(seed, &added));
	TEST_EXPECT(doc.event_count() == base_events + 1);
	TEST_EXPECT(added.index == base_events);
	TEST_EXPECT(added.flags == static_cast<int>(bms::EventFlags::ResetAfter));
	TEST_EXPECT(added.reset_after == 7);
	TEST_EXPECT(added.delay == 3);
	TEST_EXPECT(added.trigger_count == 0);
	TEST_EXPECT(added.action_count == 0);

	// Confirmed internal event bits can be seeded on add and are preserved by editor-style edits that only
	// submit author-facing checkbox bits.
	{
		MissionDocument flag_doc;
		TEST_EXPECT(flag_doc.load_bms_bytes(original.data(), original.size()));
		MissionEventRecord internal_seed;
		internal_seed.flags = 0x10 | static_cast<int>(bms::EventFlags::ResetAfter);
		MissionEventRecord internal_added;
		TEST_EXPECT(flag_doc.add_event(internal_seed, &internal_added));
		TEST_EXPECT((internal_added.flags & 0x10) == 0x10);
		TEST_EXPECT((internal_added.flags & static_cast<int>(bms::kEventAuthorFlagMask)) ==
		            static_cast<int>(bms::EventFlags::ResetAfter));

		MissionEventRecord edit = internal_added;
		edit.flags = static_cast<int>(bms::EventFlags::PreMission);
		MissionEventRecord edited;
		TEST_EXPECT(flag_doc.set_event(internal_added.index, edit, &edited));
		TEST_EXPECT((edited.flags & 0x10) == 0x10);
		TEST_EXPECT((edited.flags & static_cast<int>(bms::kEventAuthorFlagMask)) ==
		            static_cast<int>(bms::EventFlags::PreMission));
	}

	// Fill the new event with one trigger and one (self-referencing) ResetEvent action.
	MissionTriggerRecord trig;
	trig.main_type = static_cast<int>(bms::TriggerMainType::Single);
	trig.sub_type = static_cast<int>(bms::SingleTriggerType::SingleIsWithinArea);
	trig.param2 = 0;  // area-trigger index reference
	TEST_EXPECT(doc.insert_event_trigger(added.index, 0, trig));
	TEST_EXPECT(doc.trigger_count() == base_triggers + 1);

	MissionActionRecord act;
	act.action_type = static_cast<int>(bms::ActionType::ResetEvent);
	act.param1 = static_cast<int>(added.index);  // points at the new event
	TEST_EXPECT(doc.insert_event_action(added.index, 0, act));
	TEST_EXPECT(doc.action_count() == base_actions + 1);

	MissionEventChain chain;
	TEST_EXPECT(doc.get_event_chain(added.index, chain));
	TEST_EXPECT(chain.triggers.size() == 1);
	TEST_EXPECT(chain.actions.size() == 1);
	TEST_EXPECT(chain.triggers[0].main_type_name == "Single");
	TEST_EXPECT(chain.triggers[0].sub_type_name == "SingleIsWithinArea");
	TEST_EXPECT(chain.actions[0].action_type_name == "ResetEvent");

	// The augmented document round-trips through the byte writer.
	std::vector<uint8_t> bytes;
	TEST_EXPECT(doc.write_bms_bytes(bytes));
	MissionDocument reload;
	TEST_EXPECT(reload.load_bms_bytes(bytes.data(), bytes.size()));
	TEST_EXPECT(reload.event_count() == base_events + 1);
	MissionEventChain reloaded;
	TEST_EXPECT(reload.get_event_chain(base_events, reloaded));
	TEST_EXPECT(reloaded.triggers.size() == 1);
	TEST_EXPECT(reloaded.actions.size() == 1);
	TEST_EXPECT(reloaded.event.reset_after == 7);
	TEST_EXPECT(reloaded.event.delay == 3);

	// remove_event drains the event's ranges and repairs ResetEvent refs. Removing event 0 shifts the
	// appended event (and its self-reference) down by one; the reference must follow to base_events - 1.
	const size_t triggers_before = doc.trigger_count();
	const size_t actions_before = doc.action_count();
	MissionEventChain ev0;
	TEST_EXPECT(doc.get_event_chain(0, ev0));
	const size_t ev0_triggers = ev0.triggers.size();
	const size_t ev0_actions = ev0.actions.size();

	TEST_EXPECT(doc.remove_event(0));
	TEST_EXPECT(doc.event_count() == base_events);
	TEST_EXPECT(doc.trigger_count() == triggers_before - ev0_triggers);
	TEST_EXPECT(doc.action_count() == actions_before - ev0_actions);

	MissionEventChain moved;
	TEST_EXPECT(doc.get_event_chain(base_events - 1, moved));
	TEST_EXPECT(moved.actions.size() == 1);
	TEST_EXPECT(moved.actions[0].action_type == static_cast<int>(bms::ActionType::ResetEvent));
	TEST_EXPECT(moved.actions[0].param1 == static_cast<int>(base_events - 1));
	bool reset_ref_valid = false;
	for (const MissionLogicReference &ref : moved.references) {
		if (ref.source_kind == "action" && ref.target_kind == "event") {
			reset_ref_valid = ref.valid;
		}
	}
	TEST_EXPECT(reset_ref_valid);

	// Exact-hit repair: a ResetEvent that points AT the removed event (not merely past it) becomes -1
	// (dangling), which get_event_chain then flags. Add a throwaway event and a ResetEvent in the earlier
	// appended event that targets it, then remove the throwaway.
	MissionEventRecord blank;
	MissionEventRecord throwaway;
	TEST_EXPECT(doc.add_event(blank, &throwaway));
	const size_t throwaway_index = throwaway.index;
	MissionActionRecord reset_throwaway;
	reset_throwaway.action_type = static_cast<int>(bms::ActionType::ResetEvent);
	reset_throwaway.param1 = static_cast<int>(throwaway_index);
	MissionEventRecord record;
	TEST_EXPECT(doc.get_event(base_events - 1, record));
	TEST_EXPECT(doc.insert_event_action(base_events - 1, record.action_count, reset_throwaway));
	TEST_EXPECT(doc.remove_event(throwaway_index));
	MissionEventChain after_exact;
	TEST_EXPECT(doc.get_event_chain(base_events - 1, after_exact));
	bool found_dangling = false;
	for (const MissionActionRecord &a : after_exact.actions) {
		if (a.action_type == static_cast<int>(bms::ActionType::ResetEvent) && a.param1 == -1) {
			found_dangling = true;
		}
	}
	TEST_EXPECT(found_dangling);
	// The dangling reference is flagged by the chain diagnostics.
	bool flagged = false;
	for (const MissionLogicDiagnostic &d : after_exact.diagnostics) {
		if (d.code == "logic.event_reference_out_of_range") {
			flagged = true;
		}
	}
	TEST_EXPECT(flagged);

	// Enum reflectors are generated by probing the name switches, so non-contiguous / high-numbered
	// values appear without a hand-maintained table.
	const std::vector<MissionEnumEntry> mains = doc.trigger_main_types();
	TEST_EXPECT(mains.size() == 7);  // Group(1)..Player(7)
	TEST_EXPECT(mains.front().value == static_cast<int>(bms::TriggerMainType::Group));
	TEST_EXPECT(mains.front().name == "Group");

	const std::vector<MissionEnumEntry> single_subs = doc.trigger_sub_types(static_cast<int>(bms::TriggerMainType::Single));
	bool found_high_single = false;
	for (const MissionEnumEntry &e : single_subs) {
		if (e.value == 45) {
			found_high_single = e.name == "SingleDoesNotSeeOrFarther";
		}
	}
	TEST_EXPECT(found_high_single);

	const std::vector<MissionEnumEntry> action_kinds = doc.action_types();
	bool found_reset_action = false;
	for (const MissionEnumEntry &e : action_kinds) {
		if (e.value == static_cast<int>(bms::ActionType::ResetEvent)) {
			found_reset_action = e.name == "ResetEvent";
		}
	}
	TEST_EXPECT(found_reset_action);

	const std::vector<MissionEnumEntry> ai_subs = doc.action_sub_types(static_cast<int>(bms::ActionType::ChangeGroupAI));
	bool found_firing_angle = false;
	for (const MissionEnumEntry &e : ai_subs) {
		if (e.value == 46) {
			found_firing_angle = e.name == "FiringAngle";
		}
	}
	TEST_EXPECT(found_firing_angle);

	// Three author-facing event flags, matching the DFX2 editor exactly. Confirmed internal bits 0x10/0x20
	// are preserve-only, not surfaced. [orig: Med_EventDialogCommit @0x4118d0 sets bits 0/1/2 only]
	const std::vector<MissionEnumEntry> flag_bits = doc.event_flag_bits();
	TEST_EXPECT(flag_bits.size() == 3);
	TEST_EXPECT(flag_bits.front().value == static_cast<int>(bms::EventFlags::ResetAfter));
	// event_flag_mask is the OR of the author-facing bits = 0x07.
	TEST_EXPECT(doc.event_flag_mask() == 0x7);
	return 0;
}

// Zone-delete reference repair. Phase-5 RE confirmed *IsWithinArea param2 is the area-trigger ARRAY INDEX,
// so removing a zone must shift higher references down and set a direct reference to -1 (dangling).
int test_zone_reference_repair_cpp(const std::vector<uint8_t> &original) {
	using namespace opennova::mission;
	namespace bms = opennova::bms;
	MissionDocument doc;
	TEST_EXPECT(doc.load_bms_bytes(original.data(), original.size()));

	// Append three zones; capture their indices (base, base+1, base+2).
	const size_t base = doc.area_trigger_count();
	AreaTriggerRecord z;
	z.min_x = -1.0f; z.max_x = 1.0f; z.min_y = -1.0f; z.max_y = 1.0f;
	AreaTriggerRecord z0, z1, z2;
	TEST_EXPECT(doc.add_area_trigger(z, &z0));
	TEST_EXPECT(doc.add_area_trigger(z, &z1));
	TEST_EXPECT(doc.add_area_trigger(z, &z2));
	TEST_EXPECT(z0.index == base && z1.index == base + 1 && z2.index == base + 2);

	// A fresh event with three GroupIsWithinArea triggers referencing the three zones via param2.
	MissionEventRecord seed, ev;
	TEST_EXPECT(doc.add_event(seed, &ev));
	for (int i = 0; i < 3; ++i) {
		MissionTriggerRecord t;
		t.main_type = static_cast<int>(bms::TriggerMainType::Group);
		t.sub_type = static_cast<int>(bms::GroupTriggerType::GroupIsWithinArea);
		t.param2 = static_cast<int>(base) + i;  // -> z0, z1, z2
		TEST_EXPECT(doc.insert_event_trigger(ev.index, static_cast<size_t>(i), t));
	}

	// Remove the MIDDLE zone (base+1): the ref to base stays, the ref to base+1 dangles (-1), base+2 -> base+1.
	TEST_EXPECT(doc.remove_area_trigger(base + 1));
	MissionEventChain chain;
	TEST_EXPECT(doc.get_event_chain(ev.index, chain));
	TEST_EXPECT(chain.triggers.size() == 3);
	TEST_EXPECT(chain.triggers[0].param2 == static_cast<int>(base));      // below the hole: unchanged
	TEST_EXPECT(chain.triggers[1].param2 == -1);                          // direct hit: dangling
	TEST_EXPECT(chain.triggers[2].param2 == static_cast<int>(base) + 1);  // above the hole: shifted down

	// The dangling reference is flagged by get_event_chain's area-reference diagnostic.
	bool dangling_flagged = false;
	for (const MissionLogicDiagnostic &d : chain.diagnostics) {
		if (d.code == "logic.area_reference_out_of_range") {
			dangling_flagged = true;
		}
	}
	TEST_EXPECT(dangling_flagged);
	return 0;
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
	properties.ai_flags = static_cast<int>(opennova::bms::BmsiAttributeFlags::Guarding) |
	                      static_cast<int>(opennova::bms::BmsiAttributeFlags::NoShadow);
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

	// A bound beyond the 16.16 representable range (~±32768) must be clamped, not overflowed into
	// garbage (the float->int32 cast would otherwise be UB). The clamped value reads back finite and
	// in range, and survives a byte round-trip.
	OpenNovaMissionAreaTriggerRecord huge = {};
	huge.min_x = -500000.0f;  // far beyond INT32_MAX/65536
	huge.max_x = 500000.0f;
	huge.min_y = -10.0f;
	huge.max_y = 10.0f;
	huge.active = 1;
	OpenNovaMissionAreaTriggerRecord clamped = {};
	TEST_EXPECT(opennova_mission_add_area_trigger(document, &huge, &clamped) == 1);
	TEST_EXPECT(clamped.min_x > -32769.0f && clamped.min_x <= -32767.0f);
	TEST_EXPECT(clamped.max_x < 32769.0f && clamped.max_x >= 32767.0f);
	const size_t clamped_index = clamped.index;
	OpenNovaMissionBytes clamp_bytes = {};
	TEST_EXPECT(opennova_mission_write_bytes(document, &clamp_bytes) == 1);
	OpenNovaMissionDocument *clamp_reload = opennova_mission_create();
	TEST_EXPECT(opennova_mission_load_bytes(clamp_reload, clamp_bytes.data, clamp_bytes.size) == 1);
	OpenNovaMissionAreaTriggerRecord clamp_rt = {};
	TEST_EXPECT(opennova_mission_get_area_trigger(clamp_reload, clamped_index, &clamp_rt) == 1);
	TEST_EXPECT(clamp_rt.min_x <= -32767.0f && clamp_rt.min_x > -32769.0f);
	opennova_mission_destroy(clamp_reload);
	opennova_mission_free_bytes(&clamp_bytes);
	TEST_EXPECT(opennova_mission_remove_area_trigger(document, clamped_index) == 1);

	// Phase 3: weapon loadout. The fixture canonicalizes to 7 public four-field loadout records; the
	// earlier entity/zone edits do not touch the loadout list.
	static_assert(offsetof(OpenNovaMissionWeaponLoadoutEntry, flags) == 192,
	              "flags must remain an ABI tail append");
	static_assert(sizeof(OpenNovaMissionWeaponLoadoutEntry) == 256,
	              "loadout ABI is four fixed 64-byte strings");
	const size_t loadout_count = opennova_mission_weapon_loadout_count(document);
	TEST_EXPECT(loadout_count == 7);
	OpenNovaMissionWeaponLoadoutEntry first_loadout = {};
	TEST_EXPECT(opennova_mission_get_weapon_loadout_entry(document, 0, &first_loadout) == 1);
	TEST_EXPECT(std::string(first_loadout.name) == "WPN_CAR15AUTO");
	TEST_EXPECT(std::string(first_loadout.ammo_primary) == "-1");
	TEST_EXPECT(std::string(first_loadout.ammo_secondary) == "-1");
	TEST_EXPECT(std::string(first_loadout.flags) == "-1");
	OpenNovaMissionWeaponLoadoutEntry last_loadout = {};
	TEST_EXPECT(opennova_mission_get_weapon_loadout_entry(document, loadout_count - 1, &last_loadout) == 1);
	TEST_EXPECT(std::string(last_loadout.name) == "WPN_KNIFE");
	TEST_EXPECT(opennova_mission_get_weapon_loadout_entry(document, loadout_count, &last_loadout) == 0);
	// Re-serialize the parsed entries and confirm the round-trip is identity (4-string format is exact).
	std::vector<OpenNovaMissionWeaponLoadoutEntry> loadout_snapshot(loadout_count);
	for (size_t i = 0; i < loadout_count; ++i) {
		TEST_EXPECT(opennova_mission_get_weapon_loadout_entry(document, i, &loadout_snapshot[i]) == 1);
	}
	TEST_EXPECT(opennova_mission_set_weapon_loadout(document, loadout_snapshot.data(), loadout_count) == 1);
	TEST_EXPECT(opennova_mission_weapon_loadout_count(document) == loadout_count);
	OpenNovaMissionWeaponLoadoutEntry reparsed = {};
	TEST_EXPECT(opennova_mission_get_weapon_loadout_entry(document, 0, &reparsed) == 1);
	TEST_EXPECT(std::string(reparsed.name) == "WPN_CAR15AUTO");
	// Replace the loadout with a custom two-entry set; survive a byte round-trip.
	OpenNovaMissionWeaponLoadoutEntry custom[2] = {};
	std::snprintf(custom[0].name, sizeof(custom[0].name), "WPN_KNIFE");
	std::snprintf(custom[0].ammo_primary, sizeof(custom[0].ammo_primary), "-1");
	std::snprintf(custom[0].ammo_secondary, sizeof(custom[0].ammo_secondary), "-1");
	std::snprintf(custom[0].flags, sizeof(custom[0].flags), "1");
	std::snprintf(custom[1].name, sizeof(custom[1].name), "WPN_M9Berreta");
	std::snprintf(custom[1].ammo_primary, sizeof(custom[1].ammo_primary), "2");
	std::snprintf(custom[1].ammo_secondary, sizeof(custom[1].ammo_secondary), "0");
	std::snprintf(custom[1].flags, sizeof(custom[1].flags), "2");
	TEST_EXPECT(opennova_mission_set_weapon_loadout(document, custom, 2) == 1);
	TEST_EXPECT(opennova_mission_weapon_loadout_count(document) == 2);
	OpenNovaMissionBytes loadout_bytes = {};
	TEST_EXPECT(opennova_mission_write_bytes(document, &loadout_bytes) == 1);
	OpenNovaMissionDocument *loadout_reload = opennova_mission_create();
	TEST_EXPECT(opennova_mission_load_bytes(loadout_reload, loadout_bytes.data, loadout_bytes.size) == 1);
	TEST_EXPECT(opennova_mission_weapon_loadout_count(loadout_reload) == 2);
	OpenNovaMissionWeaponLoadoutEntry reload_entry = {};
	TEST_EXPECT(opennova_mission_get_weapon_loadout_entry(loadout_reload, 1, &reload_entry) == 1);
	TEST_EXPECT(std::string(reload_entry.name) == "WPN_M9Berreta");
	TEST_EXPECT(std::string(reload_entry.ammo_primary) == "2");
	TEST_EXPECT(std::string(reload_entry.ammo_secondary) == "0");
	TEST_EXPECT(std::string(reload_entry.flags) == "2");
	opennova_mission_destroy(loadout_reload);
	opennova_mission_free_bytes(&loadout_bytes);
	// Clearing the loadout yields an empty chunk (the loader installs the WPN_KNIFE default at runtime).
	TEST_EXPECT(opennova_mission_set_weapon_loadout(document, nullptr, 0) == 1);
	TEST_EXPECT(opennova_mission_weapon_loadout_count(document) == 0);
	// The empty chunk (len 0) must survive a byte round-trip — not regress to a stray NUL or a stale length.
	OpenNovaMissionBytes empty_bytes = {};
	TEST_EXPECT(opennova_mission_write_bytes(document, &empty_bytes) == 1);
	OpenNovaMissionDocument *empty_reload = opennova_mission_create();
	TEST_EXPECT(opennova_mission_load_bytes(empty_reload, empty_bytes.data, empty_bytes.size) == 1);
	TEST_EXPECT(opennova_mission_weapon_loadout_count(empty_reload) == 0);
	opennova_mission_destroy(empty_reload);
	opennova_mission_free_bytes(&empty_bytes);

	// Phase 3: groups. 64 fixed records; only the three ints at offsets 0/8/12 are editable.
	TEST_EXPECT(opennova_mission_group_count(document) == opennova::bms::kGroupRecordCount);
	OpenNovaMissionGroupRecord group_before = {};
	TEST_EXPECT(opennova_mission_get_group(document, 7, &group_before) == 1);
	TEST_EXPECT(group_before.index == 7);
	// Capture a neighbour's baseline (the fixture's groups are not all zero) to prove set_group is local.
	OpenNovaMissionGroupRecord neighbour_before = {};
	TEST_EXPECT(opennova_mission_get_group(document, 8, &neighbour_before) == 1);
	TEST_EXPECT(opennova_mission_set_group(document, 7, 3, 222, 10) == 1);
	OpenNovaMissionGroupRecord group_after = {};
	TEST_EXPECT(opennova_mission_get_group(document, 7, &group_after) == 1);
	TEST_EXPECT(group_after.field0 == 3 && group_after.field8 == 222 && group_after.field12 == 10);
	// A different group is untouched by the edit above.
	OpenNovaMissionGroupRecord neighbour_after = {};
	TEST_EXPECT(opennova_mission_get_group(document, 8, &neighbour_after) == 1);
	TEST_EXPECT(neighbour_after.field0 == neighbour_before.field0);
	TEST_EXPECT(neighbour_after.field8 == neighbour_before.field8);
	TEST_EXPECT(neighbour_after.field12 == neighbour_before.field12);
	// Out-of-range guard + byte round-trip of the edited group.
	OpenNovaMissionGroupRecord group_other = {};
	TEST_EXPECT(opennova_mission_get_group(document, opennova::bms::kGroupRecordCount, &group_other) == 0);
	OpenNovaMissionBytes group_bytes = {};
	TEST_EXPECT(opennova_mission_write_bytes(document, &group_bytes) == 1);
	OpenNovaMissionDocument *group_reload = opennova_mission_create();
	TEST_EXPECT(opennova_mission_load_bytes(group_reload, group_bytes.data, group_bytes.size) == 1);
	OpenNovaMissionGroupRecord group_rt = {};
	TEST_EXPECT(opennova_mission_get_group(group_reload, 7, &group_rt) == 1);
	TEST_EXPECT(group_rt.field0 == 3 && group_rt.field8 == 222 && group_rt.field12 == 10);
	opennova_mission_destroy(group_reload);
	opennova_mission_free_bytes(&group_bytes);

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

	TEST_EXPECT(test_event_scripting_cpp(original) == 0);
	TEST_EXPECT(test_zone_reference_repair_cpp(original) == 0);

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
	const std::string mis_path = temp_path("mission_c_abi_dispatch.mis");
	TEST_EXPECT(opennova_mission_save_path(document, mis_path.c_str()) == 1);
	const std::vector<uint8_t> saved_mis = read_file(mis_path);
	TEST_EXPECT(saved_mis.size() > 20);
	const std::string saved_mis_text(reinterpret_cast<const char *>(saved_mis.data()), saved_mis.size());
	TEST_EXPECT(saved_mis_text.find("// mission metafile\r\n") == 0);
	OpenNovaMissionDocument *mis_reload = opennova_mission_create();
	TEST_EXPECT(opennova_mission_load_path(mis_reload, mis_path.c_str()) == 1);
	TEST_EXPECT(opennova_mission_entity_count(mis_reload, 1) > 0);
	OpenNovaMissionInfo mis_info = {};
	TEST_EXPECT(opennova_mission_get_info(mis_reload, &mis_info) == 1);
	TEST_EXPECT(mis_info.mission_name[0] != '\0');
	opennova_mission_destroy(mis_reload);
	opennova_mission_free_bytes(&mis);
	TEST_EXPECT(mis.data == nullptr);
	TEST_EXPECT(mis.size == 0);

	opennova_mission_destroy(document);
	return 0;
}
