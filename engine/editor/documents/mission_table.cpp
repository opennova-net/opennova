// The mission's table (mission_table.h): the kinds, each record's fields projected from the format's
// rows (formats/mission/mission_field.h) with the editor's check before them, the Record references
// one record makes to another by its index, and the lists a record holds over the file's own vectors.
#include "mission_table.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_field.h>

namespace opennova::editor {
using namespace mission;

namespace {

using K = MissionKind;
constexpr NodeKind k(K kind) { return node_kind(kind); }

// --- the kinds -------------------------------------------------------------------------------------

// Each kind's row: its token and label, what the outline's Add of a row of it says ("" = none: the
// mission is one per file, its 128 paths are fixed, a nested record is added through its list),
// whether it is a row of the file, and the format's record its fields are.
struct KindRow {
	K kind;
	const char *token;
	const char *label;
	const char *add_label;
	bool top;
	MissionRecord record;
};
constexpr KindRow kKinds[] = {
	{K::Mission, "mission", "Mission", "", true, MissionRecord::Header},
	{K::Loadout, "loadout", "Loadout entry", "", false, MissionRecord::Loadout},
	{K::Availability, "availability", "Item availability rule", "", false, MissionRecord::Availability},
	{K::Group, "group", "Group", "", false, MissionRecord::Group},
	{K::Layer, "layer", "Layer", "", false, MissionRecord::Layer},
	{K::BoundingBox, "bounding_box", "Bounding box", "", false, MissionRecord::BoundingBox},
	{K::Item, "item", "Item", "Add item", true, MissionRecord::Entity},
	{K::Building, "building", "Building", "Add building", true, MissionRecord::Entity},
	{K::Marker, "marker", "Marker", "Add marker", true, MissionRecord::Entity},
	{K::Organic, "organic", "Organic", "Add organic", true, MissionRecord::Entity},
	{K::WaypointPath, "waypoint_path", "Waypoint path", "", true, MissionRecord::WaypointPath},
	{K::Stop, "stop", "Stop", "", false, MissionRecord::Stop},
	{K::Area, "area", "Area trigger", "Add area trigger", true, MissionRecord::Area},
	{K::Event, "event", "Event", "Add event", true, MissionRecord::Event},
	{K::Trigger, "trigger", "Trigger", "Add trigger", true, MissionRecord::Trigger},
	{K::Action, "action", "Action", "Add action", true, MissionRecord::Action},
};
static_assert(std::size(kKinds) == kMissionKindCount, "every MissionKind has exactly one row");

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) ++a, ++b;
	return *a == *b;
}
constexpr bool kinds_in_order() {
	for (size_t i = 0; i < std::size(kKinds); ++i) {
		if (size_t(kKinds[i].kind) != i) return false;
		if (*kKinds[i].add_label && !kKinds[i].top) return false;
		for (size_t j = i + 1; j < std::size(kKinds); ++j)
			if (same_text(kKinds[i].token, kKinds[j].token)) return false;
	}
	return true;
}
static_assert(kinds_in_order(), "the kinds in MissionKind's order, each token its own, only a row added by the outline");

// --- what the editor shows -------------------------------------------------------------------------

