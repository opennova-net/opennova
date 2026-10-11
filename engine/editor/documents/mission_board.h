#pragma once

// The Event board's portable half (ADR 0046 S24, The board): a mission's events as cards, each reading as its
// sentence, its triggers and actions as lines of words whose values are chips. Built from the logic's forms
// (documents/mission_logic.h) and the words' runs (mission_sentence.h), so the board says what the Inspector and
// the outline say. Its notes are the editor's own reading of a card for the author (an event that does nothing,
// a value left unset, a zone the game drops): listed, never gating (the build gate is retail's alone).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <editor/documents/mission_live.h>
#include <editor/documents/mission_logic.h>
#include <editor/documents/mission_sentence.h>
#include <editor/model/value.h>
#include <formats/mission/mission_params.h>

namespace opennova::editor {

class MissionDocument;

using BoardRun = LogicRun;

// A value of a line, a chip: its slot (0 for param1) and field, its label, its words and value, its kind, its
// unit; whether it is picked by name (an entity, an area, a group, a path, an event), whether that pick is left
// unset (0, or -1, where the type reads a record; an event's 0 is the first event), and whether what it names is
// missing (an SSN no entity holds, a zone no area has, an event past the last).
struct BoardChip {
	int slot = 0;
	std::string field, label, words;
	int64_t value = 0;
	mission::ParamKind kind = mission::ParamKind::Raw;
	const char *unit = "";
	bool picks = false, unset = false, missing = false;
};

// A trigger or an action of a card: the record, its list and its place there, its type (null for one no row
// names), its words as runs, a chip per parameter its type reads, a trigger's negation and its join to the
// next, and whether it is the last of its list.
struct BoardLine {
	NodeAddress record;
	bool action = false;
	size_t index = 0;
	const LogicType *type = nullptr;
	std::string type_words;
	std::vector<BoardRun> runs;
	std::vector<BoardChip> chips;
	bool negated = false;
	LogicJoin join = LogicJoin::And;
	bool last = false;
};

// A note on a card (never gating): the record it is about (the event, or a line's), its code and its words.
struct BoardNote {
	NodeAddress record;
	const char *code = "";
	std::string words;
};

// An event as a card: its event and index, its title ("Event 3", or the name it is given), its words, its
// flags, its wait and repeat in steps, its lines, its notes, why it takes no more triggers or actions ("" where
// it does), whether it shows folded, and the running game's report of it (null: none).
struct BoardCard {
	NodeAddress event;
	size_t index = 0;
	std::string title;
	EventWords words;
	bool repeats = false, at_start = false, at_end = false;
	int64_t delay_steps = 0, repeat_steps = 0;
	std::vector<BoardLine> triggers, actions;
	std::vector<BoardNote> notes;
	std::string trigger_refusal, action_refusal;
	bool folded = false;
	const LiveEvent *live = nullptr;
};

// What the board shows: its filter (the cards whose title or words hold it, without case), the events shown
// folded, and Needs attention (only the cards with notes).
struct BoardQuery {
	std::string filter;
	std::vector<NodeId> folded;
	bool attention = false;
};

// The cards shown, in the events' order, and how many events the mission holds and how many of its cards have
// notes (whatever the filter shows).
struct MissionBoard {
	std::vector<BoardCard> cards;
	size_t events = 0;
	size_t noted = 0;
};

// The note codes.
inline constexpr const char *kBoardNoActions = "board.no_actions";
inline constexpr const char *kBoardRightAway = "board.right_away";
inline constexpr const char *kBoardUnset = "board.unset";
inline constexpr const char *kBoardMissing = "board.missing";
inline constexpr const char *kBoardZoneDropped = "board.zone_dropped";
inline constexpr const char *kBoardWaitWraps = "board.wait_wraps";
inline constexpr const char *kBoardUnread = "board.unread";

// The board over the mission as it stands; `live` the running game's report of it (its events' cards carry
// theirs), null for none.
MissionBoard mission_board(const MissionDocument &document, const MissionNames &names, const BoardQuery &query,
                           const LiveLogic *live = nullptr);
// One event's card (no filter), false for a record that is no event of the mission.
bool mission_board_card(const MissionDocument &document, NodeId event, const MissionNames &names, BoardCard &out,
                        const LiveLogic *live = nullptr);

} // namespace opennova::editor
