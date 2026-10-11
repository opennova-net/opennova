#include "mission_board.h"

#include <algorithm>
#include <cctype>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/documents/mission_table.h>
#include <formats/mission/bms.h>

namespace opennova::editor {

using mission::ParamKind;

namespace {

constexpr NodeKind k(MissionKind kind) { return node_kind(kind); }

std::string lowered(const std::string &text) {
	std::string out = text;
	for (char &c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

// A pick its type reads left at none: 0 or -1 for an entity, an area, a group or a path; -1 for an event (0
// is the first event).
bool unset_pick(ParamKind kind, int64_t value) {
	switch (kind) {
	case ParamKind::Entity:
	case ParamKind::Zone:
	case ParamKind::Group:
	case ParamKind::Path: return value == 0 || value == -1;
	case ParamKind::Event: return value < 0;
	default: return false;
	}
}

// A pick naming nothing the mission holds: an SSN no entity holds (the player's names none and is the game's),
// a zone no area has, an event past the last, a group past the 64, a path the mission lacks.
bool missing_pick(const MissionDocument &document, ParamKind kind, int64_t value) {
	switch (kind) {
	case ParamKind::Entity: return value != mission::kPlayerSsn && !document.entity_holder(value);
	case ParamKind::Zone: return !document.zone_holder(value);
	case ParamKind::Event: return size_t(value) >= document.count_of(MissionKind::Event);
	case ParamKind::Group: return value < 0 || value >= int64_t(bms::kGroupRecordCount);
	case ParamKind::Path: return mission_path_display(document, value).dangling;
	default: return false;
	}
}

std::string slot_words(const BoardChip &chip) { return chip.label.empty() ? std::string(chip.field) : chip.label; }

BoardLine line_of(const MissionDocument &document, const EventRow &event, const NodeAddress &record, const MissionNames &names,
                  const bms::Header *header, std::vector<BoardNote> &notes) {
	BoardLine line;
	LogicForm form;
	if (!logic_form(document, record, names, form, false)) return line;
	line.record = record;
	line.action = form.action;
	line.index = form.index;
	line.type = form.type;
	line.type_words = form.type_words;
	line.negated = form.negated;
	line.join = form.join;
	line.last = form.last;
	line.runs = form.action ? action_runs(event.native.actions[form.index], names, header)
	                        : trigger_runs(event.native.triggers[form.index], names);
	for (const LogicParam &param : form.params) {
		BoardChip chip;
		chip.slot = param.slot;
		chip.field = param.field;
		chip.label = param.label;
		chip.words = param.words;
		chip.value = param.value;
		chip.kind = param.kind;
		chip.unit = param.unit;
		chip.picks = param.picks;
		chip.unset = param.picks && unset_pick(param.kind, param.value);
		chip.missing = param.picks && !chip.unset && missing_pick(document, param.kind, param.value);
		const std::string what = (form.action ? "Action " : "Trigger ") + std::to_string(form.index + 1);
		if (chip.unset) notes.push_back({record, kBoardUnset, what + ": its " + lowered(slot_words(chip)) + " is not set."});
		else if (chip.missing) notes.push_back({record, kBoardMissing, what + ": " + param.words + "."});
		// An area that does not resolve: the game drops the trigger (it reads false) and the action at the
		// mission's start [docs/mission/bms-event-runtime-re.md 7.3].
		if (param.kind == ParamKind::Zone && !chip.unset && !chip.missing && !names.zone_resolves(param.value))
			notes.push_back({record, kBoardZoneDropped,
			                 what + ": its area's box is flat, so the game drops it as the mission starts."});
		line.chips.push_back(std::move(chip));
	}
	for (const LogicParam &unread : form.unread)
		notes.push_back({record, kBoardUnread,
		                 (form.action ? "Action " : "Trigger ") + std::to_string(form.index + 1) + ": " + unread.field + " holds " +
		                         std::to_string(unread.value) + ", which its type does not read (kept as it is)."});
	return line;
}

void wait_note(std::vector<BoardNote> &notes, const NodeAddress &event, const char *what, int64_t steps) {
	if (!bms::event_steps_wrap(steps)) return;
	notes.push_back({event, kBoardWaitWraps,
	                 std::string("Its ") + what + " of " + std::to_string(steps) + " steps is past 512: the game's countdown wraps, so "
	                 "it comes to " + logic_steps_words(steps) + "."});
}

} // namespace

bool mission_board_card(const MissionDocument &document, NodeId event, const MissionNames &names, BoardCard &out,
                        const LiveLogic *live) {
	LogicEventForm form;
	if (!logic_event_form(document, event, names, form)) return false;
	const Node *row = document.row(event);
	const EventRow &native = static_cast<const EventRow &>(*row);
	const MissionRow *mission = document.mission_row();
	const bms::Header *header = mission ? &mission->native.header : nullptr;
	out = BoardCard();
	out.event = form.event;
	out.index = form.index;
	// Its number, and the name the mission's names give it where they give one.
	const std::string base = MissionNames().event(int64_t(form.index)), given = names.event(int64_t(form.index));
	out.title = "Event " + std::to_string(form.index + 1) + (given != base ? ": " + given : std::string());
	out.words = form.words;
	out.repeats = form.repeats;
	out.at_start = form.at_start;
	out.at_end = form.at_end;
	out.delay_steps = form.delay;
	out.repeat_steps = form.repeat;
	out.trigger_refusal = form.trigger_refusal;
	out.action_refusal = form.action_refusal;
	if (native.ids.lists.size() >= 2) {
		for (const RecordIds &id : native.ids.lists[0])
			if (out.triggers.size() < native.native.triggers.size())
				out.triggers.push_back(line_of(document, native, {event, k(MissionKind::Trigger), id.id}, names, header, out.notes));
		for (const RecordIds &id : native.ids.lists[1])
			if (out.actions.size() < native.native.actions.size())
				out.actions.push_back(line_of(document, native, {event, k(MissionKind::Action), id.id}, names, header, out.notes));
	}
	if (out.actions.empty()) out.notes.insert(out.notes.begin(), {out.event, kBoardNoActions, "It does nothing: it has no action."});
	if (out.triggers.empty() && !out.at_start && !out.at_end)
		out.notes.insert(out.notes.begin(),
		                 {out.event, kBoardRightAway, "It has no trigger: it fires right away, as the mission runs."});
	wait_note(out.notes, out.event, "wait", out.delay_steps);
	if (out.repeats) wait_note(out.notes, out.event, "repeat", out.repeat_steps);
	if (live && live->mission == document.path()) out.live = live->event(out.index);
	return true;
}

MissionBoard mission_board(const MissionDocument &document, const MissionNames &names, const BoardQuery &query,
                           const LiveLogic *live) {
	MissionBoard out;
	const std::string filter = lowered(query.filter);
	for (const Node *row : document.rows_of(MissionKind::Event)) {
		++out.events;
		BoardCard card;
		if (!mission_board_card(document, row->id, names, card, live)) continue;
		out.noted += !card.notes.empty();
		if (query.attention && card.notes.empty()) continue;
		if (!filter.empty() && lowered(card.title).find(filter) == std::string::npos &&
		    lowered(card.words.sentence).find(filter) == std::string::npos)
			continue;
		card.folded = std::find(query.folded.begin(), query.folded.end(), row->id) != query.folded.end();
		out.cards.push_back(std::move(card));
	}
	return out;
}

} // namespace opennova::editor
