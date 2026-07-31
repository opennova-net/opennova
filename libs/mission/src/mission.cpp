#include "mission/mission.h"

// MissionDocument: the editable in-memory mission and every operation the editor
// performs on it. The .mis text form, the enum name tables, the typed-record
// conversions and the flat C ABI each live in their own TU beside this one
// (quality campaign W3-1); this file is the document itself.

#include "mission_detail.h"
#include "mission_mis.h"
#include "mission_names.h"
#include "mission_records.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace opennova::mission {

using namespace detail; // the shared primitives, unqualified as before

struct MissionDocument::Impl {
	bms::File file;
	std::string source_path;
	std::string last_error;
	bool loaded = false;
};

MissionDocument::MissionDocument() : impl_(std::make_unique<Impl>()) {}
MissionDocument::~MissionDocument() = default;
MissionDocument::MissionDocument(MissionDocument &&) noexcept = default;
MissionDocument &MissionDocument::operator=(MissionDocument &&) noexcept = default;

bool MissionDocument::load_bms_file(const std::string &path) {
	clear();
	std::string error;
	if (!bms::parse_file(path, impl_->file, error)) {
		impl_->last_error = error;
		return false;
	}
	impl_->source_path = path;
	impl_->loaded = true;
	sync_counts();
	return true;
}

bool MissionDocument::load_bms_bytes(const uint8_t *data, size_t size) {
	clear();
	if (data == nullptr || size == 0) {
		impl_->last_error = "No BMS bytes provided";
		return false;
	}
	std::string error;
	if (!bms::parse(data, size, impl_->file, error)) {
		impl_->last_error = error;
		return false;
	}
	impl_->loaded = true;
	sync_counts();
	return true;
}

bool MissionDocument::load_mis_file(const std::string &path) {
	clear();
	if (path.empty()) {
		impl_->last_error = "No MIS path provided";
		return false;
	}
	std::ifstream file(path, std::ios::binary);
	if (!file.good()) {
		impl_->last_error = "Cannot open MIS file: " + path;
		return false;
	}
	std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	if (!file.good() && !file.eof()) {
		impl_->last_error = "Failed reading MIS file: " + path;
		return false;
	}
	if (!load_mis_text(text)) {
		return false;
	}
	impl_->source_path = path;
	return true;
}

bool MissionDocument::load_mis_text(const std::string &text) {
	clear();
	if (text.empty()) {
		impl_->last_error = "No MIS text provided";
		return false;
	}
	std::string error;
	if (!parse_mis_text_to_bms(text, impl_->file, error)) {
		impl_->last_error = error;
		return false;
	}
	impl_->loaded = true;
	sync_counts();
	return true;
}

bool MissionDocument::save_bms_file(const std::string &path) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (path.empty()) {
		impl_->last_error = "No output path provided";
		return false;
	}
	sync_counts();
	std::string error;
	if (!bms::write_file(impl_->file, path, error)) {
		impl_->last_error = error;
		return false;
	}
	impl_->source_path = path;
	return true;
}

bool MissionDocument::write_bms_bytes(std::vector<uint8_t> &out) {
	out.clear();
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	sync_counts();
	std::string error;
	if (!bms::write(impl_->file, out, error)) {
		impl_->last_error = error;
		return false;
	}
	return true;
}

bool MissionDocument::save_mis_file(const std::string &path, const std::vector<int32_t> *base_heights) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (path.empty()) {
		impl_->last_error = "No output path provided";
		return false;
	}
	std::string text;
	if (!write_mis_text(text, base_heights)) {
		return false;
	}
	std::ofstream file(path, std::ios::binary);
	if (!file.good()) {
		impl_->last_error = "Cannot create MIS file: " + path;
		return false;
	}
	file.write(text.data(), static_cast<std::streamsize>(text.size()));
	if (!file.good()) {
		impl_->last_error = "Failed writing MIS file: " + path;
		return false;
	}
	impl_->source_path = path;
	return true;
}

bool MissionDocument::write_mis_text(std::string &out, const std::vector<int32_t> *base_heights) {
	out.clear();
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	sync_counts();
	std::string error;
	if (!opennova::mission::write_mis_text(impl_->file, out, error, base_heights)) {
		impl_->last_error = error;
		return false;
	}
	return true;
}

void MissionDocument::clear() {
	impl_->file = {};
	impl_->source_path.clear();
	impl_->last_error.clear();
	impl_->loaded = false;
}

void MissionDocument::create_default() {
	// Build a minimal, valid, empty mission in memory (no file backing). The only header
	// field the format requires is the magic + version (parse gates magic == "BMS" and the
	// version byte >= kMinVersion); every other field round-trips fine at zero, and write()
	// recomputes no positional offsets. sync_counts() then backfills the fixed
	// waypoint/group/layer tables (and waypoint padding) so write_bms_bytes() produces a
	// buffer parse() accepts.
	clear();
	bms::Header &header = impl_->file.header;
	header.magic[0] = 'B';
	header.magic[1] = 'M';
	header.magic[2] = 'S';
	header.magic[3] = static_cast<char>(bms::kMinVersion);
	impl_->loaded = true;
	sync_counts();
}

bool MissionDocument::is_loaded() const {
	return impl_->loaded;
}

const std::string &MissionDocument::source_path() const {
	return impl_->source_path;
}

const std::string &MissionDocument::last_error() const {
	return impl_->last_error;
}

MissionInfo MissionDocument::info() const {
	MissionInfo out;
	if (!impl_->loaded) {
		return out;
	}
	const bms::Header &header = impl_->file.header;
	out.mission_name = fixed_string(header.mission_name, sizeof(header.mission_name));
	out.designer = fixed_string(header.designer, sizeof(header.designer));
	out.briefing = fixed_string(header.mission_briefing, sizeof(header.mission_briefing));
	// terrain[48] packs three 16-byte slots (terrain / cnv_file / tt_file); bound the read to the
	// first slot so a full 16-char terrain name does not bleed into cnv_file.
	out.terrain = fixed_string(header.terrain, 16);
	out.environment = fixed_string(header.environment, sizeof(header.environment));
	out.climate = static_cast<int>(header.climate);
	out.weather = static_cast<int>(header.weather_type);
	out.mission_type = static_cast<int>(header.mission_type);
	out.attrib_flags = static_cast<int>(header.attrib_flags);
	out.start_time = header.start_time;
	out.minutes_per_day = header.minutes_per_day;
	out.player_health = static_cast<int>(header.health);
	out.max_saves = header.max_saves;
	out.music = static_cast<int>(header.music);
	out.reverb = static_cast<int>(header.reverb);
	out.wind_speed = static_cast<int>(header.wind_speed);
	out.wind_direction = static_cast<int>(header.wind_direction);
	out.map_zoom = header.map_zoom;
	out.water_override = static_cast<int16_t>(header.water_override);
	out.fog_override = static_cast<int>(header.fog_override);
	out.fog_color[0] = header.fog_color[0];
	out.fog_color[1] = header.fog_color[1];
	out.fog_color[2] = header.fog_color[2];
	out.water_color[0] = header.water_color[0];
	out.water_color[1] = header.water_color[1];
	out.water_color[2] = header.water_color[2];
	out.water_murk = header.murk;
	return out;
}

