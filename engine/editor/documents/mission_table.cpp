// The mission's table (mission_table.h): the kinds, each record's fields projected from the format's
// rows (formats/mission/mission_field.h), a path's stops over bms_edit's, and the lists a record holds
// over the file's own vectors and tables.
#include "mission_table.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
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

// Each kind's row and the format's record its fields are (a stop's field is the file's: kCount).
struct KindRow {
	K kind;
	const char *token;
	const char *label;
	MissionRecord record;
};
constexpr KindRow kKinds[] = {
	{K::Mission, "mission", "Mission", MissionRecord::Header},
	{K::Loadout, "loadout", "Loadout entry", MissionRecord::Loadout},
	{K::Item, "item", "Item", MissionRecord::Entity},
	{K::Building, "building", "Building", MissionRecord::Entity},
	{K::Marker, "marker", "Marker", MissionRecord::Entity},
	{K::Organic, "organic", "Organic", MissionRecord::Entity},
	{K::WaypointPath, "waypoint_path", "Waypoint path", MissionRecord::WaypointPath},
	{K::Stop, "stop", "Stop", MissionRecord::kCount},
	{K::Group, "group", "Group", MissionRecord::Group},
	{K::Layer, "layer", "Layer", MissionRecord::Layer},
	{K::Area, "area", "Area trigger", MissionRecord::Area},
	{K::Event, "event", "Event", MissionRecord::Event},
	{K::Trigger, "trigger", "Trigger", MissionRecord::Trigger},
	{K::Action, "action", "Action", MissionRecord::Action},
};
static_assert(std::size(kKinds) == kMissionKindCount, "every MissionKind has exactly one row");

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) ++a, ++b;
	return *a == *b;
}
constexpr bool kinds_in_order() {
	for (size_t i = 0; i < std::size(kKinds); ++i) {
		if (size_t(kKinds[i].kind) != i) return false;
		for (size_t j = i + 1; j < std::size(kKinds); ++j)
			if (same_text(kKinds[i].token, kKinds[j].token)) return false;
	}
	return true;
}
static_assert(kinds_in_order(), "the kinds in MissionKind's order, each token its own");

// The pool an entity kind is.
EntityKind pool_of(K kind) {
	switch (kind) {
	case K::Building: return EntityKind::Building;
	case K::Marker: return EntityKind::Marker;
	case K::Organic: return EntityKind::Organic;
	default: return EntityKind::Item;
	}
}

// --- what the editor shows -------------------------------------------------------------------------

