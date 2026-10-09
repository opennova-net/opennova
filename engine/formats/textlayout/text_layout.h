#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace opennova::textlayout {

// The layout of a line-oriented text file, modeled beside the records a format's reader makes of it (ADR 0003
// holds: the maintainer's ruling of 2026-10-04, "model it, generate it", first made for the def catalogs,
// formats/def/def_notes.h, and here for the formats that share it: Avatars.def, charattr.def, score.ini and the
// AI profiles). No line's text is kept to be put back. Each line is modeled as what the reader made of it, an
// entry of a record (a key and its values, a header, a brace), the place a nested record began, or a line the
// reader reads nothing of (a blank, a comment, a key no arm reads, a section the game never reaches); and as
// its parts: the blanks before its first word, its words, what stands between them, the blanks after the last
// with the comment, and its ending. An entry's words are modeled against the writer's words for it as read:
// each word one of the writer's in the file's spelling, which stands while the writer's word is the one it
// wrote for the record as read, or a token the reader reads nothing of. A format's writer puts its records down
// as lines (OutRecord) and `compose` generates every byte from them and this data: a file read and written
// again comes out as it was because the writer makes it so, and one field changed changes its one line. None
// of it is shown or edited: it is carried for the save alone.

// A line cut into its parts.
struct Line {
	std::string indent;             // the blanks before the first word
	std::vector<std::string> words; // the words as the file spells them (a quoted run with its quotes)
	std::vector<std::string> gaps;  // gaps[i] stands between word i and word i + 1
	std::string tail;               // the blanks after the last word, then the comment
	std::string eol;                // the ending: "\r\n", "\n", "" (a last line with none)
	std::string text() const;       // the line made of its parts
};

// A format's cut of one line's text (its ending included, as the format's reader ends it) into its parts by
// the format's own tokenizer; text() of the cut is the text.
using Cut = Line (*)(const char *text, size_t length);

// The cut of the shared ASCII walk's tokenizer (Avatars.def, the AI profiles, the def families): words end at
// a blank or a comma outside quotes, a quoted run is a word with its quotes, and `//` or `;` outside quotes
// starts the comment [orig: Terrain_TokenizeConfigLine @ 0x53CB60, the delimiters @ 0x53CC33..0x53CC4C, the
// quote @ 0x53CC4E..0x53CC70, the comment @ 0x53CC16..0x53CC31]. A CR or an LF inside the line is a byte of a
// word (the walk splits at a CR LF pair alone).
Line cut_ascii_walk(const char *text, size_t length);

// A word of an entry's line as the file has it: one of the writer's words for the entry as read (`index`) in
// the file's spelling, or a token the reader reads nothing of (`written` false).
struct Word {
	bool written = true;
	size_t index = 0;
	std::string as_written; // the writer's word as read
	std::string spelling;   // the file's
};

// An entry's line modeled against the writer's words for it as read: its words in the file's order and the
// writer's words it leaves out (their places and the words as read). Not `modeled`: a line of the record's the
// writer put down no line for as read (a value the writer leaves out, a key read twice), kept as its tokens
// while the writer puts none down for its entry.
struct Shape {
	bool modeled = false;
	std::vector<Word> words;
	std::vector<std::pair<size_t, std::string>> left_out;
	// The writer's words from this place on are a set (flags, attributes; OutLine::set_from): each of the file's
	// stands while the writer still puts its word down, wherever it stands.
	size_t set_from = SIZE_MAX;
};

enum class Role : uint8_t {
	Free,  // read for nothing: kept as its tokens
	Entry, // a line of its record's: `entry` names it
	Child, // where a nested record began (no text of its own: the record's lines are its own)
};

struct NotedLine {
	Role role = Role::Free;
	std::string entry;  // Entry: which of its record's lines it is (the key the writer names it by)
	uint64_t child = 0; // Child: the note of the record that began there
	Line line;          // the line's parts (an Entry's words cleared once it is modeled)
	Shape shape;        // Entry
};

// A record's lines in the file's order, the lines read for nothing among them, and the writer's lines for it
// as read that the file has no line of (a value the file leaves out): left out while they are as read.
struct NotedRecord {
	uint64_t parent = 0;
	std::vector<NotedLine> lines;
	std::vector<std::pair<std::string, std::string>> left_out; // entry, the writer's form as read
};

// A file's notes: its records, the file itself the first (root()), each named by its note (the notes' stamp
// and its place). A record pasted from another file names none of these.
struct Notes {
	uint32_t stamp = 0;
	std::vector<NotedRecord> records;
	uint64_t root() const;
	const NotedRecord *record(uint64_t note) const;
};

