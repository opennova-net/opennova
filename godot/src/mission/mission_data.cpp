#include "mission/mission_data.h"

#include "resource_index/resource_root.h"
#include "env/mission_environment_overrides.h"
#include "mission/mission_info.h"
#include "mission/mission_records.h"

#include <formats/env/env.h> // bms_env_overrides_from_header

#include <formats/mission/bms.h>         // AttribFlags / AreaTrigger / Trigger bit names, parse / write
#include <formats/mission/bms_edit.h>    // the document's edit operations + typed views (ADR 0043 E11)
#include <formats/mission/mission.h>     // kItemIdOffset (pins ITEM_ID_OFFSET below)
#include <formats/mission/mission_mis.h> // the .mis text form

#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

using namespace godot;

namespace {

namespace mission = opennova::mission;
namespace bms = opennova::bms;

mission::EntityKind to_native_kind(MissionData::EntityKind kind) {
	switch (kind) {
		case MissionData::KIND_MARKER:
			return mission::EntityKind::Marker;
		case MissionData::KIND_BUILDING:
			return mission::EntityKind::Building;
		case MissionData::KIND_ORGANIC:
			return mission::EntityKind::Organic;
		case MissionData::KIND_ITEM:
		default:
			return mission::EntityKind::Item;
	}
}

// Godot mission-space position + euler degrees -> the record transform. The format stores
// orientation as integer degrees; round rather than truncate.
Ref<MissionEntityRecord> entity_record(const bms::Entity &entity, mission::EntityKind kind, size_t index) {
	Ref<MissionEntityRecord> out;
	out.instantiate();
	out->assign(entity, kind, index);
	return out;
}

template <typename Record, typename Value>
Ref<Record> make_record(const Value &value) {
	Ref<Record> out;
	out.instantiate();
	out->assign(value);
	return out;
}

// Build a trigger / action record from a typed edit, starting from `seed` so the
// trigger's unmodeled condition_flags high bits + unknown7, and the action's
// reserved words, survive an edit. The trigger's negated/logic_or/logic_xor
// booleans compose condition_flags bits 0/1/2 (the canonical source
// trigger_from_record serializes), and the bool mirrors are re-derived from it.
mission::MissionTriggerRecord trigger_from_edit(const mission::MissionTriggerRecord &edit,
		const mission::MissionTriggerRecord &seed) {
	mission::MissionTriggerRecord record = seed;
	record.main_type = edit.main_type;
	record.sub_type = edit.sub_type;
	record.param1 = edit.param1;
	record.param2 = edit.param2;
	record.param3 = edit.param3;
	record.param4 = edit.param4;
	using bms::Trigger;
	constexpr int kConditionMask = Trigger::kConditionNegated | Trigger::kConditionOr | Trigger::kConditionXor;
	int condition = seed.condition_flags & ~kConditionMask;
	if (edit.negated) {
		condition |= Trigger::kConditionNegated;
	}
	if (edit.logic_or) {
		condition |= Trigger::kConditionOr;
	}
	if (edit.logic_xor) {
		condition |= Trigger::kConditionXor;
	}
	record.condition_flags = condition;
	record.negated = (condition & Trigger::kConditionNegated) != 0;
	record.logic_or = (condition & Trigger::kConditionOr) != 0;
	record.logic_xor = (condition & Trigger::kConditionXor) != 0;
	return record;
}

mission::MissionActionRecord action_from_edit(const mission::MissionActionRecord &edit,
		const mission::MissionActionRecord &seed) {
	mission::MissionActionRecord record = seed;
	record.action_type = edit.action_type;
	record.action_sub_type = edit.action_sub_type;
	record.param1 = edit.param1;
	record.param2 = edit.param2;
	record.param3 = edit.param3;
	record.param4 = edit.param4;
	return record; // reserved0/reserved1 ride on the `record = seed` copy
}

mission::EntityTransform transform_from(const Vector3 &position, const Vector3 &rotation_deg) {
	mission::EntityTransform transform;
	transform.x = position.x;
	transform.y = position.y;
	transform.z = position.z;
	transform.pitch = static_cast<int>(std::lround(rotation_deg.x));
	transform.yaw = static_cast<int>(std::lround(rotation_deg.y));
	transform.roll = static_cast<int>(std::lround(rotation_deg.z));
	return transform;
}

} // namespace

int MissionData::load_progress_percent(int p_stage) {
	return mission::mission_load_progress_percent(
			static_cast<mission::MissionLoadStage>(p_stage));
}

static_assert(MissionData::LOAD_STAGE_MISSION_SETUP ==
		static_cast<int>(mission::MissionLoadStage::kMissionSetup));
static_assert(MissionData::LOAD_STAGE_ENVIRONMENT ==
		static_cast<int>(mission::MissionLoadStage::kEnvironment));
static_assert(MissionData::LOAD_STAGE_TERRAIN ==
		static_cast<int>(mission::MissionLoadStage::kTerrain));
static_assert(MissionData::LOAD_STAGE_OBJECTS ==
		static_cast<int>(mission::MissionLoadStage::kObjects));
static_assert(MissionData::LOAD_STAGE_RUNTIME ==
		static_cast<int>(mission::MissionLoadStage::kRuntime));
static_assert(MissionData::LOAD_STAGE_AUDIO ==
		static_cast<int>(mission::MissionLoadStage::kAudio));
static_assert(MissionData::LOAD_STAGE_EFFECTS ==
		static_cast<int>(mission::MissionLoadStage::kEffects));
static_assert(MissionData::LOAD_STAGE_EFFECTS_WARM ==
		static_cast<int>(mission::MissionLoadStage::kEffectsWarm));
static_assert(MissionData::LOAD_STAGE_FINISH ==
		static_cast<int>(mission::MissionLoadStage::kFinish));
static_assert(MissionData::LOAD_STAGE_FINISH + 1 ==
		static_cast<int>(mission::MissionLoadStage::kCount));
static_assert(MissionData::LOAD_PROGRESS_WORLD_READY ==
		mission::kMissionLoadProgressWorldReady);
static_assert(MissionData::LOAD_PROGRESS_COMPLETE ==
		mission::kMissionLoadProgressComplete);

