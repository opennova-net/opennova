#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <formats/def/def.h>
#include <formats/def/def_schema.h>

namespace opennova::def {

// What a save needs to put a def file back as it was read, beside the records the game reads (ADR 0003:
// noted layout and parsed data, never the file's bytes passed through; the def catalogs' "one field
// changed, one line changed"): each line's blanks before its first word, its words as the file spells
// them (a key's case, a number's own spelling, the words the game skips), what stands between them, its
// comment, its ending, and what the parser made of it: a line of a record's (its step, def.h's line order
// steps), the record's header or its end, where a nested record (a sight, an action block) began, or a
// line the game reads nothing of (a blank, a comment, a line the parser skips). A writer given the notes
// writes each line as noted while what the writer would put down for it is what it put down for the
// record as read (the baseline), and the line in its own form, keeping the noted blanks, comment and
// unchanged words, where not.

enum class DefNotedRole : uint8_t {
	Free,   // read for nothing: a blank, a comment, a line the game skips
	Header, // the record's header (`begin "..."`, `weapon "..."`, `ammo ...`, `action "..."`, `powerup ...`)
	Line,   // a line of the record's, its step
	Nested, // where a nested record began (its own notes hold its lines)
	End,    // the record's `end`
};

struct DefNotedLine {
	DefNotedRole role = DefNotedRole::Free;
	uint8_t step = 0;    // Line, Nested: the record's step (a nested record's: DEF_LINE_ORDER_ROWS or _BLOCKS)
	uint64_t nested = 0; // Nested: the note of the record that began there
	std::string indent;  // the blanks before the first word
	std::vector<std::string> words;
	std::vector<std::string> gaps; // gaps[i] stands between words[i] and words[i + 1]
	std::string tail;    // the blanks after the last word, then the comment
	std::string eol;     // "\r\n", "\n", or "" (a last line with none)
	std::string text() const;
};

// A line's text cut into its parts (the inverse of DefNotedLine::text): its ending, its blanks, its words
// (a quoted run one word, a comma or a blank ending one outside quotes, as the game's tokenizer ends a
// token [orig: Terrain_TokenizeConfigLine @ 0x53CB60, delimiters @ 0x53CC33..0x53CC4C, quote @
// 0x53CC4E..0x53CC70]) and its comment (`//` or `;` outside quotes, where trim_def_line cuts the line).
DefNotedLine def_noted_line(const char *text, size_t length);

// What the writer put down for a record as read (the baseline): its header, its end, and each step's own
// lines (a nested record's are its own).
struct DefNotedBaseline {
	std::string header;
	std::string end;
	std::vector<std::pair<uint8_t, std::string>> steps;
	const std::string *step(uint8_t of) const;
};

struct DefNotedRecord {
	DefRecordKind kind = DefRecordKind::Item;
	std::vector<DefNotedLine> lines; // the lines before its header since the last record's, then its own
	DefNotedBaseline baseline;
};

// A file's notes: its lines before its first record, each record's (a record's `note` names its own), the
// lines after its last. `stamp` tells one file's notes from another's (a record pasted from another file
// names none of these).
struct DefTextNotes {
	uint32_t stamp = 0;
	std::vector<DefNotedLine> leading;
	std::vector<DefNotedRecord> records;
	std::vector<DefNotedLine> trailing;
	// The record a note names, of `kind`, in these notes; null for none.
	const DefNotedRecord *record(uint64_t note, DefRecordKind kind) const;
	size_t footprint() const;
};

// A record's note: the notes' stamp and its place among their records.
inline constexpr uint64_t def_note_of(uint32_t stamp, size_t index) {
	return (uint64_t(stamp) << 32) | uint64_t(uint32_t(index + 1));
}

// The parsers with notes taken (`notes` filled; a record's note names its noted lines and baseline).
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

// Each record's baseline (DefNotedRecord::baseline): the writer's form of it as read.
void def_note_baseline(const DefItemsFile &file, DefTextNotes &notes);
void def_note_baseline(const DefWeaponsFile &file, DefTextNotes &notes);
void def_note_baseline(const DefAmmoFile &file, DefTextNotes &notes);
void def_note_baseline(const DefPowerupFile &file, DefTextNotes &notes);

// The step a line of `kind` names by its key (def_note_line's lookup), or -1 for none.
int def_line_step(DefRecordKind kind, const char *line, size_t length);

} // namespace opennova::def