// The readable names of the fields, by record and key; the group a row draws a field on; a unit the
// format states (the eulers are whole degrees). A field without a row shows its key.
struct FieldLabel {
	MissionRecord record;
	const char *key;
	const char *label;
	const char *group;
	const char *unit;
};
constexpr FieldLabel kLabels[] = {
	{MissionRecord::Header, "mission_name", "Name", "", ""},
	{MissionRecord::Header, "designer", "Designer", "", ""},
	{MissionRecord::Header, "briefing", "Briefing", "", ""},
	{MissionRecord::Header, "terrain", "Terrain", "", ""},
	{MissionRecord::Header, "environment", "Environment", "", ""},
	{MissionRecord::Header, "climate", "Climate", "", ""},
	{MissionRecord::Header, "weather", "Weather", "", ""},
	{MissionRecord::Header, "mission_type", "Mission type", "", ""},
	{MissionRecord::Header, "attrib_flags", "Attributes", "", ""},
	{MissionRecord::Header, "start_time", "Start time", "", ""},
	{MissionRecord::Header, "minutes_per_day", "Minutes per day", "", ""},
	{MissionRecord::Header, "player_health", "Player health", "", ""},
	{MissionRecord::Header, "max_saves", "Saves", "", ""},
	{MissionRecord::Header, "wind_speed", "Wind speed", "", ""},
	{MissionRecord::Header, "wind_direction", "Wind direction", "", ""},
	{MissionRecord::Header, "water_override", "Water level", "", ""},
	{MissionRecord::Header, "fog_override", "Fog distance", "", ""},
	{MissionRecord::Header, "map_zoom", "Map zoom", "", ""},
	{MissionRecord::Entity, "item", "Item", "", ""},
	{MissionRecord::Entity, "x", "X", "position", ""},
	{MissionRecord::Entity, "y", "Y", "position", ""},
	{MissionRecord::Entity, "z", "Z", "position", ""},
	{MissionRecord::Entity, "pitch", "Pitch", "rotation", "°"},
	{MissionRecord::Entity, "yaw", "Yaw", "rotation", "°"},
	{MissionRecord::Entity, "roll", "Roll", "rotation", "°"},
	{MissionRecord::Entity, "group", "Group", "", ""},
	{MissionRecord::Entity, "team", "Team", "", ""},
	{MissionRecord::Entity, "ai_flags", "AI attributes", "", ""},
	{MissionRecord::Entity, "spawn_count", "Move timer", "", ""},
	{MissionRecord::Entity, "max_simultaneous", "No more than", "", ""},
	{MissionRecord::Entity, "no_less_than", "No less than", "", ""},
	{MissionRecord::Entity, "map_symbol", "Map symbol", "", ""},
	{MissionRecord::Entity, "name1", "AI class", "", ""},
	{MissionRecord::Entity, "name2", "AI script", "", ""},
	{MissionRecord::Area, "id", "Zone", "", ""},
	{MissionRecord::Area, "x_min", "X min", "x", ""},
	{MissionRecord::Area, "x_max", "X max", "x", ""},
	{MissionRecord::Area, "y_min", "Y min", "y", ""},
	{MissionRecord::Area, "y_max", "Y max", "y", ""},
	{MissionRecord::Area, "z_min", "Z min", "z", ""},
	{MissionRecord::Area, "z_max", "Z max", "z", ""},
	{MissionRecord::Event, "reset_after", "Reset after", "", ""},
	{MissionRecord::Trigger, "condition_flags", "Condition", "", ""},
	{MissionRecord::Trigger, "main_type", "Type", "", ""},
	{MissionRecord::Trigger, "sub_type", "Sub-type", "", ""},
	{MissionRecord::Action, "action_type", "Type", "", ""},
	{MissionRecord::Action, "action_sub_type", "Sub-type", "", ""},
	{MissionRecord::Loadout, "name", "Weapon", "", ""},
	{MissionRecord::Loadout, "ammo_primary", "Primary clips", "", ""},
	{MissionRecord::Loadout, "ammo_secondary", "Secondary clips", "", ""},
};

const FieldLabel *label_of(MissionRecord record, const char *key) {
	for (const FieldLabel &row : kLabels)
		if (row.record == record && same_text(row.key, key)) return &row;
	return nullptr;
}

// What a field names outside its record: the mission's terrain and environment by base name, an
// entity's item by its items.def id (as the graph's mission extraction reads them).
ReferenceKind reference_of(MissionRecord record, const char *key) {
	if (record == MissionRecord::Header && same_text(key, "terrain")) return ReferenceKind::Terrain;
	if (record == MissionRecord::Header && same_text(key, "environment")) return ReferenceKind::Environment;
	if (record == MissionRecord::Entity && same_text(key, "item")) return ReferenceKind::Item;
	return ReferenceKind::None;
}

// --- the labelled fields -----------------------------------------------------------------------------

// The editor's own check before the format's rule: a whole number inside the field's range, a text
// that fits its slot (a value past them the format would clamp, cast or cut).
bool fits(const MissionField &field, const Value &value, std::string &error) {
	if (field.type == MissionFieldType::Integer) {
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
	} else if (field.type == MissionFieldType::Text && field.width) {
		const std::string *text = std::get_if<std::string>(&value);
		if (text && text->size() > field.width) {
			error = "The text is longer than its " + std::to_string(field.width) + " bytes.";
			return false;
		}
	}
	return true;
}

FieldType field_type(const MissionField &field) {
	switch (field.type) {
	case MissionFieldType::Real: return FieldType::Real;
	case MissionFieldType::Text: return FieldType::Text;
	default:
		if (field.min < 0) return FieldType::Integer;
		return field.max <= 255 ? FieldType::Byte : FieldType::Unsigned;
	}
}

