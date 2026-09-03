#include "mission/mission_data.h"

#include "resource_index/resource_root.h"
#include "env/mission_environment_overrides.h"
#include "mission/mission_info.h"

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
static_assert(MissionData::LOAD_STAGE_FINISH ==
		static_cast<int>(mission::MissionLoadStage::kFinish));
static_assert(MissionData::LOAD_STAGE_FINISH + 1 ==
		static_cast<int>(mission::MissionLoadStage::kCount));
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
	ClassDB::bind_method(D_METHOD("set_mis_base_heights", "flat_write_order"), &MissionData::set_mis_base_heights);
	ClassDB::bind_method(D_METHOD("is_modified"), &MissionData::is_modified);
	ClassDB::bind_method(D_METHOD("object_records_revision"), &MissionData::object_records_revision);
	ClassDB::bind_method(D_METHOD("structure_fingerprint"), &MissionData::structure_fingerprint);

	BIND_ENUM_CONSTANT(KIND_MARKER);
	BIND_ENUM_CONSTANT(KIND_ITEM);
	BIND_ENUM_CONSTANT(KIND_BUILDING);
	BIND_ENUM_CONSTANT(KIND_ORGANIC);
	BIND_CONSTANT(WP_FLAG_DOES_NOT_LOOP);
	BIND_CONSTANT(ATTRIB_FORCE_INDOORS);
	BIND_CONSTANT(LOAD_STAGE_ENVIRONMENT);
	BIND_CONSTANT(LOAD_STAGE_TERRAIN);
	BIND_CONSTANT(LOAD_STAGE_OBJECTS);
	BIND_CONSTANT(LOAD_STAGE_RUNTIME);
	BIND_CONSTANT(LOAD_STAGE_AUDIO);
	BIND_CONSTANT(LOAD_STAGE_EFFECTS);
	BIND_CONSTANT(LOAD_STAGE_FINISH);
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
}

static_assert(MissionData::ITEM_ID_OFFSET == mission::kItemIdOffset);
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

Dictionary MissionData::entity_to_dictionary(const bms::Entity &entity,
		mission::EntityKind kind, size_t index) const {
	const mission::EntityTransform transform = mission::entity_transform(entity);
	Dictionary out;
	out["kind"] = static_cast<int>(kind);
	out["index"] = static_cast<int>(index);
	// item_id is the items.def key (bms type_id + 100000); use it to look up the model.
	out["item_id"] = mission::entity_item_id(entity);
	out["type_id"] = entity.type_id;
	out["bms_id"] = entity.id;
	// Mission-space position (already converted from 16.16 fixed-point to float).
	out["position"] = Vector3(transform.x, transform.y, transform.z);
	// Euler degrees as authored; coordinate conversion to Godot space happens in
	// the placement layer where terrain context is available.
	out["rotation_deg"] = Vector3(transform.pitch, transform.yaw, transform.roll);
	out["group"] = entity.group_id;
	out["waypoint_id"] = entity.waypoint_id;
	out["wp_number"] = entity.wp_number;
	out["team"] = entity.team;
	out["ai_flags"] = static_cast<int>(entity.bmsi_attributes);
	out["perception"] = entity.perception2;
	out["accuracy"] = entity.w_accuracy1;
	out["alert_state"] = entity.alert_state;
	out["min_engagement_distance"] = entity.min_engagement_distance;
	out["max_engagement_distance"] = entity.max_engagement_distance;
	out["max_attack_distance"] = entity.max_attack_distance;
	out["spawn_count"] = entity.spawns;
	out["max_simultaneous"] = entity.no_more_than;  // = no_more_than (byte 74)
	out["no_less_than"] = entity.no_less_than;      // byte 75
	out["map_symbol"] = entity.map_symbol;          // byte 81
	out["name1"] = String(mission::entity_name1(entity).c_str()); // AI class (iai_name)
	out["name2"] = String(mission::entity_name2(entity).c_str()); // AI script (ai_textfile)
	return out;
}

int MissionData::get_entity_count(EntityKind kind) const {
	if (!loaded_) return 0;
	return static_cast<int>(mission::entity_count(file_, to_native_kind(kind)));
}