// The readable names of the fields, by record and key; the group a row draws a field on; a unit the
// format states (the eulers are whole degrees); a colour the number packs. A field without a row shows
// its key.
struct FieldLabel {
	MissionRecord record;
	const char *key;
	const char *label;
	const char *group;
	const char *unit;
	FieldColor color = FieldColor::None;
};
constexpr FieldLabel kLabels[] = {
	{MissionRecord::Header, "mission_name", "Name", "", ""},
	{MissionRecord::Header, "designer", "Designer", "", ""},
	{MissionRecord::Header, "briefing", "Briefing", "", ""},
	{MissionRecord::Header, "terrain", "Terrain", "terrain", ""},
	{MissionRecord::Header, "cnv_file", "CNV file", "terrain", ""},
	{MissionRecord::Header, "tt_file", "TT file", "terrain", ""},
	{MissionRecord::Header, "terrain_tile", "Terrain tile", "terrain", ""},
	{MissionRecord::Header, "environment", "Environment", "", ""},
	{MissionRecord::Header, "climate", "Climate", "", ""},
	{MissionRecord::Header, "weather", "Weather", "", ""},
	{MissionRecord::Header, "mission_type", "Mission type", "", ""},
	{MissionRecord::Header, "attrib_flags", "Attributes", "", ""},
	{MissionRecord::Header, "start_time", "Start time", "", ""},
	{MissionRecord::Header, "minutes_per_day", "Minutes per day", "", ""},
	{MissionRecord::Header, "player_health", "Player health", "", ""},
	{MissionRecord::Header, "mana", "Mana", "", ""},
	{MissionRecord::Header, "max_saves", "Saves", "", ""},
	{MissionRecord::Header, "bonus_expiration", "Bonus expiration", "", ""},
	{MissionRecord::Header, "wind_speed", "Wind speed", "wind", ""},
	{MissionRecord::Header, "wind_direction", "Wind direction", "wind", ""},
	{MissionRecord::Header, "water_override", "Water level", "water", ""},
	{MissionRecord::Header, "water_color", "Water colour", "water", "", FieldColor::PackedRgb},
	{MissionRecord::Header, "murk", "Murk", "water", ""},
	{MissionRecord::Header, "fog_override", "Fog distance", "fog", ""},
	{MissionRecord::Header, "fog_color", "Fog colour", "fog", "", FieldColor::PackedRgb},
	{MissionRecord::Header, "map_zoom", "Map zoom", "", ""},
	{MissionRecord::Entity, "item", "Item", "", ""},
	{MissionRecord::Entity, "id", "Id", "", ""},
	{MissionRecord::Entity, "x", "X", "position", ""},
	{MissionRecord::Entity, "y", "Y", "position", ""},
	{MissionRecord::Entity, "z", "Z", "position", ""},
	{MissionRecord::Entity, "pitch", "Pitch", "rotation", "°"},
	{MissionRecord::Entity, "yaw", "Yaw", "rotation", "°"},
	{MissionRecord::Entity, "roll", "Roll", "rotation", "°"},
	{MissionRecord::Entity, "group", "Group", "", ""},
	{MissionRecord::Entity, "team", "Team", "", ""},
	{MissionRecord::Entity, "ai_flags", "AI attributes", "", ""},
	{MissionRecord::Entity, "accuracy", "Accuracy", "accuracy", ""},
	{MissionRecord::Entity, "w_accuracy2", "Accuracy (second)", "accuracy", ""},
	{MissionRecord::Entity, "spawn_count", "Move timer", "timers", ""},
	{MissionRecord::Entity, "crouch_timer", "Crouch timer", "timers", ""},
	{MissionRecord::Entity, "shoot_timer", "Shoot timer", "timers", ""},
	{MissionRecord::Entity, "advancetimer", "Advance timer", "timers", ""},
	{MissionRecord::Entity, "max_simultaneous", "No more than", "", ""},
	{MissionRecord::Entity, "no_less_than", "No less than", "", ""},
	{MissionRecord::Entity, "map_symbol", "Map symbol", "", ""},
	{MissionRecord::Entity, "mission_critical", "Mission critical", "", ""},
	{MissionRecord::Entity, "next_ssn", "Next SSN", "", ""},
	{MissionRecord::Entity, "team_budget", "Team budget", "", ""},
	{MissionRecord::Entity, "name1", "AI class", "", ""},
	{MissionRecord::Entity, "name2", "AI script", "", ""},
	{MissionRecord::Entity, "gen_string", "Generator string", "", ""},
	{MissionRecord::WaypointPath, "marker_count", "Stored count", "", ""},
	{MissionRecord::Stop, "marker", "Marker", "", ""},
	{MissionRecord::Area, "id", "Zone", "", ""},
	{MissionRecord::Area, "x_min", "X min", "x", ""},
	{MissionRecord::Area, "x_max", "X max", "x", ""},
	{MissionRecord::Area, "y_min", "Y min", "y", ""},
	{MissionRecord::Area, "y_max", "Y max", "y", ""},
	{MissionRecord::Area, "z_min", "Z min", "z", ""},
	{MissionRecord::Area, "z_max", "Z max", "z", ""},
	{MissionRecord::Event, "reset_after", "Reset after", "", ""},
	{MissionRecord::Event, "trigger_index", "First trigger", "triggers", ""},
	{MissionRecord::Event, "trigger_count", "Triggers", "triggers", ""},
	{MissionRecord::Event, "action_index", "First action", "actions", ""},
	{MissionRecord::Event, "action_count", "Actions", "actions", ""},
	{MissionRecord::Trigger, "condition_flags", "Condition", "", ""},
	{MissionRecord::Trigger, "main_type", "Type", "", ""},
	{MissionRecord::Trigger, "sub_type", "Sub-type", "", ""},
	{MissionRecord::Action, "action_type", "Type", "", ""},
	{MissionRecord::Action, "action_sub_type", "Sub-type", "", ""},
	{MissionRecord::Loadout, "name", "Weapon", "", ""},
	{MissionRecord::Loadout, "ammo_primary", "Primary clips", "", ""},
	{MissionRecord::Loadout, "ammo_secondary", "Secondary clips", "", ""},
	{MissionRecord::Availability, "name", "Weapon", "", ""},
	{MissionRecord::BoundingBox, "ref_id", "Refers to", "", ""},
};