// A format row's labelled field: its schema from the row (a slot's width counts the terminator the
// editor's does, which a full slot does not write), its value through the row's own get and set.
LabelledField labelled(const MissionField &field) {
	LabelledField out;
	FieldSchema &entry = out.schema;
	entry.id = field.key;
	entry.type = field_type(field);
	entry.width = field.width ? field.width + 1 : 0;
	entry.reference = reference_of(field.record, field.key);
	if (field.type == MissionFieldType::Integer) {
		entry.ranged = true;
		entry.min = double(field.min);
		entry.max = double(field.max);
	}
	for (size_t i = 0; i < field.choice_count; ++i)
		entry.choices.push_back({field.choices[i].name, field.choices[i].value, ""});
	entry.flags = field.flags;
	// The values a choice names are the ones known; the file takes any other.
	entry.open_choices = field.choice_count != 0;
	entry.read_only = !field.set;
	entry.multiline = field.record == MissionRecord::Header && same_text(field.key, "briefing");
	if (const FieldLabel *label = label_of(field.record, field.key)) {
		entry.label = label->label;
		entry.group = label->group;
		entry.unit = label->unit;
	}
	const MissionField *row = &field;
	out.value.get = [row](const RecordHandle &record, Value &value) { return row->get(record.data, value); };
	if (field.set)
		out.value.set = [row](const RecordHandle &record, const Value &value, std::string &error) {
			return fits(*row, value, error) && row->set(record.data, value, error);
		};
	return out;
}

// The path a stop lies in (its place among the file's paths, the stop's in the path): a stop's handle
// points into its path's marker list.
bool stop_place(const RecordHandle &stop, size_t &path, size_t &index) {
	const bms::File &file = *static_cast<const bms::File *>(stop.top);
	const auto *at = static_cast<const uint32_t *>(stop.data);
	for (size_t p = 0; p < file.waypoint_records.size(); ++p) {
		const std::vector<uint32_t> &stops = file.waypoint_records[p].waypoint_numbers;
		if (!stops.empty() && at >= stops.data() && at < stops.data() + stops.size()) {
			path = p;
			index = size_t(at - stops.data());
			return true;
		}
	}
	return false;
}

// A stop's one field: the marker it visits, by its index in the file's markers (bms_edit's rule: a
// marker the file holds).
LabelledField stop_marker() {
	LabelledField out;
	FieldSchema &entry = out.schema;
	entry.id = "marker";
	entry.label = "Marker";
	entry.type = FieldType::Unsigned;
	out.value.get = [](const RecordHandle &record, Value &value) {
		value = int64_t(*static_cast<const uint32_t *>(record.data));
		return true;
	};
	out.value.set = [](const RecordHandle &record, const Value &value, std::string &error) {
		const int64_t *marker = std::get_if<int64_t>(&value);
		size_t path = 0, index = 0;
		if (!marker || !record.top || !stop_place(record, path, index)) {
			error = "A stop names a marker by its index.";
			return false;
		}
		if (*marker < 0 || *marker > INT32_MAX) {
			error = "Waypoint path marker index out of range";
			return false;
		}
		return set_waypoint_stop(*static_cast<bms::File *>(record.top), path, index, int(*marker), error);
	};
	return out;
}

// --- the lists ---------------------------------------------------------------------------------------

bms::File &file_of(const RecordHandle &record) { return *static_cast<bms::File *>(record.top); }

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