Array MissionData::get_entities(EntityKind kind) const {
	Array out;
	if (!loaded_) return out;
	const mission::EntityKind native_kind = to_native_kind(kind);
	const std::vector<bms::Entity> *list = mission::entities(file_, native_kind);
	if (list == nullptr) return out;
	for (size_t i = 0; i < list->size(); ++i) {
		out.push_back(entity_to_dictionary((*list)[i], native_kind, i));
	}
	return out;
}

Dictionary MissionData::get_entity(EntityKind kind, int index) const {
	if (!loaded_ || index < 0) {
		return Dictionary();
	}
	const mission::EntityKind native_kind = to_native_kind(kind);
	const std::vector<bms::Entity> *list = mission::entities(file_, native_kind);
	if (list == nullptr || static_cast<size_t>(index) >= list->size()) {
		return Dictionary();
	}
	return entity_to_dictionary((*list)[static_cast<size_t>(index)], native_kind, static_cast<size_t>(index));
}

Array MissionData::get_all_entities() const {
	Array out;
	const EntityKind kinds[] = { KIND_MARKER, KIND_ITEM, KIND_BUILDING, KIND_ORGANIC };
	for (EntityKind kind : kinds) {
		const Array entities = get_entities(kind);
		for (int i = 0; i < entities.size(); ++i) {
			out.push_back(entities[i]);
		}
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

Dictionary MissionData::add_entity(EntityKind kind, int item_id, const Vector3 &position, const Vector3 &rotation_deg) {
	if (!loaded_) {
		return Dictionary();
	}
	const mission::EntityKind native_kind = to_native_kind(kind);
	const size_t index = mission::add_entity(file_, native_kind, item_id, transform_from(position, rotation_deg));
	modified = true;
	return entity_to_dictionary((*mission::entities(file_, native_kind))[index], native_kind, index);
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

Dictionary MissionData::waypoint_path_to_dictionary(const mission::WaypointPath &path) const {
	Dictionary out;
	out["index"] = static_cast<int>(path.index);
	out["flags"] = path.flags;
	out["marker_count"] = static_cast<int>(path.marker_indices.size());
	PackedInt32Array indices;
	indices.resize(static_cast<int64_t>(path.marker_indices.size()));
	for (size_t i = 0; i < path.marker_indices.size(); ++i) {
		indices.set(static_cast<int64_t>(i), path.marker_indices[i]);
	}
	out["marker_indices"] = indices;
	return out;
}

Array MissionData::get_waypoint_summaries() const {
	Array out;
	if (!loaded_) return out;
	for (const mission::WaypointSummary &summary : mission::waypoint_summaries(file_)) {
		Dictionary d;
		d["index"] = static_cast<int>(summary.index);
		d["flags"] = summary.flags;
		d["marker_count"] = summary.marker_count;
		out.push_back(d);
	}
	return out;
}

Dictionary MissionData::get_waypoint_path(int index) const {
	if (!loaded_ || index < 0) {
		return Dictionary();
	}
	mission::WaypointPath path;
	if (!mission::waypoint_path(file_, static_cast<size_t>(index), path)) {
		return Dictionary();
	}
	return waypoint_path_to_dictionary(path);
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

Dictionary MissionData::add_waypoint_marker(int path_index, int marker_item_id, const Vector3 &position, const Vector3 &rotation_deg, int insert_index) {
	if (!loaded_ || path_index < 0) {
		return Dictionary();
	}
	std::string error;
	size_t marker_index = 0;
	if (!mission::add_waypoint_marker(file_, static_cast<size_t>(path_index), marker_item_id,
				transform_from(position, rotation_deg), insert_index, error, &marker_index)) {
		edit_failed(error);
		return Dictionary();
	}
	modified = true;
	mission::WaypointPath path;
	(void)mission::waypoint_path(file_, static_cast<size_t>(path_index), path);
	Dictionary out;
	out["marker"] = entity_to_dictionary(file_.markers[marker_index], mission::EntityKind::Marker, marker_index);
	out["path"] = waypoint_path_to_dictionary(path);
	return out;
}

Dictionary MissionData::area_trigger_to_dictionary(const mission::AreaTriggerRecord &record) const {
	Dictionary out;
	out["index"] = static_cast<int>(record.index);
	out["id"] = record.wp_number;  // off-0 dword (Phase-5 UNKNOWN; carried raw)
	// Mission-space corners (16.16 already converted to float). The placement layer maps to Godot space.
	out["min"] = Vector3(record.min_x, record.min_y, record.min_z);
	out["max"] = Vector3(record.max_x, record.max_y, record.max_z);
	out["active"] = record.active;
	out["constrain_z"] = record.constrain_z;
	out["raw_flags"] = record.reserved;
	return out;
}

int MissionData::get_area_trigger_count() const {
	return loaded_ ? static_cast<int>(file_.area_triggers.size()) : 0;
}

Array MissionData::get_area_triggers() const {
	Array out;
	if (!loaded_) return out;
	for (const mission::AreaTriggerRecord &record : mission::area_triggers(file_)) {
		out.push_back(area_trigger_to_dictionary(record));
	}
	return out;
}

Dictionary MissionData::get_area_trigger(int index) const {
	if (!loaded_ || index < 0) {
		return Dictionary();
	}
	mission::AreaTriggerRecord record;
	if (!mission::area_trigger(file_, static_cast<size_t>(index), record)) {
		return Dictionary();
	}
	return area_trigger_to_dictionary(record);
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

Dictionary MissionData::add_area_trigger(const Vector3 &min_bounds, const Vector3 &max_bounds, bool active, bool constrain_z, int zone_id) {
	if (!loaded_) {
		return Dictionary();
	}
	const size_t index = mission::add_area_trigger(file_,
			make_area_record(min_bounds, max_bounds, active, constrain_z, zone_id));
	modified = true;
	mission::AreaTriggerRecord out;
	(void)mission::area_trigger(file_, index, out);
	return area_trigger_to_dictionary(out);
}

Dictionary MissionData::set_area_trigger(int index, const Vector3 &min_bounds, const Vector3 &max_bounds, bool active, bool constrain_z, int zone_id) {
	if (!loaded_ || index < 0) {
		return Dictionary();
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
		return Dictionary();
	}
	modified = true;
	mission::AreaTriggerRecord out;
	(void)mission::area_trigger(file_, static_cast<size_t>(index), out);
	return area_trigger_to_dictionary(out);
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

Dictionary MissionData::weapon_loadout_to_dictionary(const mission::WeaponLoadoutEntry &entry, int index) const {
	Dictionary out;
	out["index"] = index;
	out["name"] = String::utf8(entry.name.c_str());
	out["ammo_primary"] = String::utf8(entry.ammo_primary.c_str());
	out["ammo_secondary"] = String::utf8(entry.ammo_secondary.c_str());
	out["flags"] = String::utf8(entry.flags.c_str());
	return out;
}

Array MissionData::get_weapon_loadout() const {
	Array out;
	if (!loaded_) return out;
	const std::vector<mission::WeaponLoadoutEntry> entries = mission::weapon_loadout(file_);
	for (size_t i = 0; i < entries.size(); ++i) {
		out.push_back(weapon_loadout_to_dictionary(entries[i], static_cast<int>(i)));
	}
	return out;
}

bool MissionData::set_weapon_loadout(const Array &entries) {
	if (!loaded_) return edit_failed("No mission loaded");
	std::vector<mission::WeaponLoadoutEntry> records;
	records.reserve(entries.size());
	for (int i = 0; i < entries.size(); ++i) {
		const Dictionary dict = entries[i];
		mission::WeaponLoadoutEntry record;
		record.name = String(dict.get("name", "")).utf8().get_data();
		// All three numeric strings default to "-1" when a caller omits them. flags is the
		// load-bearing per-ammo damage class used by the runtime loadout builder.
		record.ammo_primary = String(dict.get("ammo_primary", "-1")).utf8().get_data();
		record.ammo_secondary = String(dict.get("ammo_secondary", "-1")).utf8().get_data();
		record.flags = String(dict.get("flags", "-1")).utf8().get_data();
		records.push_back(std::move(record));
	}
	std::string error;
	if (!mission::set_weapon_loadout(file_, records, error)) {
		return edit_failed(error);
	}
	modified = true;
	return true;
}

Dictionary MissionData::group_to_dictionary(const mission::GroupFields &fields) const {
	Dictionary out;
	out["index"] = static_cast<int>(fields.index);
	out["field0"] = fields.field0;
	out["field8"] = fields.field8;
	out["field12"] = fields.field12;
	return out;
}

int MissionData::get_group_count() const {
	return loaded_ ? static_cast<int>(file_.group_records.size()) : 0;
}

Array MissionData::get_groups() const {
	Array out;
	if (!loaded_) return out;
	for (const mission::GroupFields &fields : mission::groups(file_)) {
		out.push_back(group_to_dictionary(fields));
	}
	return out;
}

Dictionary MissionData::get_group(int index) const {
	if (!loaded_ || index < 0) {
		return Dictionary();
	}
	mission::GroupFields fields;
	if (!mission::group(file_, static_cast<size_t>(index), fields)) {
		return Dictionary();
	}
	return group_to_dictionary(fields);
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

Dictionary MissionData::event_to_dictionary(const mission::MissionEventRecord &record) const {
	Dictionary out;
	out["index"] = static_cast<int>(record.index);
	out["flags"] = record.flags;
	out["trigger_index"] = record.trigger_index;
	out["action_index"] = record.action_index;
	out["trigger_count"] = record.trigger_count;
	out["action_count"] = record.action_count;
	out["reset_after"] = record.reset_after;
	out["delay"] = record.delay;
	out["unknown5"] = record.unknown5;
	out["unknown6"] = record.unknown6;
	return out;
}

Dictionary MissionData::trigger_to_dictionary(const mission::MissionTriggerRecord &record) const {
	Dictionary out;
	out["index"] = static_cast<int>(record.index);
	out["condition_flags"] = record.condition_flags;
	out["main_type"] = record.main_type;
	out["main_type_name"] = String::utf8(record.main_type_name.c_str());
	out["sub_type"] = record.sub_type;
	out["sub_type_name"] = String::utf8(record.sub_type_name.c_str());
	out["param1"] = record.param1;
	out["param2"] = record.param2;
	out["param3"] = record.param3;
	out["param4"] = record.param4;
	out["unknown7"] = record.unknown7;
	out["negated"] = record.negated;
	out["logic_or"] = record.logic_or;
	out["logic_xor"] = record.logic_xor;
	out["logic_operator"] = String::utf8(record.logic_operator.c_str());
	return out;
}

Dictionary MissionData::action_to_dictionary(const mission::MissionActionRecord &record) const {
	Dictionary out;
	out["index"] = static_cast<int>(record.index);
	out["action_type"] = record.action_type;
	out["action_type_name"] = String::utf8(record.action_type_name.c_str());
	out["action_sub_type"] = record.action_sub_type;
	out["action_sub_type_name"] = String::utf8(record.action_sub_type_name.c_str());
	out["param1"] = record.param1;
	out["param2"] = record.param2;
	out["param3"] = record.param3;
	out["param4"] = record.param4;
	out["reserved0"] = record.reserved0;
	out["reserved1"] = record.reserved1;
	return out;
}

Dictionary MissionData::logic_reference_to_dictionary(const mission::MissionLogicReference &reference) const {
	Dictionary out;
	out["source_kind"] = String::utf8(reference.source_kind.c_str());
	out["source_index"] = reference.source_index;
	out["target_kind"] = String::utf8(reference.target_kind.c_str());
	out["target_index"] = reference.target_index;
	out["param_slot"] = reference.param_slot;
	out["raw_value"] = reference.raw_value;
	out["label"] = String::utf8(reference.label.c_str());
	out["valid"] = reference.valid;
	return out;
}

Dictionary MissionData::logic_diagnostic_to_dictionary(const mission::MissionLogicDiagnostic &diagnostic) const {
	Dictionary out;
	out["severity"] = String::utf8(diagnostic.severity.c_str());
	out["code"] = String::utf8(diagnostic.code.c_str());
	out["message"] = String::utf8(diagnostic.message.c_str());
	out["subject_kind"] = String::utf8(diagnostic.subject_kind.c_str());
	out["subject_index"] = diagnostic.subject_index;
	return out;
}

Dictionary MissionData::event_chain_to_dictionary(const mission::MissionEventChain &chain) const {
	Dictionary out;
	out["event"] = event_to_dictionary(chain.event);
	Array triggers;
	for (const mission::MissionTriggerRecord &trigger : chain.triggers) {
		triggers.push_back(trigger_to_dictionary(trigger));
	}
	out["triggers"] = triggers;
	Array actions;
	for (const mission::MissionActionRecord &action : chain.actions) {
		actions.push_back(action_to_dictionary(action));
	}
	out["actions"] = actions;
	Array references;
	for (const mission::MissionLogicReference &reference : chain.references) {
		references.push_back(logic_reference_to_dictionary(reference));
	}
	out["references"] = references;
	Array diagnostics;
	for (const mission::MissionLogicDiagnostic &diagnostic : chain.diagnostics) {
		diagnostics.push_back(logic_diagnostic_to_dictionary(diagnostic));
	}
	out["diagnostics"] = diagnostics;
	return out;
}

mission::MissionTriggerRecord MissionData::trigger_from_dictionary(const Dictionary &dict, const mission::MissionTriggerRecord &seed) const {
	mission::MissionTriggerRecord record = seed;
	record.main_type = static_cast<int>(dict.get("main_type", seed.main_type));
	record.sub_type = static_cast<int>(dict.get("sub_type", seed.sub_type));
	record.param1 = static_cast<int>(dict.get("param1", seed.param1));
	record.param2 = static_cast<int>(dict.get("param2", seed.param2));
	record.param3 = static_cast<int>(dict.get("param3", seed.param3));
	record.param4 = static_cast<int>(dict.get("param4", seed.param4));
	// Compose condition_flags bits 0/1/2 from the editor booleans; keep the seed's unmodeled high bits so
	// an edit never drops a flag the format carries but the editor does not surface. unknown7 rides on the
	// `record = seed` copy. Defaults for omitted keys come from the condition_flags bits (the canonical
	// source: condition_flags is what trigger_from_record serializes), not the seed's mirror bool fields,
	// so an out-of-sync seed can never propagate. The bool mirrors are then re-derived to stay consistent.
	using bms::Trigger;
	constexpr int kConditionMask = Trigger::kConditionNegated | Trigger::kConditionOr | Trigger::kConditionXor;
	int condition = seed.condition_flags & ~kConditionMask;
	if (static_cast<bool>(dict.get("negated", (seed.condition_flags & Trigger::kConditionNegated) != 0))) {
		condition |= Trigger::kConditionNegated;
	}
	if (static_cast<bool>(dict.get("logic_or", (seed.condition_flags & Trigger::kConditionOr) != 0))) {
		condition |= Trigger::kConditionOr;
	}
	if (static_cast<bool>(dict.get("logic_xor", (seed.condition_flags & Trigger::kConditionXor) != 0))) {
		condition |= Trigger::kConditionXor;
	}
	record.condition_flags = condition;
	record.negated = (condition & Trigger::kConditionNegated) != 0;
	record.logic_or = (condition & Trigger::kConditionOr) != 0;
	record.logic_xor = (condition & Trigger::kConditionXor) != 0;
	return record;
}

mission::MissionActionRecord MissionData::action_from_dictionary(const Dictionary &dict, const mission::MissionActionRecord &seed) const {
	mission::MissionActionRecord record = seed;
	record.action_type = static_cast<int>(dict.get("action_type", seed.action_type));
	record.action_sub_type = static_cast<int>(dict.get("action_sub_type", seed.action_sub_type));
	record.param1 = static_cast<int>(dict.get("param1", seed.param1));
	record.param2 = static_cast<int>(dict.get("param2", seed.param2));
	record.param3 = static_cast<int>(dict.get("param3", seed.param3));
	record.param4 = static_cast<int>(dict.get("param4", seed.param4));
	return record;  // reserved0/reserved1 ride on the `record = seed` copy
}

int MissionData::get_event_count() const {
	return loaded_ ? static_cast<int>(file_.events.size()) : 0;
}

Array MissionData::get_events() const {
	Array out;
	if (!loaded_) return out;
	for (const mission::MissionEventRecord &record : mission::events(file_)) {
		out.push_back(event_to_dictionary(record));
	}
	return out;
}

Dictionary MissionData::get_event(int index) const {
	if (!loaded_ || index < 0) {
		return Dictionary();
	}
	mission::MissionEventRecord record;
	if (!mission::event(file_, static_cast<size_t>(index), record)) {
		return Dictionary();
	}
	return event_to_dictionary(record);
}

Dictionary MissionData::get_event_chain(int index) const {
	if (!loaded_ || index < 0) {
		return Dictionary();
	}
	mission::MissionEventChain chain;
	if (!mission::event_chain(file_, static_cast<size_t>(index), chain)) {
		return Dictionary();
	}
	return event_chain_to_dictionary(chain);
}

Dictionary MissionData::get_logic_summary() const {
	const mission::MissionLogicSummary summary =
			loaded_ ? mission::logic_summary(file_) : mission::MissionLogicSummary{};
	Dictionary out;
	out["events"] = static_cast<int>(summary.event_count);
	out["triggers"] = static_cast<int>(summary.trigger_count);
	out["actions"] = static_cast<int>(summary.action_count);
	out["area_triggers"] = static_cast<int>(summary.area_trigger_count);
	out["diagnostics"] = static_cast<int>(summary.diagnostic_count);
	return out;
}

Dictionary MissionData::add_event(int flags, int reset_after, int delay) {
	if (!loaded_) {
		return Dictionary();
	}
	mission::MissionEventRecord seed;
	seed.flags = flags;
	seed.reset_after = reset_after;
	seed.delay = delay;
	const size_t index = mission::add_event(file_, seed);
	modified = true;
	mission::MissionEventRecord out;
	(void)mission::event(file_, index, out);
	return event_to_dictionary(out);
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

Dictionary MissionData::add_event_trigger(int event_index, const Dictionary &trigger) {
	if (!loaded_ || event_index < 0) {
		return Dictionary();
	}
	mission::MissionEventRecord event;
	if (!mission::event(file_, static_cast<size_t>(event_index), event)) {
		return Dictionary();
	}
	// A fresh trigger defaults to a Group / Null condition (a valid, named pairing) before the dict edits.
	mission::MissionTriggerRecord seed;
	seed.main_type = static_cast<int>(bms::TriggerMainType::Group);
	const mission::MissionTriggerRecord record = trigger_from_dictionary(trigger, seed);
	std::string error;
	if (!mission::insert_event_trigger(file_, static_cast<size_t>(event_index),
				static_cast<size_t>(event.trigger_count), record, error)) {
		edit_failed(error);
		return Dictionary();
	}
	modified = true;
	mission::MissionEventChain chain;
	(void)mission::event_chain(file_, static_cast<size_t>(event_index), chain);
	return event_chain_to_dictionary(chain);
}

Dictionary MissionData::set_event_trigger(int event_index, int local_index, const Dictionary &trigger) {
	if (!loaded_ || event_index < 0 || local_index < 0) {
		return Dictionary();
	}
	mission::MissionEventRecord event;
	if (!mission::event(file_, static_cast<size_t>(event_index), event)) {
		return Dictionary();
	}
	if (local_index >= event.trigger_count) {
		return Dictionary();
	}
	const size_t global = static_cast<size_t>(event.trigger_index) + static_cast<size_t>(local_index);
	mission::MissionTriggerRecord existing;
	if (!mission::trigger(file_, global, existing)) {
		return Dictionary();
	}
	const mission::MissionTriggerRecord record = trigger_from_dictionary(trigger, existing);
	std::string error;
	if (!mission::set_trigger(file_, global, record, error)) {
		edit_failed(error);
		return Dictionary();
	}
	modified = true;
	mission::MissionEventChain chain;
	(void)mission::event_chain(file_, static_cast<size_t>(event_index), chain);
	return event_chain_to_dictionary(chain);
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

Dictionary MissionData::add_event_action(int event_index, const Dictionary &action) {
	if (!loaded_ || event_index < 0) {
		return Dictionary();
	}
	mission::MissionEventRecord event;
	if (!mission::event(file_, static_cast<size_t>(event_index), event)) {
		return Dictionary();
	}
	mission::MissionActionRecord seed;  // defaults to a Null action
	const mission::MissionActionRecord record = action_from_dictionary(action, seed);
	std::string error;
	if (!mission::insert_event_action(file_, static_cast<size_t>(event_index),
				static_cast<size_t>(event.action_count), record, error)) {
		edit_failed(error);
		return Dictionary();
	}
	modified = true;
	mission::MissionEventChain chain;
	(void)mission::event_chain(file_, static_cast<size_t>(event_index), chain);
	return event_chain_to_dictionary(chain);
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
	source_path = path;
	modified = false;
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

Dictionary MissionData::structure_fingerprint() const {
	Dictionary out;
	out["events"] = get_event_count();
	out["zones"] = get_area_trigger_count();
	out["object_rev"] = object_records_revision();
	return out;
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