const FieldLabel *label_of(MissionRecord record, const char *key) {
	for (const FieldLabel &row : kLabels)
		if (row.record == record && same_text(row.key, key)) return &row;
	return nullptr;
}

// What a field names outside its record: the mission's terrain and environment by base name, an
// entity's item by its items.def id (as the graph's mission extraction reads them); and a record of its
// own file by its index (a Record reference): a stop's marker.
struct ReferenceRow {
	MissionRecord record;
	const char *key;
	ReferenceKind reference;
};
constexpr ReferenceRow kReferences[] = {
	{MissionRecord::Header, "terrain", ReferenceKind::Terrain},
	{MissionRecord::Header, "environment", ReferenceKind::Environment},
	{MissionRecord::Entity, "item", ReferenceKind::Item},
	{MissionRecord::Stop, "marker", ReferenceKind::MissionMarker},
};

ReferenceKind reference_of(MissionRecord record, const char *key) {
	for (const ReferenceRow &row : kReferences)
		if (row.record == record && same_text(row.key, key)) return row.reference;
	return ReferenceKind::None;
}

// An event's runs: the first record of a run names a record of the file's table by its index (a Record
// reference) while the run holds one; a run of none names nothing, whatever its first index holds.
struct RunRow {
	const char *first;
	const char *count;
	ReferenceKind reference;
};
constexpr RunRow kRuns[] = {
	{"trigger_index", "trigger_count", ReferenceKind::MissionTrigger},
	{"action_index", "action_count", ReferenceKind::MissionAction},
};

// --- the labelled fields -----------------------------------------------------------------------------

// Whether a whole number is one the field's choices name (a flag word: bits of them).
bool named(const MissionField &field, int64_t number) {
	if (field.flags) {
		int64_t bits = 0;
		for (size_t i = 0; i < field.choice_count; ++i) bits |= field.choices[i].value;
		return (number & ~bits) == 0;
	}
	for (size_t i = 0; i < field.choice_count; ++i)
		if (field.choices[i].value == number) return true;
	return false;
}