bool MissionDocument::set_header_string(const std::string &field, const std::string &value) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	bms::Header &header = impl_->file.header;
	if (field == "mission_name") {
		// These are fixed-width on-disk slots read back at full width via fixed_string(., sizeof)
		// (see info()), so use copy_fixed_field, not copy_cstr: a name/designer/briefing that fills
		// every byte would otherwise lose its last byte to a forced NUL and break the byte-exact
		// round-trip, exactly the truncation the terrain / environment / name1 / name2 fields below
		// (and apply_properties) already avoid.
		copy_fixed_field(header.mission_name, sizeof(header.mission_name), value);
	} else if (field == "designer") {
		copy_fixed_field(header.designer, sizeof(header.designer), value);
	} else if (field == "briefing") {
		copy_fixed_field(header.mission_briefing, sizeof(header.mission_briefing), value);
	} else if (field == "terrain") {
		// header.terrain[48] is three 16-byte fixed slots: terrain@+0, cnv_file@+16, tt_file@+32
		// (see mission_mis_writer.cpp's write_mis_general_information). Write only the first slot so a terrain edit does not
		// zero-fill (and lose) the cnv_file / tt_file references. copy_fixed_field (not copy_cstr)
		// keeps all 16 bytes: a slot a shipped mission fills completely would otherwise lose its
		// 16th byte to a forced NUL, mirroring the name1/name2 fix in apply_properties. The
		// inspector / get_terrain reads are bounded to 16 so a full slot never bleeds into cnv_file.
		copy_fixed_field(header.terrain, 16, value);
	} else if (field == "environment") {
		// environment[16] is a standalone fixed slot read back with fixed_string(.,16); copy_fixed_field
		// preserves a full 16-char name (copy_cstr would force a NUL into byte 15 and truncate it).
		copy_fixed_field(header.environment, sizeof(header.environment), value);
	} else {
		impl_->last_error = "Unknown header string field: " + field;
		return false;
	}
	return true;
}

bool MissionDocument::set_header_int(const std::string &field, int value) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	bms::Header &header = impl_->file.header;
	if (field == "climate") {
		header.climate = static_cast<bms::ClimateType>(value);
	} else if (field == "weather") {
		header.weather_type = static_cast<bms::WeatherType>(value);
	} else if (field == "mission_type") {
		header.mission_type = static_cast<bms::MissionType>(static_cast<uint8_t>(value));
	} else if (field == "attrib_flags") {
		header.attrib_flags = static_cast<bms::AttribFlags>(static_cast<uint32_t>(value));
	} else if (field == "start_time") {
		header.start_time = static_cast<uint16_t>(value);
	} else if (field == "minutes_per_day") {
		header.minutes_per_day = static_cast<uint16_t>(value);
	} else if (field == "player_health") {
		header.health = static_cast<uint32_t>(value);
	} else if (field == "max_saves") {
		header.max_saves = static_cast<uint8_t>(value);
	} else if (field == "music") {
		header.music = static_cast<uint32_t>(value);
	} else if (field == "reverb") {
		header.reverb = static_cast<uint32_t>(value);
	} else if (field == "wind_speed") {
		header.wind_speed = static_cast<uint32_t>(value);
	} else if (field == "wind_direction") {
		header.wind_direction = static_cast<uint32_t>(value);
	} else if (field == "water_override") {
		// s16 half-world-units; only takes effect when attrib_flags WaterOverrideEnable (0x1) is set.
		header.water_override = static_cast<uint16_t>(value);
	} else if (field == "fog_override") {
		// fog distance in world units; only takes effect when attrib_flags FogDistanceOverrideEnable (0x2) is set.
		header.fog_override = static_cast<uint16_t>(value);
	} else {
		impl_->last_error = "Unknown header int field: " + field;
		return false;
	}
	return true;
}

bool MissionDocument::set_header_flag(int bit, bool on) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	uint32_t flags = static_cast<uint32_t>(impl_->file.header.attrib_flags);
	if (on) {
		flags |= static_cast<uint32_t>(bit);
	} else {
		flags &= ~static_cast<uint32_t>(bit);
	}
	impl_->file.header.attrib_flags = static_cast<bms::AttribFlags>(flags);
	return true;
}

bool MissionDocument::set_header_float(const std::string &field, float value) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (field == "map_zoom") {
		impl_->file.header.map_zoom = value;
	} else {
		impl_->last_error = "Unknown header float field: " + field;
		return false;
	}
	return true;
}

size_t MissionDocument::entity_count(EntityKind kind) const {
	if (!impl_->loaded) {
		return 0;
	}
	const std::vector<bms::Entity> *entities = entities_for(impl_->file, kind);
	return entities ? entities->size() : 0;
}

bool MissionDocument::get_entity(EntityKind kind, size_t index, EntityRecord &out) const {
	if (!impl_->loaded) {
		return false;
	}
	const std::vector<bms::Entity> *entities = entities_for(impl_->file, kind);
	if (entities == nullptr || index >= entities->size()) {
		return false;
	}
	out = to_record((*entities)[index], kind, index);
	return true;
}

bool MissionDocument::set_entity_transform(EntityKind kind, size_t index, const EntityTransform &transform) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	std::vector<bms::Entity> *entities = entities_for(impl_->file, kind);
	if (entities == nullptr || index >= entities->size()) {
		impl_->last_error = "Mission entity index out of range";
		return false;
	}
	apply_transform((*entities)[index], transform);
	return true;
}