// An entity pool: a new record the format's defaults for the pool (bms_edit's make_entity, the item
// an Add names after it); a copy that comes back in with an id another entity of the file holds (a
// Duplicate's) takes the next free one, as a new entity does.
ListOps pool_list(K kind, std::vector<bms::Entity> bms::File::*pool) {
	ListOps ops;
	ops.size = [pool](const RecordHandle &owner) { return (owner.as<bms::File>().*pool).size(); };
	ops.at = [kind, pool](const RecordHandle &owner, size_t index) {
		std::vector<bms::Entity> &records = owner.as<bms::File>().*pool;
		return index < records.size() ? RecordHandle{k(kind), &records[index], owner.top} : RecordHandle{};
	};
	ops.insert = [kind, pool](const RecordHandle &owner, size_t index, const DetachedRecord *record, std::string &error) {
		if (!own_kind(record, kind, error)) return false;
		bms::File &file = owner.as<bms::File>();
		bms::Entity entity = record ? *static_cast<const bms::Entity *>(record->data.get())
		                            : make_entity(file, pool_of(kind), kItemIdOffset, EntityTransform());
		if (record && entity_id_taken(file, entity.id)) entity.id = next_entity_id(file);
		std::vector<bms::Entity> &records = file.*pool;
		records.insert(records.begin() + std::ptrdiff_t(std::min(index, records.size())), entity);
		sync_counts(file);
		return true;
	};
	ops.erase = [pool](const RecordHandle &owner, size_t index) {
		bms::File &file = owner.as<bms::File>();
		std::vector<bms::Entity> &records = file.*pool;
		if (index >= records.size()) return false;
		records.erase(records.begin() + std::ptrdiff_t(index));
		sync_counts(file);
		return true;
	};
	ops.copy = [kind, pool](const RecordHandle &owner, size_t index) {
		const std::vector<bms::Entity> &records = owner.as<bms::File>().*pool;
		return index < records.size() ? detached(kind, records[index]) : DetachedRecord();
	};
	return ops;
}