// The editor's own check before the format's rule: a whole number inside the field's range and, where
// the field's choices are every value the record holds, one they name; a finite real, a 16.16 one in
// the word's range; a text that fits its slot (a value past them the format would clamp, cast, drop or
// cut).
bool fits(const MissionField &field, const Value &value, std::string &error) {
	switch (field.type) {
	case MissionFieldType::Integer: {
		const int64_t *number = std::get_if<int64_t>(&value);
		if (!number) {
			error = "This field takes a whole number.";
			return false;
		}
		if (*number < field.min || *number > field.max) {
			error = "The value is past what the field holds (" + std::to_string(field.min) + " to " +
			        std::to_string(field.max) + ").";
			return false;
		}
		if (field.choice_count && !field.open && !named(field, *number)) {
			error = field.flags ? "The value holds a bit none of the field's flags is." : "The value is none of the field's.";
			return false;
		}
		return true;
	}
	case MissionFieldType::Real:
	case MissionFieldType::Fixed: {
		const double *real = std::get_if<double>(&value);
		const int64_t *whole = std::get_if<int64_t>(&value);
		if (!real && !whole) {
			error = "This field takes a number.";
			return false;
		}
		const double number = real ? *real : double(*whole);
		if (!std::isfinite(number)) {
			error = "This field takes a finite number.";
			return false;
		}
		if (field.type == MissionFieldType::Fixed && (number < bms::kFixed16Min || number > bms::kFixed16Max)) {
			error = "A 16.16 number in mission units is -32768 to 32767.99998.";
			return false;
		}
		if (field.type == MissionFieldType::Real && std::abs(number) > double(std::numeric_limits<float>::max())) {
			error = "The number is past what a float holds.";
			return false;
		}
		return true;
	}
	case MissionFieldType::Text:
		if (field.width) {
			const std::string *text = std::get_if<std::string>(&value);
			if (text && text->size() > field.width) {
				error = "The text is longer than its " + std::to_string(field.width) + " bytes.";
				return false;
			}
		}
		return true;
	}
	return true;
}

FieldType field_type(const MissionField &field) {
	switch (field.type) {
	case MissionFieldType::Real:
	case MissionFieldType::Fixed: return FieldType::Real;
	case MissionFieldType::Text: return FieldType::Text;
	default:
		if (field.min < 0) return FieldType::Integer;
		return field.max <= 255 ? FieldType::Byte : FieldType::Unsigned;
	}
}

// A format row's labelled field: its schema from the row (a slot's width counts the terminator the
// editor's does, which a full slot does not write), its value through the row's own get and set, and
// what it names where its record decides (an event's run).
LabelledField labelled(const MissionField &field) {
	LabelledField out;
	FieldSchema &entry = out.schema;
	entry.id = field.key;
	entry.type = field_type(field);
	entry.width = field.width ? field.width + 1 : 0;
	entry.reference = reference_of(field.record, field.key);
	switch (field.type) {
	case MissionFieldType::Integer:
		entry.ranged = true;
		entry.min = double(field.min);
		entry.max = double(field.max);
		break;
	case MissionFieldType::Fixed:
		entry.ranged = true;
		entry.min = bms::kFixed16Min;
		entry.max = bms::kFixed16Max;
		break;
	case MissionFieldType::Real:
		entry.ranged = true;
		entry.min = -double(std::numeric_limits<float>::max());
		entry.max = double(std::numeric_limits<float>::max());
		break;
	default: break;
	}
	for (size_t i = 0; i < field.choice_count; ++i)
		entry.choices.push_back({field.choices[i].name, field.choices[i].value, ""});
	entry.flags = field.flags;
	entry.open_choices = field.open;
	entry.read_only = !field.set;
	entry.multiline = field.record == MissionRecord::Header && same_text(field.key, "briefing");
	if (const FieldLabel *label = label_of(field.record, field.key)) {
		entry.label = label->label;
		entry.group = label->group;
		entry.unit = label->unit;
		entry.color = label->color;
	}
	// The format's value is the editor's (MissionValue and Value are one variant).
	static_assert(std::is_same_v<MissionValue, Value>, "a mission field's value is the editor's");
	const MissionField *row = &field;
	out.value.get = [row](const RecordHandle &record, Value &value) { return row->get(record.data, value); };
	if (field.set)
		out.value.set = [row](const RecordHandle &record, const Value &value, std::string &error) {
			return fits(*row, value, error) && row->set(record.data, value, error);
		};
	if (field.record == MissionRecord::Event)
		for (const RunRow &run : kRuns) {
			if (!same_text(field.key, run.first)) continue;
			entry.reference = run.reference;
			const MissionField *count = find_mission_field(MissionRecord::Event, run.count);
			const ReferenceKind names = run.reference;
			out.reference = [count, names](const RecordHandle &record, const RecordOwners &) {
				MissionValue held;
				return count->get(record.data, held) && std::get<int64_t>(held) > 0 ? names : ReferenceKind::None;
			};
		}
	return out;
}