bool MissionDocument::set_entity_properties(EntityKind kind, size_t index, const EntityProperties &properties, EntityRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	std::vector<bms::Entity> *entities = entities_for(impl_->file, kind);
	if (entities == nullptr || index >= entities->size()) {
		impl_->last_error = "Mission entity index out of range";
		return false;
	}
	if ((static_cast<uint32_t>(properties.ai_flags) & ~kKnownAiAttributeMask) != 0) {
		impl_->last_error = "Mission entity AI flags include unsupported bits";
		return false;
	}
	apply_properties((*entities)[index], properties);
	if (out != nullptr) {
		*out = to_record((*entities)[index], kind, index);
	}
	return true;
}

// Edit one named int property of an entity, mirroring set_header_int: the name->member mapping lives
// here (kEntityIntFields) instead of in each caller. Seeds the full property set from the record,
// changes only the requested member, and reuses set_entity_properties (which owns the clamp rules).
bool MissionDocument::set_entity_property_int(EntityKind kind, size_t index, const std::string &name, int value) {
	EntityRecord record;
	if (!get_entity(kind, index, record)) {
		impl_->last_error = "Mission entity index out of range";
		return false;
	}
	EntityProperties properties = properties_from_record(record);
	for (const EntityIntField &field : kEntityIntFields) {
		if (name == field.name) {
			properties.*(field.member) = value;
			return set_entity_properties(kind, index, properties, nullptr);
		}
	}
	impl_->last_error = "Unknown entity int property: " + name;
	return false;
}

// Edit one named string property (name1 / name2) of an entity. Same shape as set_entity_property_int.
bool MissionDocument::set_entity_property_string(EntityKind kind, size_t index, const std::string &name, const std::string &value) {
	EntityRecord record;
	if (!get_entity(kind, index, record)) {
		impl_->last_error = "Mission entity index out of range";
		return false;
	}
	EntityProperties properties = properties_from_record(record);
	if (name == "name1") {
		properties.name1 = value;
	} else if (name == "name2") {
		properties.name2 = value;
	} else {
		impl_->last_error = "Unknown entity string property: " + name;
		return false;
	}
	return set_entity_properties(kind, index, properties, nullptr);
}

bool MissionDocument::add_entity(EntityKind kind, int item_id, const EntityTransform &transform, EntityRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	std::vector<bms::Entity> *entities = entities_for(impl_->file, kind);
	if (entities == nullptr) {
		impl_->last_error = "Invalid entity kind";
		return false;
	}
	bms::Entity entity = make_default_entity(impl_->file, kind, item_id, transform);
	entities->push_back(entity);
	sync_counts();
	if (out != nullptr) {
		*out = to_record(entities->back(), kind, entities->size() - 1);
	}
	return true;
}

bool MissionDocument::remove_entity(EntityKind kind, size_t index) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	std::vector<bms::Entity> *entities = entities_for(impl_->file, kind);
	if (entities == nullptr || index >= entities->size()) {
		impl_->last_error = "Mission entity index out of range";
		return false;
	}
	entities->erase(entities->begin() + static_cast<std::ptrdiff_t>(index));
	if (kind == EntityKind::Marker) {
		repair_waypoint_marker_references(impl_->file, index);
	}
	sync_counts();
	return true;
}

std::vector<WaypointSummary> MissionDocument::waypoint_summaries() const {
	std::vector<WaypointSummary> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.waypoint_records.size());
	for (size_t i = 0; i < impl_->file.waypoint_records.size(); ++i) {
		const bms::WaypointRecord &record = impl_->file.waypoint_records[i];
		WaypointSummary summary;
		summary.index = i;
		summary.flags = static_cast<int>(record.flags);
		// Report the count the editor can actually act on. A shipped record may carry a raw
		// marker_count above the 32-slot region (CP19.bms has 39); the parser preserves that on
		// disk for byte-exact round-trip, but the editor only ever has min(count, 32) marker slots,
		// so clamp here to keep the path-list label honest and the ">0 = populated" test correct.
		summary.marker_count = static_cast<int>(
				std::min<uint32_t>(record.marker_count, static_cast<uint32_t>(kMaxWaypointPathMarkers)));
		out.push_back(summary);
	}
	return out;
}

size_t MissionDocument::waypoint_path_count() const {
	if (!impl_->loaded) {
		return 0;
	}
	return impl_->file.waypoint_records.size();
}

bool MissionDocument::get_waypoint_path(size_t index, WaypointPath &out) const {
	if (!impl_->loaded) {
		return false;
	}
	if (index >= impl_->file.waypoint_records.size()) {
		return false;
	}
	out = to_path(impl_->file.waypoint_records[index], index);
	return true;
}

std::vector<WaypointPath> MissionDocument::waypoint_paths() const {
	std::vector<WaypointPath> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.waypoint_records.size());
	for (size_t i = 0; i < impl_->file.waypoint_records.size(); ++i) {
		out.push_back(to_path(impl_->file.waypoint_records[i], i));
	}
	return out;
}

bool MissionDocument::set_waypoint_path(size_t index, const std::vector<int> &marker_indices, int flags, WaypointPath *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	std::string error;
	if (!validate_waypoint_path(impl_->file, index, marker_indices, error)) {
		impl_->last_error = error;
		return false;
	}
	apply_waypoint_path_to_record(impl_->file.waypoint_records[index], marker_indices, flags);
	if (out != nullptr) {
		*out = to_path(impl_->file.waypoint_records[index], index);
	}
	return true;
}

bool MissionDocument::clear_waypoint_path(size_t index, WaypointPath *out) {
	return set_waypoint_path(index, {}, 0, out);
}

bool MissionDocument::add_waypoint_marker(size_t path_index,
                                          int marker_item_id,
                                          const EntityTransform &transform,
                                          int insert_index,
                                          EntityRecord *out_marker,
                                          WaypointPath *out_path) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (path_index >= impl_->file.waypoint_records.size()) {
		impl_->last_error = "Waypoint path index out of range";
		return false;
	}
	WaypointPath current = to_path(impl_->file.waypoint_records[path_index], path_index);
	if (current.marker_indices.size() >= kMaxWaypointPathMarkers) {
		impl_->last_error = "Waypoint path marker count exceeds 32";
		return false;
	}
	std::string error;
	if (!validate_waypoint_path(impl_->file, path_index, current.marker_indices, error)) {
		impl_->last_error = error;
		return false;
	}

	bms::Entity marker = make_default_entity(impl_->file, EntityKind::Marker, marker_item_id, transform);
	marker.bmsi_attributes |= static_cast<uint32_t>(bms::BmsiAttributeFlags::NavigationWaypoint);
	impl_->file.markers.push_back(marker);
	const int new_marker_index = static_cast<int>(impl_->file.markers.size() - 1);
	size_t insertion = current.marker_indices.size();
	if (insert_index >= 0) {
		insertion = std::min<size_t>(static_cast<size_t>(insert_index), current.marker_indices.size());
	}
	current.marker_indices.insert(current.marker_indices.begin() + static_cast<std::ptrdiff_t>(insertion), new_marker_index);
	apply_waypoint_path_to_record(impl_->file.waypoint_records[path_index], current.marker_indices, current.flags);
	sync_counts();
	if (out_marker != nullptr) {
		*out_marker = to_record(impl_->file.markers.back(), EntityKind::Marker, impl_->file.markers.size() - 1);
	}
	if (out_path != nullptr) {
		*out_path = to_path(impl_->file.waypoint_records[path_index], path_index);
	}
	return true;
}