// A list of the file's own records a vector holds (the loadout, the area triggers): `fresh` the list's
// new record (null: the list takes copies only, `refusal` says why), the counts synced after each edit.
template <class Record>
ListOps file_list(K kind, std::vector<Record> &(*list)(bms::File &), bool (*fresh)(Record &, std::string &)) {
	ListOps ops;
	ops.size = [list](const RecordHandle &owner) { return list(owner.as<bms::File>()).size(); };
	ops.at = [kind, list](const RecordHandle &owner, size_t index) {
		std::vector<Record> &records = list(owner.as<bms::File>());
		return index < records.size() ? RecordHandle{k(kind), &records[index], owner.top} : RecordHandle{};
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
std::vector<bms::AreaTrigger> &areas_of(bms::File &file) { return file.area_triggers; }
std::vector<bms::WaypointRecord> &paths_of(bms::File &file) { return file.waypoint_records; }
std::vector<bms::GroupRecord> &groups_of(bms::File &file) { return file.group_records; }
std::vector<bms::LayerRecord> &layers_of(bms::File &file) { return file.layer_records; }

// The loadout chunk ends at a nameless entry, so a new entry has no name to be written with: an entry
// comes in as a copy (a Duplicate, a paste).
bool fresh_loadout(bms::WeaponLoadoutRecord &, std::string &error) {
	error = "A weapon loadout entry needs a weapon's name: duplicate or paste one.";
	return false;
}
// A new zone: every field zero (the editor sets its id, bounds and flags).
bool fresh_area(bms::AreaTrigger &, std::string &) { return true; }

// A fixed table of the file (its 128 waypoint paths, 64 groups, 32 layers): its records set, never
// added or removed (the writer writes exactly that many).
template <class Record> ListOps fixed_list(K kind, std::vector<Record> &(*list)(bms::File &), size_t count) {
	ListOps ops = file_list<Record>(kind, list, nullptr);
	ops.insert = [count](const RecordHandle &, size_t, const DetachedRecord *, std::string &error) {
		error = "A mission holds exactly " + std::to_string(count) + " of these.";
		return false;
	};
	ops.erase = [](const RecordHandle &, size_t) { return false; };
	return ops;
}

// A path's stops: the markers it visits, through bms_edit's stops (32 at most, each a marker the file
// holds). A stop comes in as a copy: a new one would name no marker.
ListOps stop_list() {
	ListOps ops;
	const auto path_index = [](const RecordHandle &owner) {
		return size_t(&owner.as<bms::WaypointRecord>() - file_of(owner).waypoint_records.data());
	};
	ops.size = [](const RecordHandle &owner) { return owner.as<bms::WaypointRecord>().waypoint_numbers.size(); };
	ops.at = [](const RecordHandle &owner, size_t index) {
		std::vector<uint32_t> &stops = owner.as<bms::WaypointRecord>().waypoint_numbers;
		return index < stops.size() ? RecordHandle{k(K::Stop), &stops[index], owner.top} : RecordHandle{};
	};
	ops.insert = [path_index](const RecordHandle &owner, size_t index, const DetachedRecord *record,
	                          std::string &error) {
		if (!own_kind(record, K::Stop, error)) return false;
		if (!record) {
			error = "A stop names a marker of the mission: duplicate or paste one.";
			return false;
		}
		const uint32_t marker = *static_cast<const uint32_t *>(record->data.get());
		return insert_waypoint_stop(file_of(owner), path_index(owner), index, int(marker), error);
	};
	ops.erase = [path_index](const RecordHandle &owner, size_t index) {
		std::string error;
		return erase_waypoint_stop(file_of(owner), path_index(owner), index, error);
	};
	ops.copy = [](const RecordHandle &owner, size_t index) {
		const std::vector<uint32_t> &stops = owner.as<bms::WaypointRecord>().waypoint_numbers;
		return index < stops.size() ? detached(K::Stop, stops[index]) : DetachedRecord();
	};
	return ops;
}

// An event held apart with its chain: the event and copies of its triggers and actions, which go back
// in with it at the end of the file's tables.
struct EventCopy {
	bms::Event event{};
	std::vector<bms::Trigger> triggers;
	std::vector<bms::Action> actions;
};

ListOps event_list() {
	ListOps ops;
	ops.size = [](const RecordHandle &owner) { return owner.as<bms::File>().events.size(); };
	ops.at = [](const RecordHandle &owner, size_t index) {
		std::vector<bms::Event> &events = owner.as<bms::File>().events;
		return index < events.size() ? RecordHandle{k(K::Event), &events[index], owner.top} : RecordHandle{};
	};
	ops.insert = [](const RecordHandle &owner, size_t index, const DetachedRecord *record, std::string &error) {
		if (!own_kind(record, K::Event, error)) return false;
		const EventCopy fresh;
		const EventCopy &made = record ? *static_cast<const EventCopy *>(record->data.get()) : fresh;
		return insert_event(owner.as<bms::File>(), index, made.event, made.triggers, made.actions, error);
	};
	ops.erase = [](const RecordHandle &owner, size_t index) {
		std::string error;
		return erase_event(owner.as<bms::File>(), index, error);
	};
	ops.copy = [](const RecordHandle &owner, size_t index) {
		const bms::File &file = owner.as<bms::File>();
		if (index >= file.events.size()) return DetachedRecord();
		auto copy = std::make_shared<EventCopy>();
		copy->event = file.events[index];
		size_t first = 0, count = 0;
		if (event_trigger_range(file, index, first, count))
			copy->triggers.assign(file.triggers.begin() + std::ptrdiff_t(first),
			                      file.triggers.begin() + std::ptrdiff_t(first + count));
		if (event_action_range(file, index, first, count))
			copy->actions.assign(file.actions.begin() + std::ptrdiff_t(first),
			                     file.actions.begin() + std::ptrdiff_t(first + count));
		DetachedRecord out;
		out.kind = k(K::Event);
		out.data = std::move(copy);
		return out;
	};
	return ops;
}

// An event's triggers or actions: its range of the file's table (none where the range lies past the
// table), edited through bms_edit's chain edits, which keep every other event's range on its records.
template <class Record>
ListOps chain_list(K kind, std::vector<Record> bms::File::*table,
                   bool (*range)(const bms::File &, size_t, size_t &, size_t &),
                   bool (*insert)(bms::File &, size_t, size_t, const Record &, std::string &),
                   bool (*remove)(bms::File &, size_t, size_t, std::string &)) {
	const auto event_index = [](const RecordHandle &owner) {
		return size_t(&owner.as<bms::Event>() - file_of(owner).events.data());
	};
	ListOps ops;
	ops.size = [range, event_index](const RecordHandle &owner) {
		size_t first = 0, count = 0;
		return range(file_of(owner), event_index(owner), first, count) ? count : size_t(0);
	};
	ops.at = [kind, table, range, event_index](const RecordHandle &owner, size_t index) {
		size_t first = 0, count = 0;
		bms::File &file = file_of(owner);
		if (!range(file, event_index(owner), first, count) || index >= count) return RecordHandle{};
		return RecordHandle{k(kind), &(file.*table)[first + index], owner.top};
	};
	ops.insert = [kind, insert, event_index](const RecordHandle &owner, size_t index, const DetachedRecord *record,
	                                         std::string &error) {
		if (!own_kind(record, kind, error)) return false;
		const Record made = record ? *static_cast<const Record *>(record->data.get()) : Record{};
		const size_t event = event_index(owner);
		const bms::Event &ev = file_of(owner).events[event];
		const size_t count = kind == K::Trigger ? ev.trigger_count : ev.action_count;
		return insert(file_of(owner), event, std::min(index, count), made, error);
	};
	ops.erase = [remove, event_index](const RecordHandle &owner, size_t index) {
		std::string error;
		return remove(file_of(owner), event_index(owner), index, error);
	};
	ops.copy = [kind, table, range, event_index](const RecordHandle &owner, size_t index) {
		size_t first = 0, count = 0;
		const bms::File &file = file_of(owner);
		if (!range(file, event_index(owner), first, count) || index >= count) return DetachedRecord();
		return detached(kind, (file.*table)[first + index]);
	};
	return ops;
}

bool insert_trigger(bms::File &file, size_t event, size_t index, const bms::Trigger &trigger, std::string &error) {
	return insert_event_trigger(file, event, index, trigger, error);
}
bool insert_action(bms::File &file, size_t event, size_t index, const bms::Action &action, std::string &error) {
	return insert_event_action(file, event, index, action, error);
}

// The chain-entry ceiling the inserts keep (bms_edit's).
constexpr size_t kChainMax = 20;

RecordTable make_table() {
	std::vector<TableKind> kinds;
	for (const KindRow &row : kKinds) {
		RecordKindRow kind_row;
		kind_row.kind = k(row.kind);
		kind_row.token = row.token;
		kind_row.label = row.label;
		kind_row.top = row.kind == K::Mission;
		TableKind kind(kind_row);
		if (row.kind == K::Stop) kind.field(stop_marker());
		else
			for (const MissionField &field : mission_fields(row.record)) kind.field(labelled(field));
		switch (row.kind) {
		case K::Mission:
			kind.list({spec(K::Loadout, "Weapon loadout"), file_list<bms::WeaponLoadoutRecord>(K::Loadout, loadout_of, fresh_loadout)});
			kind.list({spec(K::Item, "Items"), pool_list(K::Item, &bms::File::items)});
			kind.list({spec(K::Building, "Buildings"), pool_list(K::Building, &bms::File::buildings)});
			kind.list({spec(K::Marker, "Markers"), pool_list(K::Marker, &bms::File::markers)});
			kind.list({spec(K::Organic, "Organics"), pool_list(K::Organic, &bms::File::organics)});
			kind.list({spec(K::WaypointPath, "Waypoint paths", bms::kWaypointRecordCount, true),
			           fixed_list<bms::WaypointRecord>(K::WaypointPath, paths_of, bms::kWaypointRecordCount)});
			kind.list({spec(K::Group, "Groups", bms::kGroupRecordCount, true),
			           fixed_list<bms::GroupRecord>(K::Group, groups_of, bms::kGroupRecordCount)});
			kind.list({spec(K::Layer, "Layers", bms::kLayerRecordCount, true),
			           fixed_list<bms::LayerRecord>(K::Layer, layers_of, bms::kLayerRecordCount)});
			kind.list({spec(K::Area, "Area triggers"), file_list<bms::AreaTrigger>(K::Area, areas_of, fresh_area)});
			kind.list({spec(K::Event, "Events"), event_list()});
			break;
		case K::WaypointPath:
			kind.list({spec(K::Stop, "Stops", kMaxWaypointPathMarkers), stop_list()});
			break;
		case K::Event:
			kind.list({spec(K::Trigger, "Triggers", kChainMax),
			           chain_list<bms::Trigger>(K::Trigger, &bms::File::triggers, event_trigger_range, insert_trigger,
			                                    remove_event_trigger)});
			kind.list({spec(K::Action, "Actions", kChainMax),
			           chain_list<bms::Action>(K::Action, &bms::File::actions, event_action_range, insert_action,
			                                   remove_event_action)});
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

RecordHandle mission_record(bms::File &file) { return RecordHandle{k(K::Mission), &file, &file}; }

} // namespace opennova::editor
