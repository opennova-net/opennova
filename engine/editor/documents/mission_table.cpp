// The mission's table (mission_table.h): the kinds, each record's fields projected from the format's
// rows (formats/mission/mission_field.h) with the editor's check before them, what each field names
// (a file, a symbol, a record of its own file by its index or by its id) and whether the game reads it
// on its record, and the lists a record holds over its native struct's own vectors.
#include "mission_table.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <base/io/strutil.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_field.h>
#include <formats/mission/mission_params.h>

namespace opennova::editor {
using namespace mission;

namespace {

using K = MissionKind;
constexpr NodeKind k(K kind) { return node_kind(kind); }

// --- the kinds -------------------------------------------------------------------------------------

// The format's record a kind's fields are fields of, inside the kind's native record: the mission
// row's header in its file, a path's record beside its number, an event's record in its chain; any
// other kind's native record is the format's own.
void *self(void *native) { return native; }
void *header_of(void *native) { return &static_cast<bms::File *>(native)->header; }
void *path_record(void *native) { return &static_cast<MissionPath *>(native)->record; }
void *chain_event(void *native) { return &static_cast<EventChain *>(native)->event; }

// Each kind's row: its token and label, what the outline's Add of a row of it says ("" = none: the
// mission is one per file, its 128 paths are fixed, a nested record is added through its list),
// whether it is a row of the file, the format's record its fields are, and where that record is in
// the kind's native one.
struct KindRow {
	K kind;
	const char *token;
	const char *label;
	const char *add_label;
	bool top;
	MissionRecord record;
	void *(*fields_of)(void *native);
};
constexpr KindRow kKinds[] = {
	{K::Mission, "mission", "Mission", "", true, MissionRecord::Header, header_of},
	{K::Loadout, "loadout", "Loadout entry", "", false, MissionRecord::Loadout, self},
	{K::Availability, "availability", "Item availability rule", "", false, MissionRecord::Availability, self},
	{K::Group, "group", "Group", "", false, MissionRecord::Group, self},
	{K::Layer, "layer", "Layer", "", false, MissionRecord::Layer, self},
	{K::BoundingBox, "bounding_box", "Bounding box", "", false, MissionRecord::BoundingBox, self},
	{K::Item, "item", "Item", "Add item", true, MissionRecord::Entity, self},
	{K::Building, "building", "Building", "Add building", true, MissionRecord::Entity, self},
	{K::Marker, "marker", "Marker", "Add marker", true, MissionRecord::Entity, self},
	{K::Organic, "organic", "Organic", "Add organic", true, MissionRecord::Entity, self},
	{K::WaypointPath, "waypoint_path", "Waypoint path", "", true, MissionRecord::WaypointPath, path_record},
	{K::Stop, "stop", "Stop", "", false, MissionRecord::Stop, self},
	{K::Area, "area", "Area trigger", "Add area trigger", true, MissionRecord::Area, self},
	{K::Event, "event", "Event", "Add event", true, MissionRecord::Event, chain_event},
	{K::Trigger, "trigger", "Trigger", "", false, MissionRecord::Trigger, self},
	{K::Action, "action", "Action", "", false, MissionRecord::Action, self},
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
// format states (the eulers are whole degrees); a colour the number packs. A field without a row goes
// by its key in words (worded()).
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
	// The original editor's cnv_file: the dialog bank's name where it holds one [orig: DialogSystem_Init @0x52760c].
	{MissionRecord::Header, "cnv_file", "Dialog bank", "terrain", ""},
	{MissionRecord::Header, "tt_file", "TT file", "terrain", ""},
	{MissionRecord::Header, "terrain_tile", "Terrain tile set", "", ""},
	{MissionRecord::Header, "default_str", "Default string", "", ""},
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
	{MissionRecord::Entity, "id", "SSN", "", ""},
	{MissionRecord::Entity, "x", "X", "position", ""},
	{MissionRecord::Entity, "y", "Y", "position", ""},
	{MissionRecord::Entity, "z", "Z", "position", ""},
	{MissionRecord::Entity, "pitch", "Pitch", "rotation", "°"},
	{MissionRecord::Entity, "yaw", "Yaw", "rotation", "°"},
	{MissionRecord::Entity, "roll", "Roll", "rotation", "°"},
	{MissionRecord::Entity, "group", "Group", "", ""},
	{MissionRecord::Entity, "waypoint_id", "Waypoint list", "waypoint", ""},
	{MissionRecord::Entity, "wp_number", "Waypoint number", "waypoint", ""},
	{MissionRecord::Entity, "wp_distance", "Waypoint distance", "", ""},
	{MissionRecord::Entity, "wp_adv_trigger", "Waypoint advance trigger", "", ""},
	{MissionRecord::Entity, "wpgoal0", "Waypoint goal 1", "", ""},
	{MissionRecord::Entity, "wpgoal1", "Waypoint goal 2", "", ""},
	{MissionRecord::Entity, "wpgoal2", "Waypoint goal 3", "", ""},
	{MissionRecord::Entity, "wpgoal3", "Waypoint goal 4", "", ""},
	{MissionRecord::Entity, "group_rel", "Group relation", "", ""},
	{MissionRecord::Entity, "team", "Team", "", ""},
	{MissionRecord::Entity, "lfp_group", "LFP group", "", ""},
	{MissionRecord::Entity, "ai_flags", "AI attributes", "", ""},
	{MissionRecord::Entity, "perfectionist2", "Perfectionist", "", ""},
	{MissionRecord::Entity, "accuracy", "Accuracy", "accuracy", ""},
	{MissionRecord::Entity, "w_accuracy2", "Accuracy (second)", "accuracy", ""},
	{MissionRecord::Entity, "spawn_count", "Move timer", "timers", ""},
	{MissionRecord::Entity, "crouch_timer", "Crouch timer", "timers", ""},
	{MissionRecord::Entity, "shoot_timer", "Shoot timer", "timers", ""},
	{MissionRecord::Entity, "advancetimer", "Advance timer", "timers", ""},
	{MissionRecord::Entity, "weapon_type", "Weapon type", "", ""},
	{MissionRecord::Entity, "sweapon_type", "Secondary weapon type", "", ""},
	{MissionRecord::Entity, "max_simultaneous", "No more than", "", ""},
	{MissionRecord::Entity, "no_less_than", "No less than", "", ""},
	{MissionRecord::Entity, "color_override", "Colour override", "", ""},
	{MissionRecord::Entity, "ref_num", "Reference number", "", ""},
	{MissionRecord::Entity, "next_ssn", "Next SSN", "", ""},
	// A waypoint marker's name id: STRWPNAME%03i in the mission text's WPNames [orig:
	// Entity_SpawnFromBMSRecord @0x40f0aa..0x40f0e0; HUD_GetWaypointName @0x594630].
	{MissionRecord::Entity, "ttool_index", "Waypoint name", "", ""},
	{MissionRecord::Entity, "blink_parent_a", "Blink parent A", "", ""},
	{MissionRecord::Entity, "blink_parent_b", "Blink parent B", "", ""},
	{MissionRecord::Entity, "blink_group_a", "Blink group A", "", ""},
	{MissionRecord::Entity, "blink_group_b", "Blink group B", "", ""},
	{MissionRecord::Entity, "name1", "AI class", "", ""},
	{MissionRecord::Entity, "name2", "AI script", "", ""},
	{MissionRecord::Entity, "gen_string", "Generator string", "", ""},
	{MissionRecord::WaypointPath, "marker_count", "Stored count", "", ""},
	{MissionRecord::Area, "id", "Zone", "", ""},
	{MissionRecord::Area, "x_min", "X min", "x", ""},
	{MissionRecord::Area, "x_max", "X max", "x", ""},
	{MissionRecord::Area, "y_min", "Y min", "y", ""},
	{MissionRecord::Area, "y_max", "Y max", "y", ""},
	{MissionRecord::Area, "z_min", "Z min", "z", ""},
	{MissionRecord::Area, "z_max", "Z max", "z", ""},
	{MissionRecord::Trigger, "condition_flags", "Condition", "", ""},
	{MissionRecord::Trigger, "main_type", "Type", "", ""},
	{MissionRecord::Trigger, "sub_type", "Sub-type", "", ""},
	{MissionRecord::Trigger, "param1", "Parameter 1", "", ""},
	{MissionRecord::Trigger, "param2", "Parameter 2", "", ""},
	{MissionRecord::Trigger, "param3", "Parameter 3", "", ""},
	{MissionRecord::Trigger, "param4", "Parameter 4", "", ""},
	{MissionRecord::Action, "action_type", "Type", "", ""},
	{MissionRecord::Action, "action_sub_type", "Sub-type", "", ""},
	{MissionRecord::Action, "param1", "Parameter 1", "", ""},
	{MissionRecord::Action, "param2", "Parameter 2", "", ""},
	{MissionRecord::Action, "param3", "Parameter 3", "", ""},
	{MissionRecord::Action, "param4", "Parameter 4", "", ""},
	{MissionRecord::Loadout, "name", "Weapon", "", ""},
	{MissionRecord::Loadout, "ammo_primary", "Primary clips", "", ""},
	{MissionRecord::Loadout, "ammo_secondary", "Secondary clips", "", ""},
	{MissionRecord::Loadout, "flags", "Damage class", "", ""},
	{MissionRecord::Availability, "name", "Weapon", "", ""},
	{MissionRecord::BoundingBox, "min_x", "Min X", "min", ""},
	{MissionRecord::BoundingBox, "min_y", "Min Y", "min", ""},
	{MissionRecord::BoundingBox, "min_z", "Min Z", "min", ""},
	{MissionRecord::BoundingBox, "max_x", "Max X", "max", ""},
	{MissionRecord::BoundingBox, "max_y", "Max Y", "max", ""},
	{MissionRecord::BoundingBox, "max_z", "Max Z", "max", ""},
	{MissionRecord::BoundingBox, "ref_id", "Refers to", "", ""},
};

const FieldLabel *label_of(MissionRecord record, const char *key) {
	for (const FieldLabel &row : kLabels)
		if (row.record == record && same_text(row.key, key)) return &row;
	return nullptr;
}

// A choice of a field in a modder's words, where a row gives them: its label and what the game does
// with it, cited (the tooltip of its box). The others show as their name.
struct ChoiceWords {
	MissionRecord record;
	const char *key;
	const char *name;
	const char *label;
	const char *meaning;
};
constexpr ChoiceWords kChoiceWords[] = {
	// The spawn sets the entity's flag 0x1000000, which the terrain's static shadow pass reads
	// (render-lighting-re.md "Static sector/model sun shadows"); the render slot never does.
	{MissionRecord::Entity, "ai_flags", "NoShadow", "No shadow",
	 "The game draws no sun shadow of this placement on the terrain: the terrain's static shadow pass "
	 "skips it, whatever its item (the item's own NoShadow skips every placement of it). A person's or "
	 "a DynamicShadow item's moving shadow still draws, and its own lighting is unchanged. "
	 "[orig: Entity_SpawnFromBMSRecord @ 0x40ED2E; Terrain_CollectAndRenderTileModels @ 0x60D42F]"},
};

const ChoiceWords *choice_words_of(MissionRecord record, const char *key, const char *name) {
	for (const ChoiceWords &row : kChoiceWords)
		if (row.record == record && same_text(row.key, key) && same_text(row.name, name)) return &row;
	return nullptr;
}

// A key in words: "win_conditions[2]" is "Win conditions 3", "lfp_group" "Lfp group".
std::string worded(const char *key) {
	std::string out;
	for (const char *c = key; *c; ++c) {
		if (*c == '[') {
			out += ' ' + std::to_string(strutil::parse_int(c + 1).value_or(0) + 1);
			break;
		}
		out += *c == '_' ? ' ' : *c;
	}
	if (!out.empty()) out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
	return out;
}

// What a field names outside its record, whatever the record holds: the mission's terrain and
// environment by base name, its tile set as the texture the terrain's atlas is; an entity's item by
// its items.def id, its AI script as the profile of that name, its group and its waypoint path by
// their index in the file's fixed tables; a stop's marker by its index in the file's markers; a
// loadout entry's and an availability rule's weapon by name. And what a field's record is named by:
// an entity by its SSN, an area trigger by its zone id. A trigger's and an action's first parameter is
// declared the event it may name (the kind the record's type decides, by_record), so the events are a
// collection the core renumbers.
struct ReferenceRow {
	MissionRecord record;
	const char *key;
	ReferenceKind reference;
	ReferenceKind defines = ReferenceKind::None;
};
constexpr ReferenceRow kReferences[] = {
	// [orig: the terrain slot read to its NUL, Environment_LoadTimeOfDayConfig @0x57db30]
	{MissionRecord::Header, "terrain", ReferenceKind::Terrain},
	{MissionRecord::Header, "environment", ReferenceKind::Environment},
	// [orig: g_BmsTileSetName @0xA762E8 read by Terrain_LoadEnvironmentConfig @0x6109C8, its extension
	// replaced by .TGA: Path_ReplaceOrAppendExtension @0x53C780 (formats/trn trn_mission_tilestrip)]. The
	// .TSD the name also gives is not checked.
	{MissionRecord::Header, "terrain_tile", ReferenceKind::Texture},
	{MissionRecord::Entity, "item", ReferenceKind::Item},
	// [orig: the "%s.aip" profile name @0x7c6e9c, runtime/mission runtime_boot's ai_profile_name_for]
	{MissionRecord::Entity, "name2", ReferenceKind::AiProfile},
	{MissionRecord::Entity, "group", ReferenceKind::MissionGroup},
	{MissionRecord::Entity, "waypoint_id", ReferenceKind::MissionPath},
	{MissionRecord::Entity, "id", ReferenceKind::None, ReferenceKind::MissionEntity},
	{MissionRecord::Stop, "marker", ReferenceKind::MissionMarker},
	{MissionRecord::Area, "id", ReferenceKind::None, ReferenceKind::MissionZone},
	{MissionRecord::Trigger, "param1", ReferenceKind::MissionEvent},
	{MissionRecord::Action, "param1", ReferenceKind::MissionEvent},
	// [orig: Mission_LoadBMSFile compares each name with g_WeaponDefTable by stricmp and drops a record
	// that matches none]
	{MissionRecord::Loadout, "name", ReferenceKind::Weapon},
	{MissionRecord::Availability, "name", ReferenceKind::Weapon},
};

const ReferenceRow *reference_of(MissionRecord record, const char *key) {
	for (const ReferenceRow &row : kReferences)
		if (row.record == record && same_text(row.key, key)) return &row;
	return nullptr;
}

// The members no runtime reader is witnessed for (Unverified), and those the game is witnessed never
// to read (Ignored), whatever their record holds. (An entity's name_index is read in every pool: the
// spawn takes each pool's records and reads it for each [orig: Entity_SpawnFromBMSRecord
// @0x40ecbf..0x40ed0a, the rec+4 gate; runtime/mission/promote.cpp].)
struct AppliesRow {
	MissionRecord record;
	const char *key;
	Applicability applies;
};
constexpr AppliesRow kApplies[] = {
	{MissionRecord::Header, "default_str", Applicability::Unverified},
	{MissionRecord::Header, "tt_file", Applicability::Unverified},
	{MissionRecord::Header, "music", Applicability::Unverified},
	{MissionRecord::Header, "reverb", Applicability::Unverified},
	// The loader keeps a group's words 0, 2 and 3 [orig: Mission_LoadBMSFile]; what reads them is open
	// (docs/mission/bms-event-runtime-re.md section 3a).
	{MissionRecord::Group, "flags", Applicability::Unverified},
	{MissionRecord::Group, "value", Applicability::Unverified},
	// Read and discarded [orig editor: Med_WriteBmsFile @0x44f920 writes the layer's name].
	{MissionRecord::Layer, "name", Applicability::Unverified},
	{MissionRecord::Entity, "next_ssn", Applicability::Unverified},
	// Read on a waypoint marker alone (type 6005 or 6006, MissionDocument::refine_field).
	{MissionRecord::Entity, "ttool_index", Applicability::Unverified},
	{MissionRecord::Entity, "color_override", Applicability::Unverified},
	{MissionRecord::Entity, "team_budget", Applicability::Unverified},
	// The spawn never reads the record's bytes 84..87 [orig: Entity_SpawnFromBMSRecord @0x40e9f0;
	// docs/correspondence.md].
	{MissionRecord::Entity, "blink_parent_a", Applicability::Ignored},
	{MissionRecord::Entity, "blink_parent_b", Applicability::Ignored},
	{MissionRecord::Entity, "blink_group_a", Applicability::Ignored},
	{MissionRecord::Entity, "blink_group_b", Applicability::Ignored},
	// What a bounding box's type and the id it refers to are to the game is not witnessed (D-MIS-8).
	{MissionRecord::BoundingBox, "type", Applicability::Unverified},
	{MissionRecord::BoundingBox, "ref_id", Applicability::Unverified},
};

Applicability applies_of(MissionRecord record, const char *key) {
	for (const AppliesRow &row : kApplies)
		if (row.record == record && same_text(row.key, key)) return row.applies;
	return Applicability::Reads;
}

// An event's runs are what the writer derives from the triggers and actions the event holds
// (formats/mission/mission_chains.h): no field of the event.
constexpr const char *kDerived[] = {"trigger_index", "trigger_count", "action_index", "action_count"};
bool derived(MissionRecord record, const char *key) {
	if (record != MissionRecord::Event) return false;
	for (const char *name : kDerived)
		if (same_text(name, key)) return true;
	return false;
}

// --- what a record's type decides ---------------------------------------------------------------------

// The reference a parameter of the kind makes: a record of the file's by its index, or by its id; none
// for a number.
ReferenceKind reference_of_param(ParamKind kind) {
	switch (kind) {
	case ParamKind::Group: return ReferenceKind::MissionGroup;
	case ParamKind::Entity: return ReferenceKind::MissionEntity;
	case ParamKind::Zone: return ReferenceKind::MissionZone;
	case ParamKind::Event: return ReferenceKind::MissionEvent;
	case ParamKind::Path: return ReferenceKind::MissionPath;
	default: return ReferenceKind::None;
	}
}

// A parameter's slot (0 for param1), or -1 for another key.
int param_slot(const char *key) {
	return key[0] == 'p' && key[1] == 'a' && key[2] == 'r' && key[3] == 'a' && key[4] == 'm' && key[5] >= '1' &&
	                       key[5] <= '4' && !key[6]
	               ? key[5] - '1'
	               : -1;
}

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
			// The word's range named by its constants (bms.h), never spelled again here.
			char range[64];
			std::snprintf(range, sizeof(range), "%.10g to %.10g", bms::kFixed16Min, bms::kFixed16Max);
			error = std::string("A 16.16 number in mission units is ") + range + ".";
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
// editor's does, which a full slot does not write), its value through the row's own get and set on
// the format's record inside the kind's native one, and what its record's type makes of it.
LabelledField labelled(const KindRow &kind, const MissionField &field) {
	LabelledField out;
	FieldSchema &entry = out.schema;
	entry.id = field.key;
	entry.type = field_type(field);
	entry.width = field.width ? field.width + 1 : 0;
	if (const ReferenceRow *names = reference_of(field.record, field.key)) {
		entry.reference = names->reference;
		entry.defines = names->defines;
	}
	entry.applies = applies_of(field.record, field.key);
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
	for (size_t i = 0; i < field.choice_count; ++i) {
		FieldChoice choice{field.choices[i].name, field.choices[i].value, ""};
		if (const ChoiceWords *words = choice_words_of(field.record, field.key, field.choices[i].name)) {
			choice.label = words->label;
			choice.description = words->meaning;
		}
		entry.choices.push_back(std::move(choice));
	}
	entry.flags = field.flags;
	entry.open_choices = field.open;
	entry.read_only = !field.set;
	entry.multiline = field.record == MissionRecord::Header && same_text(field.key, "briefing");
	if (const FieldLabel *label = label_of(field.record, field.key)) {
		entry.label = label->label;
		entry.group = label->group;
		entry.unit = label->unit;
		entry.color = label->color;
	} else {
		entry.label = worded(field.key);
	}
	// The format's value is the editor's (MissionValue and Value are one variant).
	static_assert(std::is_same_v<MissionValue, Value>, "a mission field's value is the editor's");
	const MissionField *row = &field;
	void *(*const fields_of)(void *) = kind.fields_of;
	out.value.get = [row, fields_of](const RecordHandle &record, Value &value) {
		return row->get(fields_of(record.data), value);
	};
	if (field.set)
		out.value.set = [row, fields_of](const RecordHandle &record, const Value &value, std::string &error) {
			return fits(*row, value, error) && row->set(fields_of(record.data), value, error);
		};
	// A loadout entry's fourth string, which a record may leave out (bms::WeaponLoadoutRecord::
	// has_flags): left out, the game reads the "-1" its sanitizer inserts [orig:
	// AIProfile_SanitizeConfigData @ 0x40cfe0] (what a reload reads), while the record keeps the
	// value it held latent (ADR 0002: a Write writes it again), and a Set of it writes it.
	if (field.record == MissionRecord::Loadout && same_text(field.key, "flags")) {
		entry.optional = true;
		out.value.present = [](const RecordHandle &record) { return record.as<bms::WeaponLoadoutRecord>().has_flags; };
		out.value.set_present = [](const RecordHandle &record, bool present, std::string &) {
			record.as<bms::WeaponLoadoutRecord>().has_flags = present;
			return true;
		};
	}
	// A trigger's and an action's parameter: what it names and whether the game reads it, by the
	// record's type [orig: EventTrigger_EvaluateCondition @0x453620, EventAction_Dispatch @0x4542e0
	// read the parameters formats/mission/mission_params.h lists].
	const int slot = param_slot(field.key);
	if (slot >= 0 && field.record == MissionRecord::Trigger) {
		entry.open_choices = true; // a parameter that takes named values takes any other typed
		out.reference = [slot](const RecordHandle &record, const RecordOwners &) {
			return reference_of_param(trigger_param_kind(record.as<bms::Trigger>(), slot));
		};
		out.applies = [slot](const RecordHandle &record, const RecordOwners &) {
			return trigger_param_kind(record.as<bms::Trigger>(), slot) == ParamKind::Unused ? Applicability::Ignored
			                                                                                : Applicability::Reads;
		};
	}
	if (slot >= 0 && field.record == MissionRecord::Action) {
		entry.open_choices = true;
		out.reference = [slot](const RecordHandle &record, const RecordOwners &) {
			return reference_of_param(action_param_kind(record.as<bms::Action>(), slot));
		};
		out.applies = [slot](const RecordHandle &record, const RecordOwners &) {
			return action_param_kind(record.as<bms::Action>(), slot) == ParamKind::Unused ? Applicability::Ignored
			                                                                              : Applicability::Reads;
		};
	}
	if (field.record == MissionRecord::Trigger && same_text(field.key, "sub_type")) {
		entry.open_choices = true;
		// Event's and SecondTimeThrough's cases never read it (mission_params.h).
		out.applies = [](const RecordHandle &record, const RecordOwners &) {
			return trigger_reads_sub_type(int32_t(record.as<bms::Trigger>().main_type)) ? Applicability::Reads
			                                                                           : Applicability::Ignored;
		};
	}
	if (field.record == MissionRecord::Action && same_text(field.key, "action_sub_type")) entry.open_choices = true;
	if (field.record == MissionRecord::Entity) {
		// The number beside a waypoint list that is a command 123..125 is the SSN of the entity to go
		// to and board [orig: Entity_UpdateInfantryAI @0x4ba9ad]; beside a path it is the stop to start
		// at.
		if (same_text(field.key, "wp_number"))
			out.reference = [](const RecordHandle &record, const RecordOwners &) {
				return path_command_names_entity(record.as<bms::Entity>().waypoint_id) ? ReferenceKind::MissionEntity
				                                                                       : ReferenceKind::None;
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

// A list of the mission row's own records a vector of its file holds (the loadout, the availability
// rules, the bounding boxes): `fresh` the list's new record (false with why where it has none to be
// written with), `max` the most it holds (0: any number), the header's counts and chunk lengths synced
// after each edit.
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
// A bounding box holds a type (1 or 5 in the shipped missions) and the id of what it refers to, and
// what the game makes of either is not witnessed (D-MIS-8): a new one would be a record nothing says
// how to fill, so one comes in as a copy.
bool fresh_box(bms::BoundingBox &, std::string &error) {
	error = "What a bounding box's type and the id it refers to mean is not known yet: duplicate or paste one.";
	return false;
}

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
// count as its slots). A new stop visits the file's first marker until it is given another (its
// marker is a Record reference: the picker offers the file's markers).
// A stop put into or taken out of the path's record, its count written as its slots and the slot bytes
// past them zero, 32 at most: the record's own edit, until the editor takes master's model of a path as its
// waypoint markers (mission::insert_waypoint_stop over the file, D-MIS-6; the flow lane's #992).
bool insert_record_stop(bms::WaypointRecord &path, size_t index, uint32_t marker, std::string &error) {
	std::vector<uint32_t> &stops = path.waypoint_numbers;
	if (stops.size() >= kMaxWaypointPathMarkers) {
		error = "Waypoint path marker count exceeds 32";
		return false;
	}
	stops.insert(stops.begin() + std::ptrdiff_t(std::min(index, stops.size())), marker);
	path.marker_count = uint32_t(stops.size());
	path.padding.assign(128 - stops.size() * sizeof(uint32_t), 0);
	return true;
}
bool erase_record_stop(bms::WaypointRecord &path, size_t index) {
	std::vector<uint32_t> &stops = path.waypoint_numbers;
	if (index >= stops.size()) return false;
	stops.erase(stops.begin() + std::ptrdiff_t(index));
	path.marker_count = uint32_t(stops.size());
	path.padding.assign(128 - stops.size() * sizeof(uint32_t), 0);
	return true;
}

ListOps stop_list() {
	ListOps ops;
	ops.size = [](const RecordHandle &owner) { return owner.as<MissionPath>().record.waypoint_numbers.size(); };
	ops.at = [](const RecordHandle &owner, size_t index) {
		std::vector<uint32_t> &stops = owner.as<MissionPath>().record.waypoint_numbers;
		return index < stops.size() ? RecordHandle{k(K::Stop), &stops[index]} : RecordHandle{};
	};
	ops.insert = [](const RecordHandle &owner, size_t index, const DetachedRecord *record, std::string &error) {
		if (!own_kind(record, K::Stop, error)) return false;
		const uint32_t marker = record ? *static_cast<const uint32_t *>(record->data.get()) : 0;
		return insert_record_stop(owner.as<MissionPath>().record, index, marker, error);
	};
	ops.erase = [](const RecordHandle &owner, size_t index) {
		return erase_record_stop(owner.as<MissionPath>().record, index);
	};
	ops.copy = [](const RecordHandle &owner, size_t index) {
		const std::vector<uint32_t> &stops = owner.as<MissionPath>().record.waypoint_numbers;
		return index < stops.size() ? detached(K::Stop, stops[index]) : DetachedRecord();
	};
	return ops;
}

// A new trigger or action: every word zero but its four parameters, each -1 as every shipped record
// holds a parameter its type does not read (kUnreadParam); its type set by the Add's field, the
// parameters its type reads by the logic's add (mission_logic's logic_add_edits).
template <class Record> Record fresh_logic(const EventChain &, size_t) {
	Record made{};
	made.param1 = made.param2 = made.param3 = made.param4 = kUnreadParam;
	return made;
}

// An event's triggers and its actions: the records its chain holds (a new one fresh_logic's), 20 at
// most each.
template <class Record>
ListOps chain_list(K kind, std::vector<Record> &(*list)(EventChain &)) {
	ListOps ops = vector_list<EventChain, Record>(k(kind), list, fresh_logic<Record>);
	const auto insert = ops.insert;
	ops.insert = [insert, list](const RecordHandle &owner, size_t index, const DetachedRecord *record, std::string &error) {
		if (list(owner.as<EventChain>()).size() >= kMaxEventRecords) {
			error = "An event holds " + std::to_string(kMaxEventRecords) + " of these at most.";
			return false;
		}
		return insert(owner, index, record, error);
	};
	return ops;
}
std::vector<bms::Trigger> &triggers_of(EventChain &chain) { return chain.triggers; }
std::vector<bms::Action> &actions_of(EventChain &chain) { return chain.actions; }

RecordTable make_table() {
	std::vector<TableKind> kinds;
	for (const KindRow &row : kKinds) {
		TableKind kind(RecordKindRow{k(row.kind), row.token, row.label, row.add_label, row.top});
		for (const MissionField &field : mission_fields(row.record))
			if (!derived(field.record, field.key)) kind.field(labelled(row, field));
		switch (row.kind) {
		case K::Mission:
			kind.list({spec(K::Loadout, "Weapon loadout"),
			           file_list<bms::WeaponLoadoutRecord>(K::Loadout, loadout_of, fresh_loadout), {}});
			kind.list({spec(K::Availability, "Item availability"),
			           file_list<bms::ItemAvailabilityEntry>(K::Availability, availability_of, fresh_availability), {}});
			kind.list({spec(K::Group, "Groups", bms::kGroupRecordCount, true),
			           fixed_list<bms::GroupRecord>(K::Group, groups_of, bms::kGroupRecordCount), {}});
			kind.list({spec(K::Layer, "Layers", bms::kLayerRecordCount, true),
			           fixed_list<bms::LayerRecord>(K::Layer, layers_of, bms::kLayerRecordCount), {}});
			kind.list({spec(K::BoundingBox, "Bounding boxes"),
			           file_list<bms::BoundingBox>(K::BoundingBox, boxes_of, fresh_box), {}});
			break;
		case K::WaypointPath:
			kind.list({spec(K::Stop, "Stops", kMaxWaypointPathMarkers), stop_list(), {}});
			break;
		case K::Event:
			kind.list({spec(K::Trigger, "Triggers", kMaxEventRecords), chain_list<bms::Trigger>(K::Trigger, triggers_of), {}});
			kind.list({spec(K::Action, "Actions", kMaxEventRecords), chain_list<bms::Action>(K::Action, actions_of), {}});
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

int mission_band(NodeKind kind) {
	int band = 0;
	for (const KindRow &row : kKinds) {
		if (!row.top) continue;
		if (k(row.kind) == kind) return band;
		++band;
	}
	return -1;
}

} // namespace opennova::editor
