#pragma once

// The mission's display names (ADR 0046 S15, Names): what each record of a mission, and each value its
// records hold that stands for something with a name, reads as to a modder. Pure functions of the
// document (made once per state of its rows: MissionDocument's lookups) and, for what the project's
// other files name, a NameSource (the asset graph: an item's name from its catalog, a text key's
// string from the mission's table): with none, the document's own words (MissionDocument::record_title
// is mission_record_label with none). Every value that names nothing says so in words.
//
// The meanings are the witnessed ones (docs/mission/bms-event-runtime-re.md 7, formats/mission/
// mission_params.h): an entity parameter is an SSN, the player's 10000 naming no record; a zone
// parameter a zone id; a group 0 none; a path 0 none and 123..127 the commands; an event and a stop's
// marker an index; the text keys the ones the game forms (mission_text_edges).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_sentence.h>
#include <editor/documents/name_source.h>
#include <formats/mission/bms.h>

namespace opennova::editor {

// --- records -----------------------------------------------------------------------------------------

// What a record of the mission reads as (DocumentType::record_label; the outline's rows, the viewport's
// labels, the Inspector's heading, Problems' places): the mission by its name; an entity by its item's
// name (its pool's without `names`) and its SSN, with the name the game shows for it where it has one
// ("Ranger #12 (Sgt. Miller)": its STRNAME, a navpoint's LOCATION); a path by its number and stops
// ("Path 5 (4 stops)"); a stop by the marker it visits; an area by its zone ("Zone 3", "(mission area)"
// after a boundary); an event as its sentence, a trigger as its words after the join that ties it to
// the one before, an action as its words (documents/mission_sentence.h, Events and scripts, over the
// names here: mission_event_title, mission_trigger_words, mission_action_words); a group by its index
// and members; a loadout entry and an availability rule by their weapon. "" for a record the document
// does not have.
std::string mission_record_label(const Document &document, const NodeAddress &address, const NameSource *names);
// A record's words for a column too narrow for its title (DocumentType::record_brief), what tells it
// apart first: an entity by its SSN then the name the game shows for it, else its item's ("#12 Sgt.
// Miller"); an event by its first trigger's subject and verb, an entity in it by its SSN ("#29 has passed
// waypoint 8 of path 9"), or with no trigger by what it does first ("show win objective 1"); a trigger and
// an action alike. "" for a record whose title is short already (a path, an area, a stop).
std::string mission_record_brief(const Document &document, const NodeAddress &address, const NameSource *names);

// An entity row's title ("Ranger #12", "Organic #12" without names), with its shown name where it has one.
std::string mission_entity_title(const MissionDocument &document, const Node &entity, const NameSource *names);
// A path row's title: "Path 5 (4 stops)", "Path 7 (no stops)"; path 0 "No path", 123..127 by the
// command's name.
std::string mission_path_title(const MissionPath &path);

// An event, a trigger and an action in words: the events lane's sentences (event_sentence,
// trigger_words, action_words) with what their parameters name worded here (an entity by its title with
// the project's names, a text key by its string in the mission's table where `names` are given; the
// document's own words otherwise, DocumentMissionNames).
std::string mission_event_title(const MissionDocument &document, const Node &event, const NameSource *names);
std::string mission_trigger_words(const MissionDocument &document, const bms::Trigger &trigger, const NameSource *names);
std::string mission_action_words(const MissionDocument &document, const bms::Action &action, const NameSource *names);
// The names those words take, for what else says a parameter's value in the same words (the Inspector's
// logic forms, what uses a record, a script's completions): the document's own with no `names`.
std::unique_ptr<MissionNames> mission_label_names(const MissionDocument &document, const NameSource *names);

// The headings the mission's outline groups its rows under (ui/outline_model's OutlineSpec::groups),
// one list per row in the document's order: the mission row under none; an entity under its pool
// ("Organics"), then its team where the pool holds more than one ("Team 2"), then its group where its
// team holds more than one in the pool and the group more than one row ("Group 3", "No group"; a row
// alone in its group stands under its team, before the group headings); a path, an area and an
// event under their kind ("Waypoint paths", "Area triggers", "Events"). The pools and the kinds in
// the file's bands' order, the teams and the groups by number.
void mission_row_headings(const Document &document, std::vector<std::vector<RowHeading>> &out);

// --- values ------------------------------------------------------------------------------------------

// A field's value worded where it names something (DocumentType::value_label): an entity's item, group,
// path and start stop (or the SSN it boards, beside a command 123..125), its name index's string; a
// stop's marker; a trigger's and an action's parameters by what their type reads them as (mission_params:
// an SSN, a zone id, an event, a group, a path, a stop, a sub-goal's text, a distance in metres, a time in
// seconds, a speed); the text an action shows and the objectives panel's rows. False where the value
// names nothing (a plain number), its words then the generic ones (graph/display_names.h).
bool mission_value_label(const Document &document, const NodeAddress &address, const FieldUse &field, const Value &value,
                         const NameSource *names, DisplayName &out);

// The words of what each kind of mission value names, for the pickers and the peers' views alike:
// an item by its catalog's name ("No item 123456 in the project" where none defines it);
DisplayName mission_item_display(int64_t item, const NameSource *names);
// an entity by its SSN ("Ranger #12"; the player's 10000 "The player"; "No entity has SSN 55");
DisplayName mission_ssn_display(const MissionDocument &document, int64_t ssn, const NameSource *names);
// an area by its zone id ("Zone 3"; "No area has zone 7");
DisplayName mission_zone_display(const MissionDocument &document, int64_t zone);
// an event by its index ("Event 3: <its sentence>"; "No event 9 (the mission has 4 events)");
DisplayName mission_event_display(const MissionDocument &document, int64_t index, const NameSource *names);
// a group by its index ("Group 5 (8 entities)"; 0 "No group"; past the 64 "No group 70 (the mission has 64)");
DisplayName mission_group_display(const MissionDocument &document, int64_t group);
// a path by its number ("Path 5 (4 stops)"; 0 "No path"; a command by its name);
DisplayName mission_path_display(const MissionDocument &document, int64_t number);
// a marker by its index among the markers ("Navpoint #2072 (Airfield)"; "No marker 40 (the mission has 30)").
DisplayName mission_marker_display(const MissionDocument &document, int64_t index, const NameSource *names);

} // namespace opennova::editor
