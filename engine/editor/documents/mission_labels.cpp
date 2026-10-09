// The mission's display names (mission_labels.h, ADR 0046 S15 Names).
#include <editor/documents/mission_labels.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <base/io/strutil.h>
#include <editor/documents/mission_sentence.h>
#include <editor/project/project_files.h>
#include <formats/def/reserved_items.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/mission/mission_params.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/world/trigger_relations.h>

namespace opennova::editor {

using namespace mission;

namespace {

using K = MissionKind;
constexpr NodeKind k(K kind) { return node_kind(kind); }

// The groups the game's tables hold (bms::kGroupRecordCount) [bms-event-runtime-re.md 7.2].
constexpr int64_t kGroupCount = bms::kGroupRecordCount;
// A waypoint number from 1 to 122 names a path; 0 none, 123..127 a command (kFirstPathCommand on).
constexpr int64_t kLastPathNumber = kFirstPathCommand - 1;

// The waypoint list's commands by what the game does with them (mission_sentence's path_command_words;
// the original editor's names in the tooltip, path_command_editor_name).
const char *path_command_name(int64_t number) { return path_command_words(number); }

// A pool's word for an entity of it ("Organic"), the table's label.
const char *pool_word(NodeKind kind) {
	const TableKind *row = mission_table().kind(kind);
	return row ? row->row().label : "Entity";
}

std::string counted_words(size_t count, const char *one, const char *many) {
	return std::to_string(count) + " " + (count == 1 ? one : many);
}

// The text a key's edge reaches, and where: false where none of its tables defines it.
bool text_of(const GraphEdge &edge, const NameSource *names, DisplayName &out) {
	const GraphSymbol *symbol = names ? names->reached(edge) : nullptr;
	if (!symbol) return false;
	out.text = symbol_words(*symbol);
	out.source = symbol->scope.empty() ? symbol->file : symbol->scope;
	return true;
}

// The text key a record's field forms (mission_text_edges), "" field the record's own; were it `value`
// where one is given (a picker's choice worded as the field would read set to it).
bool text_edge(const MissionDocument &document, const NodeAddress &address, const std::string &field, GraphEdge &out,
               const int64_t *value = nullptr) {
	std::vector<GraphEdge> edges;
	if (value && !field.empty()) mission_text_edges(document, address, edges, false, field.c_str(), *value);
	else mission_text_edges(document, address, edges);
	for (GraphEdge &edge : edges)
		if (edge.field == field) {
			out = std::move(edge);
			return true;
		}
	return false;
}

// The name the game shows for an entity: its STRNAME (a nonzero name index) or a navpoint's LOCATION,
// from the mission's table; "" where it has none or its table lacks the key.
std::string shown_name(const MissionDocument &document, const Node &entity, const NameSource *names) {
	if (!names) return std::string();
	std::vector<GraphEdge> edges;
	mission_text_edges(document, {entity.id, entity.kind, 0}, edges);
	for (const GraphEdge &edge : edges) {
		DisplayName text;
		if (text_of(edge, names, text) && !text.text.empty()) return text.text;
	}
	return std::string();
}

const bms::Header *header_of(const MissionDocument &document) {
	const MissionRow *mission = document.mission_row();
	return mission ? &mission->native.header : nullptr;
}

// The words the events lane's sentences (documents/mission_sentence.h) take what a parameter names
// in: the document's own (DocumentMissionNames: an entity by its record's title, a zone by its area's,
// an event by its place), an entity with the project's names by its item's name and the name the game
// shows for it, and a text key by its string in the mission's table (the game's scoped order: the
// mission's own table, else medmssn.bin), where the names are given.
class LabelNames : public DocumentMissionNames {
public:
	LabelNames(const MissionDocument &document, const NameSource *names)
	    : DocumentMissionNames(document), document_(document), names_(names) {}
	std::string entity(int64_t ssn) const override {
		if (!names_ || ssn == kPlayerSsn) return DocumentMissionNames::entity(ssn);
		const Node *row = document_.row(document_.entity_holder(ssn));
		return row ? mission_entity_title(document_, *row, names_) : DocumentMissionNames::entity(ssn);
	}
	std::string text(const std::string &section, const std::string &key) const override {
		if (!names_) return std::string();
		GraphEdge edge;
		edge.source = document_.path();
		edge.kind = ReferenceKind::TextId;
		edge.value = key;
		// The mission's own table, else medmssn.bin: the by-name table's text row.
		const Sidecar &table = *sidecar_for_role("text");
		edge.scope = strutil::to_upper(sidecar_name(basename_of(document_.path()), table)) + "/" + section;
		edge.scope_alternate = strutil::to_upper(table.fallback);
		DisplayName found;
		return text_of(edge, names_, found) ? found.text : std::string();
	}

private:
	const MissionDocument &document_;
	const NameSource *names_;
};

// A parameter's value by its slot.
int32_t param_of(const bms::Trigger &trigger, int slot) {
	return slot == 0 ? trigger.param1 : slot == 1 ? trigger.param2 : slot == 2 ? trigger.param3 : trigger.param4;
}
int32_t param_of(const bms::Action &action, int slot) {
	return slot == 0 ? action.param1 : slot == 1 ? action.param2 : slot == 2 ? action.param3 : action.param4;
}

// What a stop number is read for: an entity's start stop, a waypoint trigger's visited stop (a bit of the
// visited word), a Redirect's node.
enum class StopUse { Start, Visited, Redirect };

// The most stop numbers a waypoint trigger's visited word records: the bit is the stop's number, below
// 32 (world::TriggerRelations::kWaypointBits) [bms-event-runtime-re.md 3a, 7.4].
constexpr int64_t kVisitedStops = world::TriggerRelations::kWaypointBits;

// A stop of a path by its number, the game's own (0 the first), and the marker it visits: a Redirect's
// -1 the nearest [orig: Entity_SetWaypointByTeam @0x43CD20, only node -1 requests the nearest]; a
// waypoint trigger's past 31 (or below 0) a stop the game never records.
DisplayName stop_display(const MissionDocument &document, int64_t path, int64_t index, StopUse use,
                         const NameSource *names) {
	DisplayName out;
	out.raw = std::to_string(index);
	if (use == StopUse::Redirect && index == -1) {
		out.text = "The nearest stop";
		return out;
	}
	if (use == StopUse::Visited && (index < 0 || index >= kVisitedStops)) {
		out.text = "Stop " + out.raw + ": never recorded (the game keeps stops 0 to 31 of a path)";
		out.dangling = true;
		return out;
	}
	const Node *row = path >= 1 && path <= kLastPathNumber ? document.row_of(K::WaypointPath, size_t(path)) : nullptr;
	if (!row) {
		out.text = "Stop " + out.raw;
		return out;
	}
	const std::vector<uint32_t> &stops = static_cast<const PathRow &>(*row).native.record.waypoint_numbers;
	if (index < 0 || size_t(index) >= stops.size()) {
		// The walk reads the slot word there as written [orig: AIWaypoint_UpdateTarget @0x457476].
		out.text = "Stop " + out.raw + ": past the " + counted_words(stops.size(), "stop", "stops") + " of path " +
		           std::to_string(path);
		out.dangling = true;
		return out;
	}
	out.text = "Stop " + out.raw + " of path " + std::to_string(path) + ": " +
	           mission_marker_display(document, stops[size_t(index)], names).text;
	return out;
}

// A parameter of the kind as the Inspector words it beside its number, the record's other parameters
// read where they decide it (a stop's path); false for a kind that names nothing (a count, a distance:
// the number is its value).
bool param_display(const MissionDocument &document, ParamKind kind, int64_t value, int64_t path, StopUse use,
                   const NameSource *names, DisplayName &out) {
	switch (kind) {
	case ParamKind::Entity: out = mission_ssn_display(document, value, names); return true;
	case ParamKind::Zone: out = mission_zone_display(document, value); return true;
	case ParamKind::Event: out = mission_event_display(document, value, names); return true;
	case ParamKind::Group: out = mission_group_display(document, value); return true;
	case ParamKind::Path: out = mission_path_display(document, value); return true;
	case ParamKind::PathNode: out = stop_display(document, path, value, use, names); return true;
	case ParamKind::MissionVar: out.text = "Variable " + std::to_string(value); break;
	case ParamKind::Dialog: out.text = "Dialog " + std::to_string(value); break;
	default: return false;
	}
	out.raw = std::to_string(value);
	return true;
}

// The path a record's parameters name beside a stop (a GroupAtWaypoint's, a Redirect's): the first
// Path parameter.
template <class Record, class KindOf> int64_t path_param(const Record &record, KindOf kind_of) {
	for (int slot = 0; slot < 4; ++slot)
		if (kind_of(record, slot) == ParamKind::Path) return param_of(record, slot);
	return 0;
}

} // namespace

// --- values ------------------------------------------------------------------------------------------

DisplayName mission_item_display(int64_t item, const NameSource *names) {
	DisplayName out;
	out.raw = std::to_string(item);
	if (!names) return out;
	if (const GraphSymbol *symbol = names->symbol(ReferenceKind::Item, out.raw)) {
		out.text = symbol_words(*symbol);
		out.source = symbol->file;
		return out;
	}
	out.text = "No item " + out.raw + " in the project";
	out.dangling = true;
	return out;
}

DisplayName mission_ssn_display(const MissionDocument &document, int64_t ssn, const NameSource *names) {
	DisplayName out;
	out.raw = std::to_string(ssn);
	if (ssn == kPlayerSsn) {
		out.text = "The player";
		return out;
	}
	if (ssn == 0) {
		out.text = "No entity";
		return out;
	}
	if (const Node *entity = document.row(document.entity_holder(ssn))) {
		out.text = mission_entity_title(document, *entity, names);
		return out;
	}
	out.text = "No entity has SSN " + out.raw;
	out.dangling = true;
	return out;
}

DisplayName mission_zone_display(const MissionDocument &document, int64_t zone) {
	DisplayName out;
	out.raw = std::to_string(zone);
	if (const Node *area = document.row(document.zone_holder(zone))) {
		out.text = mission_record_label(document, {area->id, area->kind, 0}, nullptr);
		return out;
	}
	out.text = "No area has zone " + out.raw;
	out.dangling = true;
	return out;
}

DisplayName mission_event_display(const MissionDocument &document, int64_t index, const NameSource *names) {
	DisplayName out;
	out.raw = std::to_string(index);
	const Node *event = index >= 0 ? document.row_of(K::Event, size_t(index)) : nullptr;
	if (event) {
		out.text = "Event " + std::to_string(index + 1) + ": " + mission_event_title(document, *event, names);
		return out;
	}
	out.text = "No event " + std::to_string(index + 1) + " (the mission has " +
	           counted_words(document.count_of(K::Event), "event", "events") + ")";
	out.dangling = true;
	return out;
}

DisplayName mission_group_display(const MissionDocument &document, int64_t group) {
	DisplayName out;
	out.raw = std::to_string(group);
	if (group == 0) {
		out.text = "No group";
		return out;
	}
	if (group < 0 || group >= kGroupCount) {
		out.text = "No group " + out.raw + " (the mission has 64)";
		out.dangling = true;
		return out;
	}
	out.text = "Group " + out.raw + " (" + counted_words(document.group_members(group), "entity", "entities") + ")";
	return out;
}

DisplayName mission_path_display(const MissionDocument &document, int64_t number) {
	DisplayName out;
	out.raw = std::to_string(number);
	if (number == 0) {
		out.text = "No path";
		return out;
	}
	if (const char *command = path_command_name(number)) {
		out.text = command;
		// The original editor's name for it, said beside (the Inspector's tooltip: "From ...").
		out.source = std::string("the original editor's name: ") + mission::path_command_editor_name(number);
		return out;
	}
	const Node *row = number > 0 && number <= kLastPathNumber ? document.row_of(K::WaypointPath, size_t(number)) : nullptr;
	if (!row) {
		out.text = "No path " + out.raw + " (paths are 1 to 122)";
		out.dangling = true;
		return out;
	}
	out.text = mission_path_title(static_cast<const PathRow &>(*row).native);
	return out;
}

DisplayName mission_marker_display(const MissionDocument &document, int64_t index, const NameSource *names) {
	DisplayName out;
	out.raw = std::to_string(index);
	const Node *marker = index >= 0 ? document.row_of(K::Marker, size_t(index)) : nullptr;
	if (marker) {
		out.text = mission_entity_title(document, *marker, names);
		return out;
	}
	out.text = "No marker " + out.raw + " (the mission has " + counted_words(document.count_of(K::Marker), "marker", "markers") + ")";
	out.dangling = true;
	return out;
}

bool mission_value_label(const Document &base, const NodeAddress &address, const FieldUse &field, const Value &value,
                         const NameSource *names, DisplayName &out) {
	const auto *document = dynamic_cast<const MissionDocument *>(&base);
	const int64_t *number = std::get_if<int64_t>(&value);
	if (!document || !field.schema || !number) return false;
	const std::string &id = field.schema->id;
	const Node *row = document->row(address.row);
	if (!row) return false;
	// What a field's number forms as a text key (a name index, an objectives row, the line an action
	// shows): the string its table holds.
	const auto text = [&](const std::string &prefix) {
		GraphEdge edge;
		if (!text_edge(*document, address, id, edge, number)) return false;
		out = DisplayName();
		out.raw = std::to_string(*number);
		DisplayName found;
		if (text_of(edge, names, found)) {
			out.text = prefix + "\"" + found.text + "\"";
			out.source = found.source;
		} else if (names) {
			out.text = prefix + "No text " + edge.value + " in the mission's table";
			out.dangling = true;
		} else {
			out.text = prefix + edge.value;
		}
		return true;
	};
	if (is_entity_kind(address.kind) && !address.child) {
		const bms::Entity &entity = static_cast<const EntityRow &>(*row).native;
		if (id == "item") out = mission_item_display(*number, names);
		else if (id == "group") out = mission_group_display(*document, *number);
		else if (id == "waypoint_id") out = mission_path_display(*document, *number);
		else if (id == "wp_number") {
			// Beside a command 123..125 the SSN to board; beside a path the stop it starts at.
			if (path_command_names_entity(entity.waypoint_id)) out = mission_ssn_display(*document, *number, names);
			else if (entity.waypoint_id >= 1 && entity.waypoint_id <= kLastPathNumber)
				out = stop_display(*document, entity.waypoint_id, *number, StopUse::Start, names);
			else return false;
		} else if (id == "wp_adv_trigger") {
			// A waypoint's advance trigger: the event it completes on, by its index (none at 0 or below)
			// [orig: Entity_SpawnFromBMSRecord @0x40f0b3; EventTrigger_MarkLinkedSpawnPoints @0x452ce0].
			if (entity.type_id != def::DEF_TYPE_WAYPOINT) return false;
			if (*number <= 0) {
				out = DisplayName();
				out.raw = std::to_string(*number);
				out.text = "No event (the waypoint advances when the player reaches it)";
				return true;
			}
			out = mission_event_display(*document, *number, names);
		} else if (id == "name_index") {
			return *number != 0 && text("");
		} else if (id == "ttool_index") {
			// A type-6005 waypoint's name: its STRWPNAME string as the spawn keeps it, the first 15
			// characters (hud::waypoint_name_as_spawned) [orig: Entity_SpawnFromBMSRecord
			// @0x40f102..0x40f107, the string cut in place].
			if (!text("")) return false;
			if (!out.dangling && out.text.size() > 2) {
				const std::string whole = out.text.substr(1, out.text.size() - 2);
				const std::string kept = hud::waypoint_name_as_spawned(whole);
				if (kept.size() < whole.size())
					out.text = "\"" + kept + "\" (the game keeps " + std::to_string(hud::kWaypointNameChars) +
					           " characters of \"" + whole + "\")";
			}
		} else {
			return false;
		}
		return true;
	}
	if (address.kind == k(K::Stop)) {
		if (id != "marker") return false;
		out = mission_marker_display(*document, *number, names);
		return true;
	}
	if (address.kind == k(K::Mission)) {
		const bool win = id.compare(0, 15, "win_conditions[") == 0, lose = id.compare(0, 16, "lose_conditions[") == 0;
		if (!win && !lose) return false;
		if (*number == 0 || *number == 255) return false; // an empty slot
		// A win slot up to the first empty one is an objectives panel row, its STRWINCOND [orig:
		// HUD_DrawWinConditions @0x5ba940, the break @0x5ba9e0].
		if (win && text("")) return true;
		// A win slot past it, and a lose slot, still key what the sub-goal actions show of the slot: its
		// message (STRWINMSG / STRLOSEMSG) [orig: EventAction_Dispatch case 14 @0x454552, case 15 @0x45460c].
		char key[32];
		std::snprintf(key, sizeof(key), "%s%03i", win ? "STRWINMSG" : "STRLOSEMSG", int(*number));
		const std::string message = LabelNames(*document, names).text(win ? "WinConditions" : "LoseConditions", key);
		out = DisplayName();
		out.raw = std::to_string(*number);
		out.text = std::string(win ? "Not on the objectives panel (it stops at the first empty slot)" : "A lose objective") +
		           (message.empty() ? std::string() : ": its message \"" + message + "\"");
		return true;
	}
	const int slot = id.size() == 6 && id.compare(0, 5, "param") == 0 && id[5] >= '1' && id[5] <= '4' ? id[5] - '1' : -1;
	if (slot < 0 || !address.child) return false;
	const Document::RecordPath path = document->path_in(*row, address.child);
	if (path.size() != 1) return false;
	const EventRow &event = static_cast<const EventRow &>(*row);
	if (address.kind == k(K::Trigger) && path[0].index < event.native.triggers.size()) {
		const bms::Trigger &trigger = event.native.triggers[path[0].index];
		if (!param_display(*document, trigger_param_kind(trigger, slot), *number, path_param(trigger, trigger_param_kind),
		                   StopUse::Visited, names, out))
			return false;
		// An SSN the sees, targeted, shot and visited records never keep (128 or more, section 3a).
		if (mission::trigger_ssn_unrecorded(trigger, slot)) {
			out.text += " (never recorded: the game keeps SSNs below 128)";
			out.dangling = true;
		}
		return true;
	}
	if (address.kind == k(K::Action) && path[0].index < event.native.actions.size()) {
		const bms::Action &action = event.native.actions[path[0].index];
		const ParamKind kind = action_param_kind(action, slot);
		// A sub-goal by its slot and the line the action shows of it; the text an OutputText shows.
		if (kind == ParamKind::SubGoal) {
			const std::string goal = "Sub-goal " + std::to_string(*number);
			if (text(goal + ": ")) return true;
			out = DisplayName();
			out.raw = std::to_string(*number);
			out.text = goal;
			return true;
		}
		if (slot == 0 && action.action_type == bms::ActionType::OutputText) return text("");
		return param_display(*document, kind, *number, path_param(action, action_param_kind), StopUse::Redirect, names, out);
	}
	return false;
}

// --- records ------------------------------------------------------------------------------------------

void mission_row_headings(const Document &document, const NameSource *names, std::vector<std::vector<RowHeading>> &out) {
	const auto &rows = document.rows();
	out.assign(rows.size(), std::vector<RowHeading>());
	// A marker's type is its item (a waypoint, a spawn point, the map's centre): the markers stand under
	// their types first. Each pool's types, each type's teams, each team's groups and how many rows each
	// holds, a heading shown only where it tells rows apart and holds more than one row (a vehicle in a
	// group of its own stands under its team, not under a heading over itself alone).
	const auto type_of = [](const Node &row, const bms::Entity &entity) {
		return row.kind == k(K::Marker) ? int64_t(entity_item_id(entity)) : int64_t(-1);
	};
	using Type = std::pair<NodeKind, int64_t>;
	std::map<NodeKind, std::set<int64_t>> types;
	std::map<Type, size_t> typed;
	std::map<Type, std::set<int>> teams;
	std::map<std::tuple<NodeKind, int64_t, int>, std::set<int>> groups;
	std::map<std::tuple<NodeKind, int64_t, int, int>, size_t> members;
	for (const auto &row : rows) {
		if (!row || !is_entity_kind(row->kind)) continue;
		const bms::Entity &entity = static_cast<const EntityRow &>(*row).native;
		const int64_t type = type_of(*row, entity);
		types[row->kind].insert(type);
		++typed[{row->kind, type}];
		teams[{row->kind, type}].insert(entity.team);
		groups[{row->kind, type, entity.team}].insert(entity.group_id);
		++members[{row->kind, type, entity.team, entity.group_id}];
	}
	const auto number = [](char prefix, int value) {
		char key[16];
		std::snprintf(key, sizeof(key), "%c%03d", prefix, value);
		return std::string(key);
	};
	// A type's words: its item's catalog name, else its id; keyed by them (without case, no "/",
	// which joins the keys of the headings over a row), so the types read in order.
	const auto key_words = [](std::string text) {
		text = strutil::to_lower(text);
		std::replace(text.begin(), text.end(), '/', '|');
		return text;
	};
	std::map<int64_t, RowHeading> type_headings;
	const auto type_heading = [&](int64_t item) -> const RowHeading & {
		auto found = type_headings.find(item);
		if (found != type_headings.end()) return found->second;
		const DisplayName words = mission_item_display(item, names);
		const std::string text = words.text.empty() || words.dangling ? "Item " + std::to_string(item) : words.text;
		return type_headings.emplace(item, RowHeading{"i" + key_words(text) + "\x1f" + std::to_string(item), text})
		        .first->second;
	};
	for (size_t i = 0; i < rows.size(); ++i) {
		const auto &row = rows[i];
		if (!row || row->kind == k(K::Mission)) continue;
		const int band = mission_band(row->kind);
		if (band < 0) continue;
		const TableKind *kind = mission_table().kind(row->kind);
		char key[8];
		std::snprintf(key, sizeof(key), "%02d", band);
		out[i].push_back({key, std::string(kind ? kind->row().label : "Record") + "s"});
		if (!is_entity_kind(row->kind)) continue;
		const bms::Entity &entity = static_cast<const EntityRow &>(*row).native;
		const int64_t type = type_of(*row, entity);
		if (types[row->kind].size() > 1 && typed[{row->kind, type}] > 1) out[i].push_back(type_heading(type));
		if (teams[{row->kind, type}].size() > 1) out[i].push_back({number('t', entity.team), "Team " + std::to_string(entity.team)});
		if (groups[{row->kind, type, entity.team}].size() > 1 && members[{row->kind, type, entity.team, entity.group_id}] > 1)
			out[i].push_back({number('g', entity.group_id),
			                  entity.group_id ? "Group " + std::to_string(entity.group_id) : std::string("No group")});
	}
}

bool mission_row_reads_others(const Document &, const Node &row) {
	return row.kind == k(K::Event) || row.kind == k(K::WaypointPath);
}

std::string mission_path_title(const MissionPath &path) {
	if (path.number == 0) return "No path";
	if (const char *command = path_command_name(path.number)) return command;
	const size_t stops = path.record.waypoint_numbers.size();
	return "Path " + std::to_string(path.number) + " (" + (stops ? counted_words(stops, "stop", "stops") : "no stops") + ")";
}

std::string mission_entity_title(const MissionDocument &document, const Node &row, const NameSource *names) {
	const bms::Entity &entity = static_cast<const EntityRow &>(row).native;
	std::string what = pool_word(row.kind);
	if (names) {
		const DisplayName item = mission_item_display(entity_item_id(entity), names);
		if (!item.dangling && !item.text.empty()) what = item.text;
	}
	std::string title = what + " #" + std::to_string(entity.id);
	const std::string shown = shown_name(document, row, names);
	if (!shown.empty()) title += " (" + shown + ")";
	return title;
}

std::string mission_trigger_words(const MissionDocument &document, const bms::Trigger &trigger, const NameSource *names) {
	return trigger_words(trigger, LabelNames(document, names));
}

std::string mission_action_words(const MissionDocument &document, const bms::Action &action, const NameSource *names) {
	return action_words(action, LabelNames(document, names), header_of(document));
}

std::string mission_event_title(const MissionDocument &document, const Node &row, const NameSource *names) {
	return event_sentence(static_cast<const EventRow &>(row).native, LabelNames(document, names), header_of(document));
}

std::unique_ptr<MissionNames> mission_label_names(const MissionDocument &document, const NameSource *names) {
	return std::make_unique<LabelNames>(document, names);
}

std::string mission_record_label(const Document &base, const NodeAddress &address, const NameSource *names) {
	const auto *document = dynamic_cast<const MissionDocument *>(&base);
	const Node *row = document ? document->row(address.row) : nullptr;
	if (!row) return std::string();
	if (!address.child) {
		switch (static_cast<K>(row->kind)) {
		case K::Item:
		case K::Building:
		case K::Marker:
		case K::Organic: return mission_entity_title(*document, *row, names);
		case K::WaypointPath: return mission_path_title(static_cast<const PathRow &>(*row).native);
		case K::Area: {
			const bms::AreaTrigger &area = static_cast<const AreaRow &>(*row).native;
			return "Zone " + std::to_string(area.id) + (area.is_active() ? " (mission area)" : "");
		}
		// An event by its number, what the triggers and the actions that name it say ("event 7"), then its
		// sentence (S15).
		case K::Event: return "Event " + std::to_string(document->index_of(*row) + 1) + ": " + mission_event_title(*document, *row, names);
		default: return document->record_name(address);
		}
	}
	const Document::RecordPath path = document->path_in(*row, address.child);
	if (path.empty()) return document->record_name(address);
	const size_t index = path[path.size() - 1].index;
	switch (static_cast<K>(address.kind)) {
	case K::Trigger: {
		// Its words after the join that ties it to the one before ("or Hostage #10034 is destroyed").
		const EventRow &event = static_cast<const EventRow &>(*row);
		if (index >= event.native.triggers.size()) break;
		const std::string words = mission_trigger_words(*document, event.native.triggers[index], names);
		return index == 0 ? words : std::string(logic_join_words(trigger_join(event.native.triggers[index - 1]))) + " " + words;
	}
	case K::Action: {
		const EventRow &event = static_cast<const EventRow &>(*row);
		if (index < event.native.actions.size()) return mission_action_words(*document, event.native.actions[index], names);
		break;
	}
	case K::Stop: {
		const std::vector<uint32_t> &stops = static_cast<const PathRow &>(*row).native.record.waypoint_numbers;
		// By the game's own number for it (0 the first: what the waypoint triggers and an entity's start
		// stop name).
		if (index < stops.size())
			return "Stop " + std::to_string(index) + ": " + mission_marker_display(*document, stops[index], names).text;
		break;
	}
	case K::Group: {
		const DisplayName group = mission_group_display(*document, int64_t(index));
		return index == 0 ? "Group 0 (no group)" : group.text;
	}
	case K::Loadout: {
		const auto &entries = static_cast<const MissionRow &>(*row).native.loadout.entries;
		if (index < entries.size() && !entries[index].name.empty()) return entries[index].name;
		break;
	}
	case K::Availability: {
		const auto &entries = static_cast<const MissionRow &>(*row).native.item_availability;
		if (index < entries.size() && !entries[index].name.empty()) return entries[index].name;
		break;
	}
	default: break;
	}
	return document->record_name(address);
}

namespace {

// The words a brief takes what a parameter names in: an entity by its SSN alone ("#29"; the player as
// the player), everything else as the base says a value by itself ("zone 3", "group 4").
class BriefNames : public MissionNames {
public:
	std::string entity(int64_t ssn) const override {
		return ssn == kPlayerSsn ? MissionNames::entity(ssn) : "#" + std::to_string(ssn);
	}
};

} // namespace

void mission_game_choices(const Document &, const NodeAddress &, const FieldUse &field, std::vector<GameChoice> &out) {
	if (field.reference == ReferenceKind::MissionEntity) out.push_back({std::to_string(kPlayerSsn), "The player"});
	// A number forming a text key, 0 forming none: an entity with no name (the spawn looks no STRNAME
	// up for it [orig: Entity_SpawnFromBMSRecord @0x40ecbf..0x40ed0a]), the end of the objectives panel's
	// rows [orig: HUD_DrawWinConditions @0x5ba940, the break @0x5ba9e0].
	// (A waypoint's name id forms STRWPNAME000 from 0 like any other number [orig: @0x40f0c6].)
	if (field.reference == ReferenceKind::TextId && field.key_prefix && field.schema) {
		if (field.schema->id == "name_index") out.push_back({"0", "No name"});
		else if (field.schema->id.compare(0, 15, "win_conditions[") == 0)
			out.push_back({"0", "No objective (the panel's rows end here)"});
	}
}

std::string mission_record_brief(const Document &base, const NodeAddress &address, const NameSource *names) {
	const auto *document = dynamic_cast<const MissionDocument *>(&base);
	const Node *row = document ? document->row(address.row) : nullptr;
	if (!row) return std::string();
	const BriefNames brief;
	if (!address.child) {
		switch (static_cast<K>(row->kind)) {
		case K::Item:
		case K::Building:
		case K::Marker:
		case K::Organic: {
			const bms::Entity &entity = static_cast<const EntityRow &>(*row).native;
			std::string what = shown_name(*document, *row, names);
			if (what.empty() && names) {
				const DisplayName item = mission_item_display(entity_item_id(entity), names);
				if (!item.dangling) what = item.text;
			}
			if (what.empty()) what = pool_word(row->kind);
			return "#" + std::to_string(entity.id) + " " + what;
		}
		case K::Event: {
			// Its number, then what waits first, else what it does first.
			const mission::EventChain &chain = static_cast<const EventRow &>(*row).native;
			const std::string number = "Event " + std::to_string(document->index_of(*row) + 1) + ": ";
			if (!chain.triggers.empty()) return number + trigger_words(chain.triggers.front(), brief);
			if (!chain.actions.empty()) return number + action_words(chain.actions.front(), brief, header_of(*document));
			return number + "nothing";
		}
		default: return std::string();
		}
	}
	const Document::RecordPath path = document->path_in(*row, address.child);
	if (path.empty() || static_cast<K>(row->kind) != K::Event) return std::string();
	const size_t index = path[path.size() - 1].index;
	const EventRow &event = static_cast<const EventRow &>(*row);
	if (static_cast<K>(address.kind) == K::Trigger && index < event.native.triggers.size())
		return trigger_words(event.native.triggers[index], brief);
	if (static_cast<K>(address.kind) == K::Action && index < event.native.actions.size())
		return action_words(event.native.actions[index], brief, header_of(*document));
	return std::string();
}

} // namespace opennova::editor