// --- the lists ---------------------------------------------------------------------------------------

// The spec a list is to the core.
Document::CollectionSpec spec(K kind, const char *label, size_t max = 0, bool fixed = false) {
	Document::CollectionSpec out;
	out.kind = k(kind);
	out.label = label;
	out.max = max;
	out.fixed = fixed;
	return out;
}

template <class Record> DetachedRecord detached(K kind, const Record &record) {
	DetachedRecord out;
	out.kind = k(kind);
	out.data = std::make_shared<Record>(record);
	return out;
}

bool own_kind(const DetachedRecord *record, K kind, std::string &error) {
	if (record && (!record->data || record->kind != k(kind))) {
		error = "This list takes records of its own kind only.";
		return false;
	}
	return true;
}

// A list of the mission row's own records a vector of the file holds (the loadout, the availability
// rules, the bounding boxes): `fresh` the list's new record (false with why where it has none to be
// written with), the header's counts and chunk lengths synced after each edit.
template <class Record>
ListOps file_list(K kind, std::vector<Record> &(*list)(bms::File &), bool (*fresh)(Record &, std::string &)) {
	ListOps ops;
	ops.size = [list](const RecordHandle &owner) { return list(owner.as<bms::File>()).size(); };
	ops.at = [kind, list](const RecordHandle &owner, size_t index) {
		std::vector<Record> &records = list(owner.as<bms::File>());
		return index < records.size() ? RecordHandle{k(kind), &records[index]} : RecordHandle{};
	};
	ops.insert = [kind, list, fresh](const RecordHandle &owner, size_t index, const DetachedRecord *record,
	                                 std::string &error) {
		if (!own_kind(record, kind, error)) return false;
		Record made{};
		if (record) made = *static_cast<const Record *>(record->data.get());
		else if (!fresh(made, error)) return false;
		bms::File &file = owner.as<bms::File>();
		std::vector<Record> &records = list(file);
		records.insert(records.begin() + std::ptrdiff_t(std::min(index, records.size())), std::move(made));
		sync_counts(file);
		return true;
	};
	ops.erase = [list](const RecordHandle &owner, size_t index) {
		bms::File &file = owner.as<bms::File>();
		std::vector<Record> &records = list(file);
		if (index >= records.size()) return false;
		records.erase(records.begin() + std::ptrdiff_t(index));
		sync_counts(file);
		return true;
	};
	ops.copy = [kind, list](const RecordHandle &owner, size_t index) {
		const std::vector<Record> &records = list(owner.as<bms::File>());
		return index < records.size() ? detached(kind, records[index]) : DetachedRecord();
	};
	return ops;
}

std::vector<bms::WeaponLoadoutRecord> &loadout_of(bms::File &file) { return file.loadout.entries; }
std::vector<bms::ItemAvailabilityEntry> &availability_of(bms::File &file) { return file.item_availability; }
std::vector<bms::GroupRecord> &groups_of(bms::File &file) { return file.group_records; }
std::vector<bms::LayerRecord> &layers_of(bms::File &file) { return file.layer_records; }
std::vector<bms::BoundingBox> &boxes_of(bms::File &file) { return file.bounding_boxes; }

// A loadout entry and an availability rule end their chunk at an empty name, so a new one has no name
// to be written with: one comes in as a copy (a Duplicate, a paste).
bool fresh_loadout(bms::WeaponLoadoutRecord &, std::string &error) {
	error = "A weapon loadout entry needs a weapon's name: duplicate or paste one.";
	return false;
}
bool fresh_availability(bms::ItemAvailabilityEntry &, std::string &error) {
	error = "An item availability rule needs a weapon's name: duplicate or paste one.";
	return false;
}
// A new bounding box: every word zero (the reserved word the writer writes as zero too).
bool fresh_box(bms::BoundingBox &, std::string &) { return true; }

// A fixed table of the file (its 64 groups, 32 layers): its records set, never added or removed (the
// writer writes exactly that many).
template <class Record> ListOps fixed_list(K kind, std::vector<Record> &(*list)(bms::File &), size_t count) {
	ListOps ops = file_list<Record>(kind, list, nullptr);
	ops.insert = [count](const RecordHandle &, size_t, const DetachedRecord *, std::string &error) {
		error = "A mission holds exactly " + std::to_string(count) + " of these.";
		return false;
	};
	ops.erase = [](const RecordHandle &, size_t) { return false; };
	return ops;
}