size_t MissionDocument::area_trigger_count() const {
	if (!impl_->loaded) {
		return 0;
	}
	return impl_->file.area_triggers.size();
}

bool MissionDocument::get_area_trigger(size_t index, AreaTriggerRecord &out) const {
	if (!impl_->loaded || index >= impl_->file.area_triggers.size()) {
		return false;
	}
	out = to_area_trigger_record(impl_->file.area_triggers[index], index);
	return true;
}

std::vector<AreaTriggerRecord> MissionDocument::area_triggers() const {
	std::vector<AreaTriggerRecord> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.area_triggers.size());
	for (size_t i = 0; i < impl_->file.area_triggers.size(); ++i) {
		out.push_back(to_area_trigger_record(impl_->file.area_triggers[i], i));
	}
	return out;
}

bool MissionDocument::add_area_trigger(const AreaTriggerRecord &record, AreaTriggerRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	impl_->file.area_triggers.push_back(from_area_trigger_record(record));
	sync_counts();
	if (out != nullptr) {
		*out = to_area_trigger_record(impl_->file.area_triggers.back(), impl_->file.area_triggers.size() - 1);
	}
	return true;
}

bool MissionDocument::set_area_trigger(size_t index, const AreaTriggerRecord &record, AreaTriggerRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (index >= impl_->file.area_triggers.size()) {
		impl_->last_error = "Area trigger index out of range";
		return false;
	}
	impl_->file.area_triggers[index] = from_area_trigger_record(record);
	if (out != nullptr) {
		*out = to_area_trigger_record(impl_->file.area_triggers[index], index);
	}
	return true;
}

bool MissionDocument::remove_area_trigger(size_t index) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (index >= impl_->file.area_triggers.size()) {
		impl_->last_error = "Area trigger index out of range";
		return false;
	}
	// Repair *IsWithinArea references. Phase-5 RE confirmed param2 is the area-trigger ARRAY INDEX
	// (Entity_IsTeamInTriggerBounds @0x43c730: &unk_A32D10 + 32*param2), so removing a zone shifts every
	// higher index down by one. A reference to the removed zone becomes -1 (dangling), which
	// get_event_chain then flags. [orig: zone bounds consumers @0x43c730 / @0x43e510]
	const int removed = static_cast<int>(index);
	for (bms::Trigger &t : impl_->file.triggers) {
		const bool area_trigger =
				(t.main_type == bms::TriggerMainType::Group &&
						t.sub_type == static_cast<int>(bms::GroupTriggerType::GroupIsWithinArea)) ||
				(t.main_type == bms::TriggerMainType::Single &&
						t.sub_type == static_cast<int>(bms::SingleTriggerType::SingleIsWithinArea));
		if (!area_trigger) {
			continue;
		}
		if (t.param2 == removed) {
			t.param2 = -1;  // the referenced zone is gone
		} else if (t.param2 > removed) {
			--t.param2;     // zones above the hole shifted down
		}
	}
	impl_->file.area_triggers.erase(impl_->file.area_triggers.begin() + static_cast<std::ptrdiff_t>(index));
	sync_counts();
	return true;
}

std::vector<WeaponLoadoutEntry> MissionDocument::weapon_loadout() const {
	std::vector<WeaponLoadoutEntry> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.loadout.entries.size());
	for (const bms::WeaponLoadoutRecord &entry : impl_->file.loadout.entries) {
		out.push_back({entry.name, entry.ammo_primary, entry.ammo_secondary, entry.flags});
	}
	return out;
}

std::vector<ItemAvailabilityEntry> MissionDocument::item_availability() const {
	std::vector<ItemAvailabilityEntry> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.item_availability.size());
	for (const bms::ItemAvailabilityEntry &entry : impl_->file.item_availability) {
		// The status byte is signed in the original's read (a -1 pair value maps to
		// 3 mission-allowed at apply) [orig: @0x54de3f..0x54de49].
		out.push_back({entry.name, static_cast<int>(static_cast<int8_t>(entry.status))});
	}
	return out;
}

bool MissionDocument::set_weapon_loadout(const std::vector<WeaponLoadoutEntry> &entries) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	// The .bms loadout chunk serializes an empty name as a leading NUL, which the loader reads as the
	// chunk terminator: a nameless entry cannot be stored and would silently drop that weapon (and every
	// entry after it). Reject the whole edit and leave the loadout untouched rather than lose data.
	// Removing a weapon goes through the dedicated remove path, never a blanked name; the editor also
	// guards this up front in _commit_loadout_editors. An empty entry list (delete all) is still valid.
	for (const WeaponLoadoutEntry &entry : entries) {
		if (entry.name.empty()) {
			impl_->last_error = "Weapon loadout entries require a name";
			return false;
		}
	}
	std::vector<bms::WeaponLoadoutRecord> records;
	records.reserve(entries.size());
	for (const WeaponLoadoutEntry &entry : entries) {
		// Names are guaranteed non-empty by the validation above.
		records.push_back({entry.name, entry.ammo_primary, entry.ammo_secondary,
		                   entry.flags.empty() ? "-1" : entry.flags});
	}
	impl_->file.loadout.entries = std::move(records);
	sync_counts();
	return true;
}

size_t MissionDocument::group_count() const {
	if (!impl_->loaded) {
		return 0;
	}
	return impl_->file.group_records.size();
}