void MissionData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open_file", "path"), &MissionData::open_file);
	ClassDB::bind_method(D_METHOD("create_default"), &MissionData::create_default);
	ClassDB::bind_method(D_METHOD("open_wire_header", "header_bytes"), &MissionData::open_wire_header);
	ClassDB::bind_method(D_METHOD("open_from_resource_root", "resource_root", "name", "lookup_policy"),
			&MissionData::open_from_resource_root, DEFVAL(ResourceRoot::LOOKUP_SESSION_DEFAULT));
	ClassDB::bind_method(D_METHOD("is_loaded"), &MissionData::is_loaded);
	ClassDB::bind_method(D_METHOD("is_wire_header_only"), &MissionData::is_wire_header_only);
	ClassDB::bind_method(D_METHOD("get_source_path"), &MissionData::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &MissionData::get_last_error);
	ClassDB::bind_method(D_METHOD("get_mission_name"), &MissionData::get_mission_name);
	ClassDB::bind_method(D_METHOD("get_terrain_ref"), &MissionData::get_terrain_ref);
	ClassDB::bind_method(D_METHOD("get_environment_ref"), &MissionData::get_environment_ref);
	ClassDB::bind_method(D_METHOD("get_info"), &MissionData::get_info);
	ClassDB::bind_method(D_METHOD("get_environment_overrides"), &MissionData::get_environment_overrides);
	ClassDB::bind_method(D_METHOD("get_entity_count", "kind"), &MissionData::get_entity_count);
	ClassDB::bind_method(D_METHOD("get_entities", "kind"), &MissionData::get_entities);
	ClassDB::bind_method(D_METHOD("get_entity", "kind", "index"), &MissionData::get_entity);
	ClassDB::bind_method(D_METHOD("get_all_entities"), &MissionData::get_all_entities);

	ClassDB::bind_method(D_METHOD("set_entity_transform", "kind", "index", "position", "rotation_deg"), &MissionData::set_entity_transform);
	ClassDB::bind_method(D_METHOD("set_entity_property_int", "kind", "index", "property", "value"), &MissionData::set_entity_property_int);
	ClassDB::bind_method(D_METHOD("set_entity_property_string", "kind", "index", "property", "value"), &MissionData::set_entity_property_string);
	ClassDB::bind_method(D_METHOD("set_header_string", "field", "value"), &MissionData::set_header_string);
	ClassDB::bind_method(D_METHOD("set_header_int", "field", "value"), &MissionData::set_header_int);
	ClassDB::bind_method(D_METHOD("set_header_flag", "bit", "on"), &MissionData::set_header_flag);
	ClassDB::bind_method(D_METHOD("get_game_mode"), &MissionData::get_game_mode);
	ClassDB::bind_method(D_METHOD("set_game_mode", "bit"), &MissionData::set_game_mode);
	ClassDB::bind_method(D_METHOD("add_entity", "kind", "item_id", "position", "rotation_deg"), &MissionData::add_entity);
	ClassDB::bind_method(D_METHOD("remove_entity", "kind", "index"), &MissionData::remove_entity);

	ClassDB::bind_method(D_METHOD("get_waypoint_summaries"), &MissionData::get_waypoint_summaries);
	ClassDB::bind_method(D_METHOD("get_waypoint_path", "index"), &MissionData::get_waypoint_path);
	ClassDB::bind_method(D_METHOD("set_waypoint_path", "index", "marker_indices", "flags"), &MissionData::set_waypoint_path);
	ClassDB::bind_method(D_METHOD("clear_waypoint_path", "index"), &MissionData::clear_waypoint_path);
	ClassDB::bind_method(D_METHOD("add_waypoint_marker", "path_index", "marker_item_id", "position", "rotation_deg", "insert_index"), &MissionData::add_waypoint_marker);

	ClassDB::bind_method(D_METHOD("get_area_trigger_count"), &MissionData::get_area_trigger_count);
	ClassDB::bind_method(D_METHOD("get_area_triggers"), &MissionData::get_area_triggers);
	ClassDB::bind_method(D_METHOD("get_area_trigger", "index"), &MissionData::get_area_trigger);
	ClassDB::bind_method(D_METHOD("add_area_trigger", "min_bounds", "max_bounds", "active", "constrain_z", "zone_id"), &MissionData::add_area_trigger);
	ClassDB::bind_method(D_METHOD("set_area_trigger", "index", "min_bounds", "max_bounds", "active", "constrain_z", "zone_id"), &MissionData::set_area_trigger);
	ClassDB::bind_method(D_METHOD("remove_area_trigger", "index"), &MissionData::remove_area_trigger);

	ClassDB::bind_method(D_METHOD("get_weapon_loadout"), &MissionData::get_weapon_loadout);
	ClassDB::bind_method(D_METHOD("set_weapon_loadout", "entries"), &MissionData::set_weapon_loadout);
	ClassDB::bind_method(D_METHOD("get_group_count"), &MissionData::get_group_count);
	ClassDB::bind_method(D_METHOD("get_groups"), &MissionData::get_groups);
	ClassDB::bind_method(D_METHOD("get_group", "index"), &MissionData::get_group);
	ClassDB::bind_method(D_METHOD("set_group", "index", "field0", "field8", "field12"), &MissionData::set_group);

	ClassDB::bind_method(D_METHOD("get_event_count"), &MissionData::get_event_count);
	ClassDB::bind_method(D_METHOD("get_events"), &MissionData::get_events);
	ClassDB::bind_method(D_METHOD("get_event", "index"), &MissionData::get_event);
	ClassDB::bind_method(D_METHOD("get_event_chain", "index"), &MissionData::get_event_chain);
	ClassDB::bind_method(D_METHOD("get_logic_summary"), &MissionData::get_logic_summary);
	ClassDB::bind_method(D_METHOD("add_event", "flags", "reset_after", "delay"), &MissionData::add_event);
	ClassDB::bind_method(D_METHOD("remove_event", "index"), &MissionData::remove_event);
	ClassDB::bind_method(D_METHOD("set_event", "index", "flags", "reset_after", "delay"), &MissionData::set_event);
	ClassDB::bind_method(D_METHOD("add_event_trigger", "event_index", "trigger"), &MissionData::add_event_trigger);
	ClassDB::bind_method(D_METHOD("set_event_trigger", "event_index", "local_index", "trigger"), &MissionData::set_event_trigger);
	ClassDB::bind_method(D_METHOD("add_event_action", "event_index", "action"), &MissionData::add_event_action);
	ClassDB::bind_method(D_METHOD("save_file"), &MissionData::save_file);
	ClassDB::bind_method(D_METHOD("save_as", "path"), &MissionData::save_as);
	ClassDB::bind_method(D_METHOD("save_to_path", "path"), &MissionData::save_to_path);
	ClassDB::bind_method(D_METHOD("set_mis_base_heights", "flat_write_order"), &MissionData::set_mis_base_heights);
	ClassDB::bind_method(D_METHOD("is_modified"), &MissionData::is_modified);
	ClassDB::bind_method(D_METHOD("object_records_revision"), &MissionData::object_records_revision);

	BIND_ENUM_CONSTANT(KIND_MARKER);
	BIND_ENUM_CONSTANT(KIND_ITEM);
	BIND_ENUM_CONSTANT(KIND_BUILDING);
	BIND_ENUM_CONSTANT(KIND_ORGANIC);
	BIND_CONSTANT(WP_FLAG_DOES_NOT_LOOP);
	BIND_CONSTANT(ATTRIB_FORCE_INDOORS);
	BIND_CONSTANT(LOAD_STAGE_MISSION_SETUP);
	BIND_CONSTANT(LOAD_STAGE_ENVIRONMENT);
	BIND_CONSTANT(LOAD_STAGE_TERRAIN);
	BIND_CONSTANT(LOAD_STAGE_OBJECTS);
	BIND_CONSTANT(LOAD_STAGE_RUNTIME);
	BIND_CONSTANT(LOAD_STAGE_AUDIO);
	BIND_CONSTANT(LOAD_STAGE_EFFECTS);
	BIND_CONSTANT(LOAD_STAGE_EFFECTS_WARM);
	BIND_CONSTANT(LOAD_STAGE_FINISH);
	BIND_CONSTANT(LOAD_PROGRESS_WORLD_READY);
	BIND_CONSTANT(LOAD_PROGRESS_COMPLETE);
	ClassDB::bind_static_method("MissionData", D_METHOD("load_progress_percent", "stage"),
			&MissionData::load_progress_percent);
	BIND_CONSTANT(ATTRIB_ROTATE_MAP_180);
	BIND_CONSTANT(ATTRIB_START_WITH_NVG_ON);
	BIND_CONSTANT(ATTRIB_ADVANCE_AND_SECURE);
	BIND_CONSTANT(ATTRIB_CONQUER_AND_CONTROL);
	BIND_CONSTANT(ATTRIB_ATTACK_AND_DEFEND);
	BIND_CONSTANT(ATTRIB_COOP);
	BIND_CONSTANT(ATTRIB_DEATHMATCH);
	BIND_CONSTANT(ATTRIB_KING_OF_THE_HILL);
	BIND_CONSTANT(ATTRIB_FLAGBALL);
	BIND_CONSTANT(ATTRIB_CAPTURE_THE_FLAG);
	BIND_CONSTANT(ATTRIB_TEAM_DEATHMATCH);
	BIND_CONSTANT(ATTRIB_TEAM_KING_OF_THE_HILL);
	BIND_CONSTANT(ATTRIB_SEARCH_AND_DESTROY);
	BIND_CONSTANT(ATTRIB_GAME_MODE_MASK);
	BIND_CONSTANT(ITEM_ID_OFFSET);
	BIND_CONSTANT(PLAYER_START_ITEM_ID);
}