// A path's stops, through bms_edit's stops (32 at most; one put in or taken out writes the path's
// count as its slots). A stop comes in as a copy: a new one would name no marker.
ListOps stop_list() {
	ListOps ops;
	ops.size = [](const RecordHandle &owner) { return owner.as<bms::WaypointRecord>().waypoint_numbers.size(); };
	ops.at = [](const RecordHandle &owner, size_t index) {
		std::vector<uint32_t> &stops = owner.as<bms::WaypointRecord>().waypoint_numbers;
		return index < stops.size() ? RecordHandle{k(K::Stop), &stops[index]} : RecordHandle{};
	};
	ops.insert = [](const RecordHandle &owner, size_t index, const DetachedRecord *record, std::string &error) {
		if (!own_kind(record, K::Stop, error)) return false;
		if (!record) {
			error = "A stop names a marker of the mission: duplicate or paste one.";
			return false;
		}
		const uint32_t marker = *static_cast<const uint32_t *>(record->data.get());
		return insert_waypoint_stop(owner.as<bms::WaypointRecord>(), index, marker, error);
	};
	ops.erase = [](const RecordHandle &owner, size_t index) {
		return erase_waypoint_stop(owner.as<bms::WaypointRecord>(), index);
	};
	ops.copy = [](const RecordHandle &owner, size_t index) {
		const std::vector<uint32_t> &stops = owner.as<bms::WaypointRecord>().waypoint_numbers;
		return index < stops.size() ? detached(K::Stop, stops[index]) : DetachedRecord();
	};
	return ops;
}

RecordTable make_table() {
	std::vector<TableKind> kinds;
	for (const KindRow &row : kKinds) {
		TableKind kind(RecordKindRow{k(row.kind), row.token, row.label, row.add_label, row.top});
		for (const MissionField &field : mission_fields(row.record)) kind.field(labelled(field));
		switch (row.kind) {
		case K::Mission:
			kind.list({spec(K::Loadout, "Weapon loadout"),
			           file_list<bms::WeaponLoadoutRecord>(K::Loadout, loadout_of, fresh_loadout)});
			kind.list({spec(K::Availability, "Item availability"),
			           file_list<bms::ItemAvailabilityEntry>(K::Availability, availability_of, fresh_availability)});
			kind.list({spec(K::Group, "Groups", bms::kGroupRecordCount, true),
			           fixed_list<bms::GroupRecord>(K::Group, groups_of, bms::kGroupRecordCount)});
			kind.list({spec(K::Layer, "Layers", bms::kLayerRecordCount, true),
			           fixed_list<bms::LayerRecord>(K::Layer, layers_of, bms::kLayerRecordCount)});
			kind.list({spec(K::BoundingBox, "Bounding boxes"), file_list<bms::BoundingBox>(K::BoundingBox, boxes_of, fresh_box)});
			break;
		case K::WaypointPath:
			kind.list({spec(K::Stop, "Stops", kMaxWaypointPathMarkers), stop_list()});
			break;
		default: break;
		}
		kinds.push_back(std::move(kind));
	}
	return RecordTable(std::move(kinds));
}

} // namespace

const RecordTable &mission_table() {
	static const RecordTable table = make_table();
	return table;
}

std::vector<RecordHandle> mission_rows(bms::File &file) {
	std::vector<RecordHandle> rows;
	rows.push_back({k(K::Mission), &file});
	const auto each = [&rows](K kind, auto &records) {
		for (auto &record : records) rows.push_back({k(kind), &record});
	};
	each(K::Item, file.items);
	each(K::Building, file.buildings);
	each(K::Marker, file.markers);
	each(K::Organic, file.organics);
	each(K::WaypointPath, file.waypoint_records);
	each(K::Area, file.area_triggers);
	each(K::Event, file.events);
	each(K::Trigger, file.triggers);
	each(K::Action, file.actions);
	return rows;
}

} // namespace opennova::editor