bool MissionDocument::get_group(size_t index, GroupFields &out) const {
	if (!impl_->loaded || index >= impl_->file.group_records.size()) {
		return false;
	}
	out.index = index;
	out.field0 = impl_->file.group_records[index].flags;
	out.field8 = impl_->file.group_records[index].value;
	out.field12 = 10;
	return true;
}

std::vector<GroupFields> MissionDocument::groups() const {
	std::vector<GroupFields> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.group_records.size());
	for (size_t i = 0; i < impl_->file.group_records.size(); ++i) {
		GroupFields g;
		get_group(i, g);
		out.push_back(g);
	}
	return out;
}

bool MissionDocument::set_group(size_t index, int field0, int field8, int field12) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (index >= impl_->file.group_records.size()) {
		impl_->last_error = "Group index out of range";
		return false;
	}
	if ((field0 & ~0x3) != 0) {
		impl_->last_error = "Group flags may only use bits 0 and 1";
		return false;
	}
	if (field12 != 10) {
		impl_->last_error = "Group constant must be 10";
		return false;
	}
	impl_->file.group_records[index].flags = static_cast<int32_t>(field0);
	impl_->file.group_records[index].value = static_cast<int32_t>(field8);
	return true;
}

size_t MissionDocument::event_count() const {
	if (!impl_->loaded) {
		return 0;
	}
	return impl_->file.events.size();
}

bool MissionDocument::get_event(size_t index, MissionEventRecord &out) const {
	if (!impl_->loaded || index >= impl_->file.events.size()) {
		return false;
	}
	out = to_event_record(impl_->file.events[index], index);
	return true;
}

std::vector<MissionEventRecord> MissionDocument::events() const {
	std::vector<MissionEventRecord> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.events.size());
	for (size_t i = 0; i < impl_->file.events.size(); ++i) {
		out.push_back(to_event_record(impl_->file.events[i], i));
	}
	return out;
}

bool MissionDocument::set_event(size_t index, const MissionEventRecord &record, MissionEventRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	apply_event_record(impl_->file.events[index], record);
	if (out != nullptr) {
		*out = to_event_record(impl_->file.events[index], index);
	}
	return true;
}

size_t MissionDocument::trigger_count() const {
	if (!impl_->loaded) {
		return 0;
	}
	return impl_->file.triggers.size();
}

bool MissionDocument::get_trigger(size_t index, MissionTriggerRecord &out) const {
	if (!impl_->loaded || index >= impl_->file.triggers.size()) {
		return false;
	}
	out = to_trigger_record(impl_->file.triggers[index], index);
	return true;
}

std::vector<MissionTriggerRecord> MissionDocument::triggers() const {
	std::vector<MissionTriggerRecord> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.triggers.size());
	for (size_t i = 0; i < impl_->file.triggers.size(); ++i) {
		out.push_back(to_trigger_record(impl_->file.triggers[i], i));
	}
	return out;
}

bool MissionDocument::set_trigger(size_t index, const MissionTriggerRecord &record, MissionTriggerRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (index >= impl_->file.triggers.size()) {
		impl_->last_error = "Mission trigger index out of range";
		return false;
	}
	impl_->file.triggers[index] = trigger_from_record(record);
	if (out != nullptr) {
		*out = to_trigger_record(impl_->file.triggers[index], index);
	}
	return true;
}

size_t MissionDocument::action_count() const {
	if (!impl_->loaded) {
		return 0;
	}
	return impl_->file.actions.size();
}

bool MissionDocument::get_action(size_t index, MissionActionRecord &out) const {
	if (!impl_->loaded || index >= impl_->file.actions.size()) {
		return false;
	}
	out = to_action_record(impl_->file.actions[index], index);
	return true;
}

std::vector<MissionActionRecord> MissionDocument::actions() const {
	std::vector<MissionActionRecord> out;
	if (!impl_->loaded) {
		return out;
	}
	out.reserve(impl_->file.actions.size());
	for (size_t i = 0; i < impl_->file.actions.size(); ++i) {
		out.push_back(to_action_record(impl_->file.actions[i], i));
	}
	return out;
}

bool MissionDocument::set_action(size_t index, const MissionActionRecord &record, MissionActionRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (index >= impl_->file.actions.size()) {
		impl_->last_error = "Mission action index out of range";
		return false;
	}
	impl_->file.actions[index] = action_from_record(record);
	if (out != nullptr) {
		*out = to_action_record(impl_->file.actions[index], index);
	}
	return true;
}