static_assert(MissionData::ITEM_ID_OFFSET == mission::kItemIdOffset);
static_assert(MissionData::PLAYER_START_ITEM_ID == 6001 + mission::kItemIdOffset);
// Every ATTRIB_* mirror is pinned to its engine home (engine/formats/mission
// bms.h AttribFlags) — a drifted copy here would silently mis-edit headers.
static_assert(MissionData::ATTRIB_FORCE_INDOORS ==
              static_cast<uint32_t>(bms::AttribFlags::ForceIndoors));
static_assert(MissionData::ATTRIB_ROTATE_MAP_180 ==
              static_cast<uint32_t>(bms::AttribFlags::RotateMap180));
static_assert(MissionData::ATTRIB_ENABLE_NVG ==
              static_cast<uint32_t>(bms::AttribFlags::EnableNVG));
static_assert(MissionData::ATTRIB_START_WITH_NVG_ON ==
              static_cast<uint32_t>(bms::AttribFlags::StartWithNVGOn));
static_assert(MissionData::ATTRIB_ADVANCE_AND_SECURE ==
              static_cast<uint32_t>(bms::AttribFlags::AdvanceAndSecure));
static_assert(MissionData::ATTRIB_CONQUER_AND_CONTROL ==
              static_cast<uint32_t>(bms::AttribFlags::ConquerAndControl));
static_assert(MissionData::ATTRIB_ATTACK_AND_DEFEND ==
              static_cast<uint32_t>(bms::AttribFlags::AttackAndDefend));
static_assert(MissionData::ATTRIB_COOP ==
              static_cast<uint32_t>(bms::AttribFlags::Coop));
static_assert(MissionData::ATTRIB_DEATHMATCH ==
              static_cast<uint32_t>(bms::AttribFlags::Deathmatch));
static_assert(MissionData::ATTRIB_KING_OF_THE_HILL ==
              static_cast<uint32_t>(bms::AttribFlags::KingOfTheHill));
static_assert(MissionData::ATTRIB_FLAGBALL ==
              static_cast<uint32_t>(bms::AttribFlags::FlagBall));
static_assert(MissionData::ATTRIB_CAPTURE_THE_FLAG ==
              static_cast<uint32_t>(bms::AttribFlags::CaptureTheFlag));
static_assert(MissionData::ATTRIB_TEAM_DEATHMATCH ==
              static_cast<uint32_t>(bms::AttribFlags::TeamDeathmatch));
static_assert(MissionData::ATTRIB_TEAM_KING_OF_THE_HILL ==
              static_cast<uint32_t>(bms::AttribFlags::TeamKingOfTheHill));
static_assert(MissionData::ATTRIB_SEARCH_AND_DESTROY ==
              static_cast<uint32_t>(bms::AttribFlags::SearchAndDestroy));
