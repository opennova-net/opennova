#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <formats/def/def.h>
#include <formats/def/def_schema.h>

namespace opennova::def {

// The layout of a def file modeled beside the records the game reads (ADR 0003 holds: the maintainer's
// ruling of 2026-10-04, "model it, generate it"; the def catalogs' "one field changed, one line changed"):
// no line's text is kept to be put back. Each line is modeled as what the parser made of it (a line of a
// record's, its step: def.h's line order steps; the record's header or its end; where a nested record (a
// sight, an action block) began; or a line the game reads nothing of: a blank, a comment, a line the
// parser skips), its blanks before its first word, the separators between its words, its comment, its
// ending, and for a record's line its shape (DefNotedShape): each of its words one of the writer's words
// in the file's spelling (a key's case, an alias, a number's own spelling) or a token the game skips, and
// the writer's words it leaves out. A line the game reads nothing of keeps its tokens as tokens. The
// writer generates every line from the records and this data: a record's line from the words it puts
// down now, each in the file's spelling while it is the word it wrote for the record as read, so a file
// read and written again comes out as it was because the writer makes it so. None of it is shown or
// edited in the editor: it is carried for the save alone.

enum class DefNotedRole : uint8_t {
	Free,   // read for nothing: a blank, a comment, a line the game skips
	Header, // the record's header (`begin "..."`, `weapon "..."`, `ammo ...`, `action "..."`, `powerup ...`)
	Line,   // a line of the record's, its step
	Nested, // where a nested record began (its own notes hold its lines)
	End,    // the record's `end`
};

// A word of a record's line (a Header, a Line, an End) as the file has it, against the words the writer
// puts down for the line (def_note_baseline): one of them (`index`), in the file's spelling, which stands
// while the writer's word is the one it wrote for the record as read (`as_written`); or a token the game
// reads nothing of (`neutral` on an `attrib:` line, a placeholder past an `addeweap`'s angles).
struct DefNotedWord {
	bool written = true;
	size_t index = 0;
	std::string as_written;
	std::string spelling; // the file's spelling of the writer's word, or the token
};

// A record's line modeled against the writer's words for it as read: its words in the file's order, the
// writer's words it leaves out (their places and the words as read: a default the file does not write),
// and the entry it is (its key and first word as the writer writes them: what tells one line of a step of
// several from another). `modeled` false: a line of the record's the writer puts nothing down for, kept as
// tokens (`DefNotedLine::words`) while its step is as read.
struct DefNotedShape {
	bool modeled = false;
	std::vector<DefNotedWord> words;
	std::vector<std::pair<size_t, std::string>> left_out;
	std::string entry;
};

struct DefNotedLine {
	DefNotedRole role = DefNotedRole::Free;
	uint8_t step = 0;    // Line, Nested: the record's step (a nested record's: DEF_LINE_ORDER_ROWS or _BLOCKS)
	uint64_t nested = 0; // Nested: the note of the record that began there
	// A Free line of a block a later one replaced (the weapon reader's earlier `action` of a name): read
	// again on its own it would be a block, so a record written in the writer's form leaves it out, named.
	bool superseded = false;
	std::string indent;  // the blanks before the first word
	// A Free line's tokens (and a record's line until def_note_baseline models it: its shape then, these
	// cleared); a line read for nothing keeps them, written as the tokens they are.
	std::vector<std::string> words;
	std::vector<std::string> gaps; // gaps[i] stands between the line's word i and word i + 1
	std::string tail;    // the blanks after the last word, then the comment
	std::string eol;     // "\r\n", "\n", or "" (a last line with none)
	DefNotedShape shape; // a record's line, modeled
	// The line made of its tokens (a Free line's, or a record's line kept as tokens).
	std::string text() const;
};

// A line's text cut into its parts (the inverse of DefNotedLine::text): its ending, its blanks, its words
// (a quoted run one word, a comma or a blank ending one outside quotes, as the game's tokenizer ends a
// token [orig: Terrain_TokenizeConfigLine @ 0x53CB60, delimiters @ 0x53CC33..0x53CC4C, quote @
// 0x53CC4E..0x53CC70]) and its comment (`//` or `;` outside quotes, where trim_def_line cuts the line).
DefNotedLine def_noted_line(const char *text, size_t length);

// What the writer put down for a record as read (the baseline): each step's own lines (a nested record's
// are its own), which tell a step as read from a step changed, and which of its lines the file left out.
struct DefNotedBaseline {
	std::vector<std::pair<uint8_t, std::string>> steps;
	// The writer's lines the file has no line for (a default the file leaves out), by step: left out while
	// the writer puts them down as it did.
	std::vector<std::pair<uint8_t, std::string>> left_out;
	const std::string *step(uint8_t of) const;
};

struct DefNotedRecord {
	DefRecordKind kind = DefRecordKind::Item;
	std::vector<DefNotedLine> lines; // the lines before its header since the last record's, then its own
	DefNotedBaseline baseline;
};

// A file's notes, its modeled layout: its lines before its first record, each record's (a record's `note`
// names its own), the lines after its last. `stamp` tells one file's notes from another's (a record pasted
// from another file names none of these).
struct DefTextNotes {
	uint32_t stamp = 0;
	std::vector<DefNotedLine> leading;
	std::vector<DefNotedRecord> records;
	std::vector<DefNotedLine> trailing;
	// The record a note names, of `kind`, in these notes; null for none.
	const DefNotedRecord *record(uint64_t note, DefRecordKind kind) const;
};

// A record's note: the notes' stamp and its place among their records.
inline constexpr uint64_t def_note_of(uint32_t stamp, size_t index) {
	return (uint64_t(stamp) << 32) | uint64_t(uint32_t(index + 1));
}

// The parsers with the layout modeled (`notes` filled; a record's note names its lines and baseline, each
// line modeled by def_note_baseline, which the parsers run last).
int def_parse_items_memory(const uint8_t *data, size_t size, DefItemsFile *out, DefParseReport *report,
                           DefTextNotes &notes);
int def_parse_weapons_memory(const uint8_t *data, size_t size, DefWeaponsFile *out, DefParseReport *report,
                             DefTextNotes &notes);
int def_parse_ammo_memory(const uint8_t *data, size_t size, DefAmmoFile *out, DefParseReport *report,
                          DefTextNotes &notes);
int def_parse_powerup_memory(const uint8_t *data, size_t size, DefPowerupFile *out, DefParseReport *report,
                             DefTextNotes &notes);

// A record and what it holds named by no notes (a copy a Duplicate makes: written in the writer's form).
void def_clear_notes(DefRecordKind kind, void *record);

// The parsers' recorder of a file's notes (internal to the family parsers): told where each line begins
// and what the parser made of it. With no notes asked for, every call does nothing.
class DefTextNoter {
public:
	DefTextNoter(const char *text, size_t size, DefTextNotes *notes);
	bool on() const { return notes_ != nullptr; }
	// A line begins at `at` (a place within the text): the line before it ends there.
	void line(const char *at);
	// The current line opens a record of the file (its header line; `header` false: a record of one line,
	// such as a carry limit, the line its own Line at `step`): its note.
	uint64_t open(DefRecordKind kind, bool header = true, uint8_t step = 0);
	// The current line opens a record nested in the open one, where its `step` (ROWS or BLOCKS) stands
	// (`header` false: a row of one line, its line its own Line at `row_step`, closed at once): its note.
	uint64_t open_nested(DefRecordKind kind, uint8_t step, bool header = true, uint8_t row_step = 0);
	// The current line is a line of the open record, at `step` (none: a line of it read for nothing).
	void property(int step);
	// The current line ends the open record.
	void close();
	// The nested record `note` was read for nothing (a later action block of its name replaced it): its
	// lines are its parent's, read for nothing, where it began.
	void drop_nested(uint64_t note);
	// After the last line: the lines left over close the file.
	void finish();

private:
	enum class Target { Pending, Record, Opened, NestedOpened, Closed };
	void flush_line(size_t end);
	DefNotedRecord &record_at(uint64_t note);
	const char *text_;
	size_t size_;
	DefTextNotes *notes_;
	size_t begin_ = SIZE_MAX; // the current line's first byte
	// What the current line is: its role and step, and the record it goes to (a note), as the calls said.
	DefNotedRole role_ = DefNotedRole::Free;
	uint8_t step_ = 0;
	uint64_t target_ = 0;
	uint64_t parent_ = 0;   // NestedOpened: the record it began in
	uint8_t parent_step_ = 0;
	bool closes_ = false;   // the line closes its record (an End, or a one-line record)
	std::vector<uint64_t> open_; // the records open, innermost last
	std::vector<DefNotedLine> pending_;
	bool any_record_ = false;
};

// Each record's baseline (DefNotedRecord::baseline: the writer's form of it as read) and each of its lines
// modeled against the writer's words for it (DefNotedLine::shape, its words then cleared).
void def_note_baseline(const DefItemsFile &file, DefTextNotes &notes);
void def_note_baseline(const DefWeaponsFile &file, DefTextNotes &notes);
void def_note_baseline(const DefAmmoFile &file, DefTextNotes &notes);
void def_note_baseline(const DefPowerupFile &file, DefTextNotes &notes);

// The step a line of `kind` names by its key (def_note_line's lookup), or -1 for none.
int def_line_step(DefRecordKind kind, const char *line, size_t length);

} // namespace opennova::def