inline constexpr uint64_t note_of(uint32_t stamp, size_t index) {
	return (uint64_t(stamp) << 32) | uint64_t(uint32_t(index + 1));
}

// What a writer puts down for a record, in its order: an entry, its key and its line's text in the writer's
// own form (no ending), or a nested record (its place in `children`). Of several lines of an entry the file
// has, the one the reader keeps is the last (`last`), else the first; the others are read for nothing. An
// entry of no key is the writer's own separator (a blank line between records): put down in its form alone,
// never over a file's layout.
struct OutLine {
	std::string entry;
	std::string form;
	int child = -1;
	bool last = true;
	// The words from this place on are a set the reader takes in any order (a mask's words): the file's words of
	// it stand while the writer still puts them down, in the file's order, the writer's others after them.
	size_t set_from = SIZE_MAX;
};

// A record as its writer puts it down: its note (0 for one the file has none of: written in the writer's
// form), its kind (the nested records of a kind take the file's places of their kind in the writer's order: a
// reorder moves them, with the lines read for nothing before each), its lines, its nested records.
struct OutRecord {
	uint64_t note = 0;
	std::string kind;
	std::vector<OutLine> lines;
	std::vector<OutRecord> children;
};

// The parsers' recorder of a file's notes, told where each line begins and what the reader made of it. A
// line goes to the innermost record open when it began, unless it is marked otherwise; a record opened on a
// line holds that line. With no notes asked for, every call does nothing.
class Noter {
public:
	Noter(const char *text, size_t size, Notes *notes, Cut cut);
	bool on() const { return notes_ != nullptr; }
	// The file's own record.
	uint64_t root() const;
	// A line is [begin, next): its text and its ending, as the reader cuts it. Called for every line in order.
	void line(size_t begin, size_t next);
	// The current line opens a record nested in `parent` (the place it began in the parent's lines, the line
	// its own): its note.
	uint64_t open(uint64_t parent);
	// The current line is an entry of `record` (any record open or not).
	void entry(uint64_t record, const std::string &key);
	// After the current line, `record` and the records nested in it are closed.
	void close(uint64_t record);
	// `record` (and what is nested in it) ended before the current line, which is none of its own (a format
	// whose records run to the next one's first line, a score.ini block, a charattr.def section): the lines read
	// for nothing at its end (a blank, a banner before the next record) go to its parent, where they stand.
	void end_before(uint64_t record);
	// After the last line.
	void finish();

private:
	void flush();
	const char *text_;
	size_t size_;
	Notes *notes_;
	Cut cut_;
	std::vector<uint64_t> open_; // innermost last
	bool pending_ = false;
	size_t begin_ = 0, next_ = 0;
	uint64_t target_ = 0;
	NotedLine line_;
};

// Each record's entries modeled against what the writer puts down for the records as read (`as_read`, the
// root's tree): an entry's line paired with the writer's line of its entry (the reader's line of several: the
// last or the first) and its words modeled against the writer's; a line the writer puts down none of kept as
// its tokens; a writer's line the file has none of left out while it is as read. The parsers run it last.
void model(Notes &notes, const OutRecord &as_read, Cut cut);

// The file generated from the writer's records now (`now`, the root's tree) over the notes: every record that
// has notes in the file's form (each entry the writer's words now in the file's shape, the lines read for
// nothing where they stood, a nested record in its place of its kind in the writer's order, an entry or a
// record the writer puts down anew in the writer's form after the line before it in the writer's order, one
// it puts down no more gone with it), a record that has none in the writer's form. `eol` ends a line put down
// anew (the file's own ending where it has one). Null notes: the writer's form throughout.
std::string compose(const Notes *notes, const OutRecord &now, Cut cut, const std::string &eol);

// The writer's form of a record tree alone (no notes): each line its form and `eol`, nested records in place.
std::string form_of(const OutRecord &record, const std::string &eol);

// A record's note and those of the records nested in it, gathered (a copy that names none of the file's
// layout is cleared of them).
void collect_notes(const OutRecord &record, std::vector<uint64_t> &out);

// A fresh stamp for a file's notes (a process-wide counter, never 0).
uint32_t next_stamp();

// The ending the file's lines carry (its first line's that has one), else `fallback`.
std::string file_eol(const Notes &notes, const std::string &fallback);

} // namespace opennova::textlayout