static_assert(MissionData::ATTRIB_GAME_MODE_MASK == bms::kGameModeMask);

bool MissionData::edit_failed(const std::string &error) {
	last_error = String(error.c_str());
	return false;
}

Error MissionData::open_file(const String &path) {
	source_path = path;
	last_error = String();
	mis_base_heights = PackedInt32Array(); // staged heights never apply to a different document
	file_ = {};
	loaded_ = false;
	header_only_ = false;
	const String ext = path.get_extension().to_lower();
	const std::string native_path(path.utf8().get_data());
	std::string error;
	bool ok = false;
	if (ext == "mis") {
		// .mis loads pass the empty items.def-TYPE resolver: MissionData holds no
		// ItemDatabase (the placer's is built after parse), so the pool kind is
		// unknowable here and every record lands in the generic item pool
		// (mission.h MisItemTypeResolver keeps that explicit).
		std::ifstream in(native_path, std::ios::binary);
		if (!in.good()) {
			error = "Cannot open MIS file: " + native_path;
		} else {
			const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			if (!in.good() && !in.eof()) {
				error = "Failed reading MIS file: " + native_path;
			} else if (text.empty()) {
				error = "No MIS text provided";
			} else {
				ok = mission::parse_mis_text(text, {}, file_, error);
			}
		}
	} else {
		ok = bms::parse_file(native_path, file_, error);
	}
	if (!ok) {
		file_ = {};
		last_error = String(error.c_str());
		return ERR_CANT_OPEN;
	}
	mission::sync_counts(file_);
	loaded_ = true;
	modified = false;
	return OK;
}

Error MissionData::create_default() {
	source_path = String();
	last_error = String();
	mis_base_heights = PackedInt32Array(); // staged heights never apply to a different document
	mission::make_default(file_);
	loaded_ = true;
	header_only_ = false;
	modified = false;
	return OK;
}

Error MissionData::open_wire_header(const PackedByteArray &p_header_bytes) {
	source_path = String();
	last_error = String();
	mis_base_heights = PackedInt32Array();
	file_ = {};
	loaded_ = false;
	header_only_ = false;
	// Only the exact header carried by retail S2C 0x0B: a read-only metadata
	// view for a network join, with no locally-authored entities, events, or
	// other BMS body sections.
	std::string error;
	if (!bms::parse_header_blob(p_header_bytes.ptr(), static_cast<size_t>(p_header_bytes.size()),
				file_.header, error)) {
		last_error = String(error.c_str());
		modified = false;
		return ERR_PARSE_ERROR;
	}
	loaded_ = true;
	header_only_ = true;
	modified = false;
	return OK;
}

Error MissionData::open_from_resource_root(const Ref<ResourceRoot> &p_resource_root,
		const String &p_name, int p_lookup_policy) {
	last_error = String();
	if (p_resource_root.is_null() || p_resource_root->get_root_dir().is_empty()) {
		last_error = "Resource root is not configured";
		return ERR_INVALID_PARAMETER;
	}
	const String file = p_name.get_file();
	if (file.is_empty()) {
		last_error = "Mission filename is empty";
		return ERR_INVALID_PARAMETER;
	}
	const PackedByteArray bytes = p_resource_root->read_file(
			file, static_cast<ResourceRoot::LookupPolicy>(p_lookup_policy));
	if (bytes.is_empty()) {
		last_error = "Mission file not found in resource root: " + file;
		return ERR_FILE_NOT_FOUND;
	}
	file_ = {};
	loaded_ = false;
	header_only_ = false;
	const String ext = file.get_extension().to_lower();
	std::string error;
	bool ok = false;
	if (ext == "mis") {
		const std::string text(reinterpret_cast<const char *>(bytes.ptr()), static_cast<size_t>(bytes.size()));
		// Empty items.def-TYPE resolver: same rationale as open_file above.
		ok = mission::parse_mis_text(text, {}, file_, error);
	} else {
		ok = bms::parse(bytes.ptr(), static_cast<size_t>(bytes.size()), file_, error);
	}
	if (!ok) {
		file_ = {};
		last_error = String(error.c_str());
		return ERR_CANT_OPEN;
	}
	mission::sync_counts(file_);
	loaded_ = true;
	source_path = file;
	modified = false;
	return OK;
}

bool MissionData::is_loaded() const {
	return loaded_;
}

bool MissionData::is_wire_header_only() const {
	return header_only_;
}

String MissionData::get_source_path() const {
	return source_path;
}

String MissionData::get_last_error() const {
	return last_error;
}

String MissionData::get_mission_name() const {
	return loaded_ ? String(mission::mission_info(file_).mission_name.c_str()) : String();
}

String MissionData::get_terrain_ref() const {
	return loaded_ ? String(mission::mission_info(file_).terrain.c_str()) : String();
}

String MissionData::get_environment_ref() const {
	return loaded_ ? String(mission::mission_info(file_).environment.c_str()) : String();
}

Ref<MissionInfo> MissionData::get_info() const {
	Ref<MissionInfo> out;
	out.instantiate();
	out->assign(loaded_ ? mission::mission_info(file_) : mission::MissionInfo{},
			static_cast<int>(get_game_mode()));
	return out;
}

Ref<MissionEnvironmentOverrides> MissionData::get_environment_overrides() const {
	const mission::MissionInfo info = loaded_ ? mission::mission_info(file_) : mission::MissionInfo{};
	Ref<MissionEnvironmentOverrides> out;
	out.instantiate();
	out->assign(opennova::env::bms_env_overrides_from_header(
			static_cast<uint32_t>(info.attrib_flags), info.water_override, info.fog_override,
			info.fog_color, info.water_color, info.water_murk));
	return out;
}

int MissionData::get_entity_count(EntityKind kind) const {
	if (!loaded_) return 0;
	return static_cast<int>(mission::entity_count(file_, to_native_kind(kind)));
}

TypedArray<MissionEntityRecord> MissionData::get_entities(EntityKind kind) const {
	TypedArray<MissionEntityRecord> out;
	if (!loaded_) return out;
	const mission::EntityKind native_kind = to_native_kind(kind);
	const std::vector<bms::Entity> *list = mission::entities(file_, native_kind);
	if (list == nullptr) return out;
	for (size_t i = 0; i < list->size(); ++i) {
		out.push_back(entity_record((*list)[i], native_kind, i));
	}
	return out;
}