bool MissionDocument::insert_event_trigger(size_t event_index, size_t local_index, const MissionTriggerRecord &record, MissionEventChain *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (event_index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	bms::Event &event = impl_->file.events[event_index];
	if (event.trigger_count >= kMaxEventChainEntries) {
		impl_->last_error = "Mission event trigger count exceeds 20";
		return false;
	}
	if (local_index > event.trigger_count) {
		impl_->last_error = "Mission event trigger insert index out of range";
		return false;
	}
	if (event.trigger_count > 0 && !valid_range(event.trigger_index, event.trigger_count, impl_->file.triggers.size())) {
		impl_->last_error = "Mission event trigger range is invalid";
		return false;
	}
	const size_t global_index = event.trigger_count == 0 ? impl_->file.triggers.size() : static_cast<size_t>(event.trigger_index) + local_index;
	impl_->file.triggers.insert(impl_->file.triggers.begin() + static_cast<std::ptrdiff_t>(global_index), trigger_from_record(record));
	for (size_t i = 0; i < impl_->file.events.size(); ++i) {
		if (i == event_index) {
			continue;
		}
		bms::Event &other = impl_->file.events[i];
		if (other.trigger_count > 0 && other.trigger_index >= static_cast<int32_t>(global_index)) {
			other.trigger_index += 1;
		}
	}
	if (event.trigger_count == 0) {
		event.trigger_index = static_cast<int32_t>(global_index);
	}
	event.trigger_count = static_cast<uint8_t>(event.trigger_count + 1);
	sync_counts();
	if (out != nullptr) {
		get_event_chain(event_index, *out);
	}
	return true;
}

bool MissionDocument::remove_event_trigger(size_t event_index, size_t local_index, MissionEventChain *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (event_index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	bms::Event &event = impl_->file.events[event_index];
	if (local_index >= event.trigger_count) {
		impl_->last_error = "Mission event trigger index out of range";
		return false;
	}
	if (!valid_range(event.trigger_index, event.trigger_count, impl_->file.triggers.size())) {
		impl_->last_error = "Mission event trigger range is invalid";
		return false;
	}
	const size_t global_index = static_cast<size_t>(event.trigger_index) + local_index;
	impl_->file.triggers.erase(impl_->file.triggers.begin() + static_cast<std::ptrdiff_t>(global_index));
	event.trigger_count = static_cast<uint8_t>(event.trigger_count - 1);
	if (event.trigger_count == 0) {
		event.trigger_index = 0;
	}
	for (size_t i = 0; i < impl_->file.events.size(); ++i) {
		if (i == event_index) {
			continue;
		}
		bms::Event &other = impl_->file.events[i];
		if (other.trigger_count > 0 && other.trigger_index > static_cast<int32_t>(global_index)) {
			other.trigger_index -= 1;
		}
	}
	sync_counts();
	if (out != nullptr) {
		get_event_chain(event_index, *out);
	}
	return true;
}

bool MissionDocument::move_event_trigger(size_t event_index, size_t local_index, int delta, MissionEventChain *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (event_index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	bms::Event &event = impl_->file.events[event_index];
	const int next_local = static_cast<int>(local_index) + delta;
	if (delta == 0 || local_index >= event.trigger_count || next_local < 0 || next_local >= event.trigger_count) {
		impl_->last_error = "Mission event trigger move index out of range";
		return false;
	}
	if (!valid_range(event.trigger_index, event.trigger_count, impl_->file.triggers.size())) {
		impl_->last_error = "Mission event trigger range is invalid";
		return false;
	}
	const size_t first = static_cast<size_t>(event.trigger_index);
	std::swap(impl_->file.triggers[first + local_index], impl_->file.triggers[first + static_cast<size_t>(next_local)]);
	if (out != nullptr) {
		get_event_chain(event_index, *out);
	}
	return true;
}

bool MissionDocument::insert_event_action(size_t event_index, size_t local_index, const MissionActionRecord &record, MissionEventChain *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (event_index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	bms::Event &event = impl_->file.events[event_index];
	if (event.action_count >= kMaxEventChainEntries) {
		impl_->last_error = "Mission event action count exceeds 20";
		return false;
	}
	if (local_index > event.action_count) {
		impl_->last_error = "Mission event action insert index out of range";
		return false;
	}
	if (event.action_count > 0 && !valid_range(event.action_index, event.action_count, impl_->file.actions.size())) {
		impl_->last_error = "Mission event action range is invalid";
		return false;
	}
	const size_t global_index = event.action_count == 0 ? impl_->file.actions.size() : static_cast<size_t>(event.action_index) + local_index;
	impl_->file.actions.insert(impl_->file.actions.begin() + static_cast<std::ptrdiff_t>(global_index), action_from_record(record));
	for (size_t i = 0; i < impl_->file.events.size(); ++i) {
		if (i == event_index) {
			continue;
		}
		bms::Event &other = impl_->file.events[i];
		if (other.action_count > 0 && other.action_index >= static_cast<int32_t>(global_index)) {
			other.action_index += 1;
		}
	}
	if (event.action_count == 0) {
		event.action_index = static_cast<int32_t>(global_index);
	}
	event.action_count = static_cast<uint8_t>(event.action_count + 1);
	sync_counts();
	if (out != nullptr) {
		get_event_chain(event_index, *out);
	}
	return true;
}

bool MissionDocument::remove_event_action(size_t event_index, size_t local_index, MissionEventChain *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (event_index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	bms::Event &event = impl_->file.events[event_index];
	if (local_index >= event.action_count) {
		impl_->last_error = "Mission event action index out of range";
		return false;
	}
	if (!valid_range(event.action_index, event.action_count, impl_->file.actions.size())) {
		impl_->last_error = "Mission event action range is invalid";
		return false;
	}
	const size_t global_index = static_cast<size_t>(event.action_index) + local_index;
	impl_->file.actions.erase(impl_->file.actions.begin() + static_cast<std::ptrdiff_t>(global_index));
	event.action_count = static_cast<uint8_t>(event.action_count - 1);
	if (event.action_count == 0) {
		event.action_index = 0;
	}
	for (size_t i = 0; i < impl_->file.events.size(); ++i) {
		if (i == event_index) {
			continue;
		}
		bms::Event &other = impl_->file.events[i];
		if (other.action_count > 0 && other.action_index > static_cast<int32_t>(global_index)) {
			other.action_index -= 1;
		}
	}
	sync_counts();
	if (out != nullptr) {
		get_event_chain(event_index, *out);
	}
	return true;
}

bool MissionDocument::move_event_action(size_t event_index, size_t local_index, int delta, MissionEventChain *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (event_index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	bms::Event &event = impl_->file.events[event_index];
	const int next_local = static_cast<int>(local_index) + delta;
	if (delta == 0 || local_index >= event.action_count || next_local < 0 || next_local >= event.action_count) {
		impl_->last_error = "Mission event action move index out of range";
		return false;
	}
	if (!valid_range(event.action_index, event.action_count, impl_->file.actions.size())) {
		impl_->last_error = "Mission event action range is invalid";
		return false;
	}
	const size_t first = static_cast<size_t>(event.action_index);
	std::swap(impl_->file.actions[first + local_index], impl_->file.actions[first + static_cast<size_t>(next_local)]);
	if (out != nullptr) {
		get_event_chain(event_index, *out);
	}
	return true;
}

bool MissionDocument::add_event(const MissionEventRecord &record, MissionEventRecord *out) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	bms::Event event = {};
	apply_event_record(event, record);
	// A fresh event owns no triggers/actions; the index/count fields stay zero until the caller adds
	// entries via insert_event_trigger/insert_event_action (each assigns the global index on first add).
	event.trigger_index = 0;
	event.action_index = 0;
	event.trigger_count = 0;
	event.action_count = 0;
	impl_->file.events.push_back(event);
	sync_counts();
	if (out != nullptr) {
		*out = to_event_record(impl_->file.events.back(), impl_->file.events.size() - 1);
	}
	return true;
}

bool MissionDocument::remove_event(size_t index) {
	if (!impl_->loaded) {
		impl_->last_error = "No mission loaded";
		return false;
	}
	if (index >= impl_->file.events.size()) {
		impl_->last_error = "Mission event index out of range";
		return false;
	}
	const bms::Event &event = impl_->file.events[index];
	if (!valid_range(event.trigger_index, event.trigger_count, impl_->file.triggers.size())) {
		impl_->last_error = "Mission event trigger range is invalid";
		return false;
	}
	if (!valid_range(event.action_index, event.action_count, impl_->file.actions.size())) {
		impl_->last_error = "Mission event action range is invalid";
		return false;
	}
	// Drain the event's triggers and actions through the single-element removers, which fix up every
	// other event's trigger_index / action_index exactly as a normal trigger/action delete would. The
	// ranges above are validated before the first mutation so malformed documents fail transactionally.
	while (impl_->file.events[index].trigger_count > 0) {
		remove_event_trigger(index, 0);
	}
	while (impl_->file.events[index].action_count > 0) {
		remove_event_action(index, 0);
	}
	// Repair ResetEvent action references (param1 = event index, the one proven cross-reference): events
	// after the hole shift down by one; a reference to the removed event becomes dangling (-1), which
	// get_event_chain then flags as out-of-range. (Area-trigger refs are left alone because their index
	// semantics are still under RE; here the semantics are proven, so the repair is safe.)
	for (bms::Action &action : impl_->file.actions) {
		if (action.action_type != bms::ActionType::ResetEvent) {
			continue;
		}
		if (action.param1 > static_cast<int32_t>(index)) {
			action.param1 -= 1;
		} else if (action.param1 == static_cast<int32_t>(index)) {
			action.param1 = -1;
		}
	}
	impl_->file.events.erase(impl_->file.events.begin() + static_cast<std::ptrdiff_t>(index));
	sync_counts();
	return true;
}

bool MissionDocument::get_event_chain(size_t index, MissionEventChain &out) const {
	out = {};
	if (!impl_->loaded || index >= impl_->file.events.size()) {
		return false;
	}
	out.event = to_event_record(impl_->file.events[index], index);
	if (!valid_range(out.event.trigger_index, out.event.trigger_count, impl_->file.triggers.size())) {
		out.diagnostics.push_back(logic_diagnostic(
				"logic.trigger_range_out_of_range",
				"Event trigger range is outside the mission trigger table.",
				"event",
				static_cast<int>(index)));
	} else {
		for (int i = 0; i < out.event.trigger_count; ++i) {
			const size_t trigger_index = static_cast<size_t>(out.event.trigger_index + i);
			MissionTriggerRecord trigger = to_trigger_record(impl_->file.triggers[trigger_index], trigger_index);
			out.triggers.push_back(trigger);
			out.references.push_back(logic_reference("event", static_cast<int>(index), "trigger", static_cast<int>(trigger_index), 0, static_cast<int>(trigger_index), "trigger", true));
			add_trigger_area_reference(trigger, impl_->file.area_triggers.size(), out);
		}
	}
	if (!valid_range(out.event.action_index, out.event.action_count, impl_->file.actions.size())) {
		out.diagnostics.push_back(logic_diagnostic(
				"logic.action_range_out_of_range",
				"Event action range is outside the mission action table.",
				"event",
				static_cast<int>(index)));
	} else {
		for (int i = 0; i < out.event.action_count; ++i) {
			const size_t action_index = static_cast<size_t>(out.event.action_index + i);
			MissionActionRecord action = to_action_record(impl_->file.actions[action_index], action_index);
			out.actions.push_back(action);
			out.references.push_back(logic_reference("event", static_cast<int>(index), "action", static_cast<int>(action_index), 0, static_cast<int>(action_index), "action", true));
			if (action.action_type == static_cast<int>(bms::ActionType::ResetEvent)) {
				const bool valid = action.param1 >= 0 && static_cast<size_t>(action.param1) < impl_->file.events.size();
				out.references.push_back(logic_reference("action", static_cast<int>(action_index), "event", action.param1, 1, action.param1, "reset event", valid));
				if (!valid) {
					out.diagnostics.push_back(logic_diagnostic(
							"logic.event_reference_out_of_range",
							"Action references an event index outside the mission event table.",
							"action",
							static_cast<int>(action_index)));
				}
			}
		}
	}
	return true;
}

MissionLogicSummary MissionDocument::logic_summary() const {
	MissionLogicSummary out;
	if (!impl_->loaded) {
		return out;
	}
	out.event_count = impl_->file.events.size();
	out.trigger_count = impl_->file.triggers.size();
	out.action_count = impl_->file.actions.size();
	out.area_trigger_count = impl_->file.area_triggers.size();
	for (size_t i = 0; i < impl_->file.events.size(); ++i) {
		MissionEventChain chain;
		if (get_event_chain(i, chain)) {
			out.diagnostic_count += chain.diagnostics.size();
		}
	}
	return out;
}

namespace {

// True when a name-mapping switch named the probed value (anything other than the "Unknown(N)" fallback
// from unknown_label). The reflectors below keep only named values, so the dropdowns track bms.h.
bool is_named_enum_value(const std::string &name) {
	return name.rfind("Unknown(", 0) != 0;
}

} // namespace

std::vector<MissionEnumEntry> MissionDocument::trigger_main_types() const {
	std::vector<MissionEnumEntry> out;
	for (int value = 0; value <= 15; ++value) {
		std::string name = trigger_main_type_name(value);
		if (is_named_enum_value(name)) {
			out.push_back({value, name});
		}
	}
	return out;
}

std::vector<MissionEnumEntry> MissionDocument::trigger_sub_types(int main_type) const {
	std::vector<MissionEnumEntry> out;
	// Sub-type values are non-contiguous (e.g. SingleTriggerType jumps 17 -> 42); probe wide and keep
	// the named ones so the editor lists exactly the engine's accepted sub-types for this main type.
	for (int value = 0; value <= 63; ++value) {
		std::string name = trigger_sub_type_name(main_type, value);
		if (is_named_enum_value(name)) {
			out.push_back({value, name});
		}
	}
	return out;
}

std::vector<MissionEnumEntry> MissionDocument::action_types() const {
	std::vector<MissionEnumEntry> out;
	for (int value = 0; value <= 63; ++value) {
		std::string name = action_type_name(value);
		if (is_named_enum_value(name)) {
			out.push_back({value, name});
		}
	}
	return out;
}

std::vector<MissionEnumEntry> MissionDocument::action_sub_types(int action_type) const {
	std::vector<MissionEnumEntry> out;
	for (int value = 0; value <= 63; ++value) {
		std::string name = action_sub_type_name(action_type, value);
		if (is_named_enum_value(name)) {
			out.push_back({value, name});
		}
	}
	return out;
}

std::vector<MissionEnumEntry> MissionDocument::event_flag_bits() const {
	// The three author-facing event flags, matching the DFX2 editor's checkboxes exactly. Shipped missions
	// also use internal bits 0x10/0x20; set_event preserves those while editing only the checkbox mask.
	// [orig: Med_EventDialogPopulate @0x411690 (CheckDlgButton 4203/4212/4213),
	// Med_EventDialogCommit @0x4118d0 (sets bits 0/1/2 only). dfx2med.exe]
	return {
			{static_cast<int>(bms::EventFlags::ResetAfter), "Reset after"},
			{static_cast<int>(bms::EventFlags::PreMission), "Pre-mission"},
			{static_cast<int>(bms::EventFlags::PostMission), "Post-mission"},
	};
}

int MissionDocument::event_flag_mask() const {
	return static_cast<int>(bms::kEventAuthorFlagMask);
}

std::vector<MissionEnumEntry> MissionDocument::ai_attribute_flag_bits() const {
	// Author-facing AI attribute flags (bmsi_attributes). Labels confirmed against the DFX2 object-properties
	// dialog (Med_ObjectPropertiesDialog @0x4096d0; label table @0x5b1c84: BLIND / GUARDING / MULTIPLAYER /
	// INDESTRUCTABLE / NAVIGATION_WAYPT / REFLECTIVE / ...). Attribute17 is accepted because it appears in
	// shipped missions, but its editor label is not pinned, so it is intentionally omitted from checkboxes.
	return {
			{static_cast<int>(bms::BmsiAttributeFlags::Blind), "Blind"},
			{static_cast<int>(bms::BmsiAttributeFlags::Guarding), "Guarding"},
			{static_cast<int>(bms::BmsiAttributeFlags::RemoveIfLessThan), "Remove if fewer than"},
			{static_cast<int>(bms::BmsiAttributeFlags::RemoveIfMoreThan), "Remove if more than"},
			{static_cast<int>(bms::BmsiAttributeFlags::Multiplayer), "Multiplayer only"},
			{static_cast<int>(bms::BmsiAttributeFlags::Berserk), "Berserk"},
			{static_cast<int>(bms::BmsiAttributeFlags::FlyingOrganic), "Flying"},
			{static_cast<int>(bms::BmsiAttributeFlags::Coward), "Coward"},
			{static_cast<int>(bms::BmsiAttributeFlags::AdvancedAmmo), "Advanced ammo"},
			{static_cast<int>(bms::BmsiAttributeFlags::Indestructible), "Indestructible"},
			{static_cast<int>(bms::BmsiAttributeFlags::NavigationWaypoint), "Navigation waypoint"},
			{static_cast<int>(bms::BmsiAttributeFlags::Reflective), "Reflective"},
			{static_cast<int>(bms::BmsiAttributeFlags::NoShadow), "No shadow"},
	};
}

const bms::File &MissionDocument::bms_file() const {
	return impl_->file;
}

bms::File &MissionDocument::bms_file() {
	return impl_->file;
}

void MissionDocument::sync_counts() {
	impl_->file.header.num_items = static_cast<uint32_t>(impl_->file.items.size());
	impl_->file.header.num_buildings = static_cast<uint32_t>(impl_->file.buildings.size());
	impl_->file.header.num_markers = static_cast<uint32_t>(impl_->file.markers.size());
	impl_->file.header.num_people = static_cast<uint32_t>(impl_->file.organics.size());
	impl_->file.header.num_events = static_cast<uint32_t>(impl_->file.events.size());
	impl_->file.header.area_trigger_count = static_cast<int16_t>(impl_->file.area_triggers.size());
	std::vector<uint8_t> loadout_chunk;
	for (const bms::WeaponLoadoutRecord &entry : impl_->file.loadout.entries) {
		loadout_chunk.insert(loadout_chunk.end(), entry.name.begin(), entry.name.end());
		loadout_chunk.push_back(0);
		loadout_chunk.insert(loadout_chunk.end(), entry.ammo_primary.begin(), entry.ammo_primary.end());
		loadout_chunk.push_back(0);
		loadout_chunk.insert(loadout_chunk.end(), entry.ammo_secondary.begin(), entry.ammo_secondary.end());
		loadout_chunk.push_back(0);
		const std::string flags = entry.flags.empty() ? "-1" : entry.flags;
		loadout_chunk.insert(loadout_chunk.end(), flags.begin(), flags.end());
		loadout_chunk.push_back(0);
	}
	if (!loadout_chunk.empty()) {
		loadout_chunk.push_back(0);
	}
	std::vector<uint8_t> availability_chunk;
	for (const bms::ItemAvailabilityEntry &entry : impl_->file.item_availability) {
		availability_chunk.insert(availability_chunk.end(), entry.name.begin(), entry.name.end());
		availability_chunk.push_back(0);
		availability_chunk.push_back(entry.status);
	}
	if (!availability_chunk.empty()) {
		availability_chunk.push_back(0);
	}
	impl_->file.header.weapon_loadout_chunk_len = static_cast<uint16_t>(loadout_chunk.size());
	impl_->file.header.secondary_chunk_len = static_cast<uint16_t>(availability_chunk.size());
	impl_->file.events_count = static_cast<int32_t>(impl_->file.events.size());
	impl_->file.trigger_count = static_cast<int32_t>(impl_->file.triggers.size());
	impl_->file.action_count = static_cast<int32_t>(impl_->file.actions.size());
	impl_->file.bounding_box_count = static_cast<int32_t>(impl_->file.bounding_boxes.size());

	if (impl_->file.waypoint_records.empty()) {
		impl_->file.waypoint_records.resize(bms::kWaypointRecordCount);
	}
	if (impl_->file.group_records.empty()) {
		impl_->file.group_records.resize(bms::kGroupRecordCount);
	}
	if (impl_->file.layer_records.empty()) {
		impl_->file.layer_records.resize(bms::kLayerRecordCount);
	}
	// A freshly-resized WaypointRecord has empty padding, so the writer emits a short
	// (8-byte) record that no longer reparses (parse expects the fixed 136-byte record).
	// Normalize every record's padding to the 128-byte payload (128 - markers*4), matching
	// parse_waypoint_record. Idempotent for already-loaded records, so byte-exact
	// round-trips are preserved; it is the from-scratch (create_default) path that needs it.
	for (bms::WaypointRecord &record : impl_->file.waypoint_records) {
		resize_waypoint_padding(record, /*preserve_over_count=*/true); // pure round-trip: keep a shipped over-count
	}
}

} // namespace opennova::mission