Ref<MissionEntityRecord> MissionData::get_entity(EntityKind kind, int index) const {
	if (!loaded_ || index < 0) {
		return Ref<MissionEntityRecord>();
	}
	const mission::EntityKind native_kind = to_native_kind(kind);
	const std::vector<bms::Entity> *list = mission::entities(file_, native_kind);
	if (list == nullptr || static_cast<size_t>(index) >= list->size()) {
		return Ref<MissionEntityRecord>();
	}
	return entity_record((*list)[static_cast<size_t>(index)], native_kind, static_cast<size_t>(index));
}

TypedArray<MissionEntityRecord> MissionData::get_all_entities() const {
	TypedArray<MissionEntityRecord> out;
	const EntityKind kinds[] = { KIND_MARKER, KIND_ITEM, KIND_BUILDING, KIND_ORGANIC };
	for (EntityKind kind : kinds) {
		out.append_array(get_entities(kind));
	}
	return out;
}

bool MissionData::set_entity_transform(EntityKind kind, int index, const Vector3 &position, const Vector3 &rotation_deg) {
	if (!loaded_ || index < 0) {
		return false;
	}
	std::string error;
	if (!mission::set_entity_transform(file_, to_native_kind(kind), static_cast<size_t>(index),
				transform_from(position, rotation_deg), error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

bool MissionData::set_entity_property_int(EntityKind kind, int index, const String &property, int value) {
	if (!loaded_ || index < 0) {
		return false;
	}
	// The name->member mapping (and the clamp rules) lives in engine/formats/mission,
	// the same as the header setters; the wrapper just forwards the field name.
	// Adding an AI/waypoint field is one edit there, not four parallel ones across
	// this file and the inspector.
	std::string error;
	if (!mission::set_entity_property_int(file_, to_native_kind(kind), static_cast<size_t>(index),
	            property.utf8().get_data(), value, error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

bool MissionData::set_entity_property_string(EntityKind kind, int index, const String &property, const String &value) {
	if (!loaded_ || index < 0) {
		return false;
	}
	std::string error;
	if (!mission::set_entity_property_string(file_, to_native_kind(kind), static_cast<size_t>(index),
	            property.utf8().get_data(), value.utf8().get_data(), error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

bool MissionData::set_header_string(const String &field, const String &value) {
	if (!loaded_) return edit_failed("No mission loaded");
	std::string error;
	if (!mission::set_header_string(file_, field.utf8().get_data(), value.utf8().get_data(), error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

bool MissionData::set_header_int(const String &field, int value) {
	if (!loaded_) return edit_failed("No mission loaded");
	std::string error;
	if (!mission::set_header_int(file_, field.utf8().get_data(), value, error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

bool MissionData::set_header_flag(int bit, bool on) {
	if (!loaded_) return edit_failed("No mission loaded");
	mission::set_header_flag(file_, bit, on);
	modified = true;
	return true;
}

bool MissionData::set_header_float(const String &field, float value) {
	if (!loaded_) return edit_failed("No mission loaded");
	std::string error;
	if (!mission::set_header_float(file_, field.utf8().get_data(), value, error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

Ref<MissionEntityRecord> MissionData::add_entity(EntityKind kind, int item_id, const Vector3 &position, const Vector3 &rotation_deg) {
	if (!loaded_) {
		return Ref<MissionEntityRecord>();
	}
	const mission::EntityKind native_kind = to_native_kind(kind);
	const size_t index = mission::add_entity(file_, native_kind, item_id, transform_from(position, rotation_deg));
	modified = true;
	return entity_record((*mission::entities(file_, native_kind))[index], native_kind, index);
}

bool MissionData::remove_entity(EntityKind kind, int index) {
	if (!loaded_ || index < 0) {
		return false;
	}
	std::string error;
	if (!mission::remove_entity(file_, to_native_kind(kind), static_cast<size_t>(index), error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

TypedArray<MissionWaypointSummary> MissionData::get_waypoint_summaries() const {
	TypedArray<MissionWaypointSummary> out;
	if (!loaded_) return out;
	for (const mission::WaypointSummary &summary : mission::waypoint_summaries(file_)) {
		out.push_back(make_record<MissionWaypointSummary>(summary));
	}
	return out;
}

Ref<MissionWaypointPath> MissionData::get_waypoint_path(int index) const {
	if (!loaded_ || index < 0) {
		return Ref<MissionWaypointPath>();
	}
	mission::WaypointPath path;
	if (!mission::waypoint_path(file_, static_cast<size_t>(index), path)) {
		return Ref<MissionWaypointPath>();
	}
	return make_record<MissionWaypointPath>(path);
}

bool MissionData::set_waypoint_path(int index, const PackedInt32Array &marker_indices, int flags) {
	if (!loaded_ || index < 0) {
		return false;
	}
	std::vector<int> indices;
	indices.reserve(static_cast<size_t>(marker_indices.size()));
	for (int64_t i = 0; i < marker_indices.size(); ++i) {
		indices.push_back(marker_indices[i]);
	}
	std::string error;
	if (!mission::set_waypoint_path(file_, static_cast<size_t>(index), indices, flags, error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

bool MissionData::clear_waypoint_path(int index) {
	if (!loaded_ || index < 0) {
		return false;
	}
	std::string error;
	if (!mission::clear_waypoint_path(file_, static_cast<size_t>(index), error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

Ref<MissionWaypointMarker> MissionData::add_waypoint_marker(int path_index, int marker_item_id, const Vector3 &position, const Vector3 &rotation_deg, int insert_index) {
	if (!loaded_ || path_index < 0) {
		return Ref<MissionWaypointMarker>();
	}
	std::string error;
	size_t marker_index = 0;
	if (!mission::add_waypoint_marker(file_, static_cast<size_t>(path_index), marker_item_id,
				transform_from(position, rotation_deg), insert_index, error, &marker_index)) {
		edit_failed(error);
		return Ref<MissionWaypointMarker>();
	}
	modified = true;
	mission::WaypointPath path;
	(void)mission::waypoint_path(file_, static_cast<size_t>(path_index), path);
	Ref<MissionWaypointMarker> out;
	out.instantiate();
	out->assign(entity_record(file_.markers[marker_index], mission::EntityKind::Marker, marker_index),
			make_record<MissionWaypointPath>(path));
	return out;
}

int MissionData::get_area_trigger_count() const {
	return loaded_ ? static_cast<int>(file_.area_triggers.size()) : 0;
}

TypedArray<MissionAreaTrigger> MissionData::get_area_triggers() const {
	TypedArray<MissionAreaTrigger> out;
	if (!loaded_) return out;
	for (const mission::AreaTriggerRecord &record : mission::area_triggers(file_)) {
		out.push_back(make_record<MissionAreaTrigger>(record));
	}
	return out;
}

Ref<MissionAreaTrigger> MissionData::get_area_trigger(int index) const {
	if (!loaded_ || index < 0) {
		return Ref<MissionAreaTrigger>();
	}
	mission::AreaTriggerRecord record;
	if (!mission::area_trigger(file_, static_cast<size_t>(index), record)) {
		return Ref<MissionAreaTrigger>();
	}
	return make_record<MissionAreaTrigger>(record);
}

// Build a typed record from Godot-side corners, normalizing min<=max per axis (the engine does not
// auto-swap area triggers, so a crossed-corner drag must be fixed here). raw_flags is composed from
// the two known bits; any other flag bits start clear for a freshly authored zone.
static mission::AreaTriggerRecord make_area_record(const Vector3 &min_bounds, const Vector3 &max_bounds,
		bool active, bool constrain_z, int zone_id) {
	mission::AreaTriggerRecord record;
	record.wp_number = zone_id;
	record.min_x = MIN(min_bounds.x, max_bounds.x);
	record.max_x = MAX(min_bounds.x, max_bounds.x);
	record.min_y = MIN(min_bounds.y, max_bounds.y);
	record.max_y = MAX(min_bounds.y, max_bounds.y);
	record.min_z = MIN(min_bounds.z, max_bounds.z);
	record.max_z = MAX(min_bounds.z, max_bounds.z);
	record.active = active;
	record.constrain_z = constrain_z;
	record.reserved = (active ? 0x1 : 0) | (constrain_z ? 0x2 : 0);
	return record;
}

Ref<MissionAreaTrigger> MissionData::add_area_trigger(const Vector3 &min_bounds, const Vector3 &max_bounds, bool active, bool constrain_z, int zone_id) {
	if (!loaded_) {
		return Ref<MissionAreaTrigger>();
	}
	const size_t index = mission::add_area_trigger(file_,
			make_area_record(min_bounds, max_bounds, active, constrain_z, zone_id));
	modified = true;
	mission::AreaTriggerRecord out;
	(void)mission::area_trigger(file_, index, out);
	return make_record<MissionAreaTrigger>(out);
}

Ref<MissionAreaTrigger> MissionData::set_area_trigger(int index, const Vector3 &min_bounds, const Vector3 &max_bounds, bool active, bool constrain_z, int zone_id) {
	if (!loaded_ || index < 0) {
		return Ref<MissionAreaTrigger>();
	}
	// Preserve unknown flag bits across an edit: seed reserved from the existing record, then overwrite
	// only the two known bits below. A fresh make_area_record would otherwise zero them.
	mission::AreaTriggerRecord record = make_area_record(min_bounds, max_bounds, active, constrain_z, zone_id);
	mission::AreaTriggerRecord existing;
	if (mission::area_trigger(file_, static_cast<size_t>(index), existing)) {
		constexpr int kKnownBits = static_cast<int>(bms::AreaTrigger::kFlagMissionArea |
		                                            bms::AreaTrigger::kFlagConstrainZ);
		record.reserved = (existing.reserved & ~kKnownBits) |
		                  (active ? static_cast<int>(bms::AreaTrigger::kFlagMissionArea) : 0) |
		                  (constrain_z ? static_cast<int>(bms::AreaTrigger::kFlagConstrainZ) : 0);
	}
	std::string error;
	if (!mission::set_area_trigger(file_, static_cast<size_t>(index), record, error)) {
		edit_failed(error);
		return Ref<MissionAreaTrigger>();
	}
	modified = true;
	mission::AreaTriggerRecord out;
	(void)mission::area_trigger(file_, static_cast<size_t>(index), out);
	return make_record<MissionAreaTrigger>(out);
}

bool MissionData::remove_area_trigger(int index) {
	if (!loaded_ || index < 0) {
		return false;
	}
	std::string error;
	if (!mission::remove_area_trigger(file_, static_cast<size_t>(index), error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

TypedArray<MissionWeaponLoadoutEntry> MissionData::get_weapon_loadout() const {
	TypedArray<MissionWeaponLoadoutEntry> out;
	if (!loaded_) return out;
	const std::vector<mission::WeaponLoadoutEntry> entries = mission::weapon_loadout(file_);
	for (size_t i = 0; i < entries.size(); ++i) {
		Ref<MissionWeaponLoadoutEntry> record;
		record.instantiate();
		record->assign(entries[i], static_cast<int>(i));
		out.push_back(record);
	}
	return out;
}

bool MissionData::set_weapon_loadout(const TypedArray<MissionWeaponLoadoutEntry> &entries) {
	if (!loaded_) return edit_failed("No mission loaded");
	std::vector<mission::WeaponLoadoutEntry> records;
	records.reserve(static_cast<size_t>(entries.size()));
	for (int64_t i = 0; i < entries.size(); ++i) {
		const Ref<MissionWeaponLoadoutEntry> entry = entries[i];
		if (entry.is_null()) {
			return edit_failed("Weapon loadout entry is null");
		}
		records.push_back(entry->value());
	}
	std::string error;
	if (!mission::set_weapon_loadout(file_, records, error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

int MissionData::get_group_count() const {
	return loaded_ ? static_cast<int>(file_.group_records.size()) : 0;
}

TypedArray<MissionGroup> MissionData::get_groups() const {
	TypedArray<MissionGroup> out;
	if (!loaded_) return out;
	for (const mission::GroupFields &fields : mission::groups(file_)) {
		out.push_back(make_record<MissionGroup>(fields));
	}
	return out;
}

Ref<MissionGroup> MissionData::get_group(int index) const {
	if (!loaded_ || index < 0) {
		return Ref<MissionGroup>();
	}
	mission::GroupFields fields;
	if (!mission::group(file_, static_cast<size_t>(index), fields)) {
		return Ref<MissionGroup>();
	}
	return make_record<MissionGroup>(fields);
}

bool MissionData::set_group(int index, int field0, int field8, int field12) {
	if (!loaded_ || index < 0) {
		return false;
	}
	std::string error;
	if (!mission::set_group(file_, static_cast<size_t>(index), field0, field8, field12, error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

// --- Mission scripting (events / triggers / actions, Phase 4) ----------------

Ref<MissionEventChain> MissionData::event_chain_record(size_t event_index) const {
	mission::MissionEventChain chain;
	if (!mission::event_chain(file_, event_index, chain)) {
		return Ref<MissionEventChain>();
	}
	return make_record<MissionEventChain>(chain);
}

int MissionData::get_event_count() const {
	return loaded_ ? static_cast<int>(file_.events.size()) : 0;
}

TypedArray<MissionEvent> MissionData::get_events() const {
	TypedArray<MissionEvent> out;
	if (!loaded_) return out;
	for (const mission::MissionEventRecord &record : mission::events(file_)) {
		out.push_back(make_record<MissionEvent>(record));
	}
	return out;
}

Ref<MissionEvent> MissionData::get_event(int index) const {
	if (!loaded_ || index < 0) {
		return Ref<MissionEvent>();
	}
	mission::MissionEventRecord record;
	if (!mission::event(file_, static_cast<size_t>(index), record)) {
		return Ref<MissionEvent>();
	}
	return make_record<MissionEvent>(record);
}

Ref<MissionEventChain> MissionData::get_event_chain(int index) const {
	if (!loaded_ || index < 0) {
		return Ref<MissionEventChain>();
	}
	return event_chain_record(static_cast<size_t>(index));
}

Ref<MissionLogicSummary> MissionData::get_logic_summary() const {
	return make_record<MissionLogicSummary>(
			loaded_ ? mission::logic_summary(file_) : mission::MissionLogicSummary{});
}

Ref<MissionEvent> MissionData::add_event(int flags, int reset_after, int delay) {
	if (!loaded_) {
		return Ref<MissionEvent>();
	}
	mission::MissionEventRecord seed;
	seed.flags = flags;
	seed.reset_after = reset_after;
	seed.delay = delay;
	const size_t index = mission::add_event(file_, seed);
	modified = true;
	mission::MissionEventRecord out;
	(void)mission::event(file_, index, out);
	return make_record<MissionEvent>(out);
}

bool MissionData::remove_event(int index) {
	if (!loaded_ || index < 0) {
		return false;
	}
	std::string error;
	if (!mission::remove_event(file_, static_cast<size_t>(index), error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

bool MissionData::set_event(int index, int flags, int reset_after, int delay) {
	if (!loaded_ || index < 0) {
		return false;
	}
	// Seed from the existing event so the read-only structural fields (trigger/action index + count) survive:
	// set_event applies only the editable attributes below.
	mission::MissionEventRecord record;
	if (!mission::event(file_, static_cast<size_t>(index), record)) {
		return false;
	}
	// Preserve internal bits outside the three author-facing event flags. `record.flags` is seeded from
	// the existing on-disk event, so its complementary bits are exactly the ones to keep.
	const int exposed = static_cast<int>(bms::kEventAuthorFlagMask);
	record.flags = (record.flags & ~exposed) | (flags & exposed);
	record.reset_after = reset_after;
	record.delay = delay;
	std::string error;
	if (!mission::set_event(file_, static_cast<size_t>(index), record, error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

Ref<MissionEventChain> MissionData::add_event_trigger(int event_index, const Ref<MissionEventTrigger> &trigger) {
	if (!loaded_ || event_index < 0 || trigger.is_null()) {
		return Ref<MissionEventChain>();
	}
	mission::MissionEventRecord event;
	if (!mission::event(file_, static_cast<size_t>(event_index), event)) {
		return Ref<MissionEventChain>();
	}
	// A fresh trigger seeds as a Group / Null condition (a valid, named pairing) before the edit lands.
	mission::MissionTriggerRecord seed;
	seed.main_type = static_cast<int>(bms::TriggerMainType::Group);
	const mission::MissionTriggerRecord record = trigger_from_edit(trigger->value(), seed);
	std::string error;
	if (!mission::insert_event_trigger(file_, static_cast<size_t>(event_index),
				static_cast<size_t>(event.trigger_count), record, error)) {
		edit_failed(error);
		return Ref<MissionEventChain>();
	}
	modified = true;
	return event_chain_record(static_cast<size_t>(event_index));
}

Ref<MissionEventChain> MissionData::set_event_trigger(int event_index, int local_index, const Ref<MissionEventTrigger> &trigger) {
	if (!loaded_ || event_index < 0 || local_index < 0 || trigger.is_null()) {
		return Ref<MissionEventChain>();
	}
	mission::MissionEventRecord event;
	if (!mission::event(file_, static_cast<size_t>(event_index), event)) {
		return Ref<MissionEventChain>();
	}
	if (local_index >= event.trigger_count) {
		return Ref<MissionEventChain>();
	}
	const size_t global = static_cast<size_t>(event.trigger_index) + static_cast<size_t>(local_index);
	mission::MissionTriggerRecord existing;
	if (!mission::trigger(file_, global, existing)) {
		return Ref<MissionEventChain>();
	}
	const mission::MissionTriggerRecord record = trigger_from_edit(trigger->value(), existing);
	std::string error;
	if (!mission::set_trigger(file_, global, record, error)) {
		edit_failed(error);
		return Ref<MissionEventChain>();
	}
	modified = true;
	return event_chain_record(static_cast<size_t>(event_index));
}

bool MissionData::remove_event_trigger(int event_index, int local_index) {
	if (!loaded_ || event_index < 0 || local_index < 0) {
		return false;
	}
	std::string error;
	if (!mission::remove_event_trigger(file_, static_cast<size_t>(event_index), static_cast<size_t>(local_index), error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

bool MissionData::move_event_trigger(int event_index, int local_index, int delta) {
	if (!loaded_ || event_index < 0 || local_index < 0) {
		return false;
	}
	std::string error;
	if (!mission::move_event_trigger(file_, static_cast<size_t>(event_index), static_cast<size_t>(local_index), delta, error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

Ref<MissionEventChain> MissionData::add_event_action(int event_index, const Ref<MissionEventAction> &action) {
	if (!loaded_ || event_index < 0 || action.is_null()) {
		return Ref<MissionEventChain>();
	}
	mission::MissionEventRecord event;
	if (!mission::event(file_, static_cast<size_t>(event_index), event)) {
		return Ref<MissionEventChain>();
	}
	mission::MissionActionRecord seed;  // defaults to a Null action
	const mission::MissionActionRecord record = action_from_edit(action->value(), seed);
	std::string error;
	if (!mission::insert_event_action(file_, static_cast<size_t>(event_index),
				static_cast<size_t>(event.action_count), record, error)) {
		edit_failed(error);
		return Ref<MissionEventChain>();
	}
	modified = true;
	return event_chain_record(static_cast<size_t>(event_index));
}

bool MissionData::remove_event_action(int event_index, int local_index) {
	if (!loaded_ || event_index < 0 || local_index < 0) {
		return false;
	}
	std::string error;
	if (!mission::remove_event_action(file_, static_cast<size_t>(event_index), static_cast<size_t>(local_index), error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

bool MissionData::move_event_action(int event_index, int local_index, int delta) {
	if (!loaded_ || event_index < 0 || local_index < 0) {
		return false;
	}
	std::string error;
	if (!mission::move_event_action(file_, static_cast<size_t>(event_index), static_cast<size_t>(local_index), delta, error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

Error MissionData::save_file() {
	if (source_path.is_empty()) {
		// No path yet: let the shell route to Save As (matches the editor save contract).
		return ERR_INVALID_PARAMETER;
	}
	return save_as(source_path);
}

Error MissionData::save_as(const String &path) {
	const Error error = save_to_path(path);
	if (error == OK) {
		source_path = path;
		modified = false;
	}
	return error;
}

Error MissionData::save_to_path(const String &path) {
	last_error = String();
	if (path.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}
	if (!loaded_) {
		last_error = "No mission loaded";
		return ERR_FILE_CANT_WRITE;
	}
	if (header_only_) {
		last_error = "Wire BMS header is not a complete mission";
		return ERR_FILE_CANT_WRITE;
	}
	// Consume (and always clear) any staged base heights: they describe THIS save's entity
	// write order, so they must not survive onto a later save after the document changed.
	const PackedInt32Array staged_heights = mis_base_heights;
	mis_base_heights = PackedInt32Array();
	const std::string native_path(path.utf8().get_data());
	const String ext = path.get_extension().to_lower();
	std::string error;
	bool ok = false;
	mission::sync_counts(file_);
	if (ext == "mis") {
		// The .mis writer takes the editor-sampled terrain heights (flat, write order) and emits
		// them as each entity's extra_bheight next to the height_lock declaration; see
		// mission_mis.h write_mis_text and docs/mission/mis-format-re.md (D-MIS-4).
		std::vector<int32_t> base_heights;
		base_heights.reserve(static_cast<size_t>(staged_heights.size()));
		for (int i = 0; i < staged_heights.size(); ++i) {
			base_heights.push_back(staged_heights[i]);
		}
		std::string text;
		ok = mission::write_mis_text(file_, text, error,
				base_heights.empty() ? nullptr : &base_heights);
		if (ok) {
			std::ofstream out(native_path, std::ios::binary);
			if (!out.good()) {
				error = "Cannot create MIS file: " + native_path;
				ok = false;
			} else {
				out.write(text.data(), static_cast<std::streamsize>(text.size()));
				if (!out.good()) {
					error = "Failed writing MIS file: " + native_path;
					ok = false;
				}
			}
		}
	} else {
		ok = bms::write_file(file_, native_path, error);
	}
	if (!ok) {
		last_error = String(error.c_str());
		return ERR_FILE_CANT_WRITE;
	}
	return OK;
}

void MissionData::set_mis_base_heights(const PackedInt32Array &flat_write_order) {
	mis_base_heights = flat_write_order;
}

bool MissionData::is_modified() const {
	return modified;
}

int64_t MissionData::object_records_revision() const {
	// 64-bit FNV-1a over the raw bytes of the placed-object record vectors. This mirrors
	// how bms::equal decides these vectors (memcmp via pod_vectors_equal), so two
	// documents with byte-identical object records share a revision and any change moves
	// it -- far cheaper than marshalling ~every entity into a Dictionary to hash it.
	const bms::File &file = file_;
	uint64_t h = 1469598103934665603ull; // FNV-1a 64-bit offset basis
	const auto mix = [&h](const void *data, size_t size) {
		const unsigned char *p = static_cast<const unsigned char *>(data);
		for (size_t i = 0; i < size; ++i) {
			h ^= p[i];
			h *= 1099511628211ull; // FNV-1a 64-bit prime
		}
	};
	const auto mix_entities = [&](const std::vector<bms::Entity> &v) {
		const uint64_t count = v.size();
		mix(&count, sizeof(count)); // a count change moves the revision even at a byte realignment
		if (!v.empty()) {
			mix(v.data(), v.size() * sizeof(bms::Entity));
		}
	};
	mix_entities(file.items);
	mix_entities(file.buildings);
	mix_entities(file.markers);
	mix_entities(file.organics);
	return static_cast<int64_t>(h);
}

// Game mode is a single-select among the 11 attrib_flags mode bits. The decode
// priority, the mask, and the clear-then-OR encode live in the engine
// (engine/formats/mission bms.h selected_game_mode/set_game_mode — the
// witness cites ride there); this binding only marshals.
int64_t MissionData::get_game_mode() const {
	if (!loaded_) {
		return 0;
	}
	return static_cast<int64_t>(bms::selected_game_mode(file_.header.attrib_flags));
}

bool MissionData::set_game_mode(int64_t bit) {
	if (!loaded_) {
		return false;
	}
	if (!bms::set_game_mode(file_.header.attrib_flags,
				static_cast<uint32_t>(static_cast<uint64_t>(bit)))) {
		return false;
	}
	modified = true;
	return true;
}
