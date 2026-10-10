// The text layout of a .mnu file, modeled beside the records the game reads (D-MNU-22). ADR 0003
// holds, under the maintainer's ruling of 2026-10-04 ("model it, generate it"): no element's
// text is kept to be put back. Each element of the file is modeled as what the reader made of it
// (an element the writer puts down for a record, `key` naming which; or one the game reads
// nothing of), and each of its tokens against the writer's own token for it (mnu_write.h): its
// name's spelling (case, an alias such as ULX or SCROLLLEFT, a close tag that names another
// element), each attribute's blanks, name, quoting and value spelling, its text's spelling
// (entities, RAW_TEXT), each beside the writer's token as read. The text between tags (the blanks
// and words of a run, a RAW_TEXT block's body, what follows the text's NUL) is cut into lines of
// parts, as the def catalogs cut a line (formats/def/def_notes.h): each line's blanks before its
// first word, its words, the blanks between and after them, its ending. The blanks and comments
// between elements go with the element they stand by (below).
// The writer generates every byte from the records and this data: a token in the file's
// spelling while the writer's token is the one it wrote for the record as read, else the
// writer's own; so a file read and written again comes out as it was because the writer makes it
// so, and an edit changes only its own tokens. What the game reads nothing of is carried as
// tokens for the save alone; nothing shows it. A parse models the layout only when asked
// (ParseLayout::Text, an editor's load); the game's loads read the records alone.
//
// The rules the generation keeps (mnu_text_layout_write.cpp):
// - A record the layout does not hold (made in code, pasted from another document, a second copy
//   of one) is written in the writer's own layout in the file's style (its indent step, and the
//   line ending of the line it is put beside). It is placed beside the nearest record of its own
//   list the file holds (after the one before it in the model's order, else before the one after
//   it); a record of no list, or of a list none of whose records the file holds, after the
//   nearest record before it in the writer's order that the file holds, at that record's
//   indentation.
// - A removed record takes its element with it, and the text and comments that go with it: the
//   blanks and comments before an element since the line the element before it ends on, and a
//   comment on the line an element ends on. What stands before a close tag after the last
//   element's line is its parent's and stays.
// - Retail reads each list in document order, so a list the model reorders is written in the
//   model's order: the records that keep their order stay in place, the others move with their
//   own layout, each beside the nearest record of its list that stays.
// - A token the game reads nothing of whatever else the file holds (a comment, an attribute or
//   element no parse reads, a WINDOW retail never creates) is kept always. One that only another
//   element makes it read nothing of is kept while that element is written as read, and dropped
//   once it changes, so it can never come to be read: an element a later one replaces (kept while
//   the one that replaces it is as read), what a stopped parse never reached (while the element
//   whose parse stopped is), a singleton read into an empty record (while the writer puts none
//   of its kind down). An attribute's repeat is kept while the attribute it repeats is put down
//   on its element as read (its name and value as read: the reader takes the same one of them),
//   else while its element is written as read.
// - Where the reader reads a value relative to its neighbours, the writer generates it from the
//   model at its place: a POSITION's WIDTH and HEIGHT from the edges before them; a HEADER,
//   BODY or SUBST leaves its COLUMN out while the running index carries it; a table sort key
//   stays where the file has it while retail's walk still reads it as the model's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <formats/mnu/mnu.h>
#include <formats/mnu/mnu_write.h>

namespace opennova::mnu_xml {
struct Document;
struct Node;
} // namespace opennova::mnu_xml

namespace opennova::mnu {

// What a token is to the game.
enum class NotedRole : uint8_t {
	Written, // the writer's token for a record (`key` names which)
	Tied,    // read for nothing while another element stands as read (NotedElement::tied_by): a
	         // repeat, a replaced element, past a parse's stop, a singleton read into an empty record
	Inert,   // read for nothing whatever else the file holds: kept always
};

// A line of the text between tags cut into its parts, as the def catalogs cut a line
// (formats/def/def_notes.h, DefNotedLine): the blanks before its first word, its words and the
// blanks between them, the blanks after its last word, its ending. A blank is a space or a tab; a
// line ends at an LF (a CR before it is the ending's); any other character (a CR alone, a NUL) is
// a word's.
struct NotedLine {
	std::string indent;
	std::vector<std::string> words;
	std::vector<std::string> gaps; // gaps[i] stands between word i and word i + 1
	std::string tail;              // the blanks after the last word (with no word, the indent's)
	std::string eol;               // "\r\n", "\n", or "" (a run's last line)
};

// A run of text cut into lines (each but the last ending with its line ending), and the text the
// lines make.
std::vector<NotedLine> noted_lines(const std::string &text);
void put_lines(const std::vector<NotedLine> &lines, std::string &out);

// A run of an element's content (or the document's).
struct NotedPiece {
	enum class Kind : uint8_t { Text, RawText, Comment, Element };
	Kind kind = Kind::Text;
	std::vector<NotedLine> lines; // Text: the run; RawText: its block's body
	// Comment: the comment as spelled, from its '<' to past its "-->" (a token, as a def line's
	// comment is). RawText: its open tag as spelled, from its '<' to the character the reader skips
	// after the name ("<RAW_TEXT>").
	std::string tag;
	std::string close;    // RawText: its close tag as spelled, from "</" to past its '>'
	uint32_t element = 0; // Element: its index in TextLayout::elements
	// The element piece (an index into the same content) this piece goes with, or -1 for the
	// content's own: the blanks and comments before an element since the line the element before it
	// ends on, and a comment on the line an element ends on, go with that element.
	int32_t owner = -1;
	// RawText: the reader gave the element created last attributes from its closing tag.
	bool carries = false;
	// The text the piece makes from its parts (an Element piece makes none here).
	void put(std::string &out) const;
	bool broken() const { return kind == Kind::Text && lines.size() > 1; } // it holds a line end
};

// An attribute of an element's open tag.
struct NotedAttribute {
	NotedRole role = NotedRole::Inert;
	std::string key;   // written_key of its name on its element ("" for the blanks before '>')
	std::string gap;   // the blanks before it
	std::string name;  // its name as spelled
	std::string value; // the rest as spelled: '=', the value with its quotes, what the reader skipped
	std::string as_name;  // Written: the writer's name for it as read
	std::string as_value; // Written: the writer's value token as read (WrittenAttribute::value_token)
	bool read = false;    // the reader took a value from it (the capture's choice of the written one)
};

struct NotedElement {
	NotedRole role = NotedRole::Inert;
	// Written: the WrittenElement key it spells within its owner; a Tied singleton read into an
	// empty record: the key it was read as.
	std::string key;
	uint32_t parent = UINT32_MAX;  // the element it stands in (UINT32_MAX: the document)
	uint32_t at = 0;               // its piece's index in its parent's content
	std::string after_lt;          // the blanks between '<' and its name
	std::string tag;               // its name as spelled
	std::vector<NotedAttribute> attributes;
	std::string tail;              // what stands in its open tag after its attributes, before '>'
	bool ended = false;            // its open tag ends with '>'
	std::vector<NotedPiece> content;
	bool closed = false;           // a close tag ends it (else the file ends inside it)
	std::string close;             // the close tag as spelled
	bool close_carries = false;    // its close tag gave the element created last attributes
	// Written: what the writer put down for it as read.
	std::string as_tag;
	std::string as_text;           // its text, escaped; a POSITION edge: the number it carried
	WrittenForm as_form = WrittenForm::Leaf;
	uint64_t as_own = 0, as_tree = 0; // WrittenElement::own_digest / tree_digest as read
	// The writer's attributes the file leaves out (their key and value token as read): left out
	// while the writer puts them down as it did (a HEADER's COLUMN the running index carries).
	std::vector<std::pair<std::string, std::string>> left_out;
	bool primary = true;           // the first of the elements one record was read from
	// Tied: the element that keeps it read for nothing while it stands as read (the one that
	// replaces it, the first of a repeat, the element whose parse stopped before it); UINT32_MAX
	// for a singleton read into an empty record (kept while the writer puts none of `key` down).
	uint32_t tied_by = UINT32_MAX;
};

// A file's text layout: its document content (the blanks and comments around its top-level
// elements, and those elements), every element, and what the writer needs to put down an element
// the file does not hold beside its neighbours.
struct TextLayout {
	uint32_t stamp = 0;            // tells one file's layout from another's (Window::source names it)
	SourceEncoding encoding = SourceEncoding::CodePage;
	std::string bom;               // the bytes before the text the loader skips
	std::vector<NotedPiece> content;
	// The text ends at a NUL, where the reader ends (else at the file's end); what follows the NUL
	// is never read, kept as its lines of words (no shipped menu holds a NUL).
	bool nul = false;
	std::vector<NotedLine> after_nul;
	std::vector<NotedElement> elements;
	std::vector<uint32_t> records; // a record's source -> its element
	bool styled = false;           // the file breaks lines (else one line: new elements inline)
	std::string eol;               // its first line ending (a new element with no neighbour's)
	std::string unit;              // its indent step
	bool has_unit = false;
	// The element a record's source names in this layout; null for one it does not name.
	const NotedElement *element(uint64_t source) const;
	uint32_t index(uint64_t source) const; // UINT32_MAX when it names none
};

// The record name `source` carries for the `n`th record (1-based) of the layout with `stamp`.
inline constexpr uint64_t text_layout_source(uint32_t stamp, uint32_t n) { return (uint64_t(stamp) << 32) | n; }

// The reader's recorder (mnu.cpp's TreeReader): told which element each record and singleton
// was read from, and which elements are read for nothing only while another stands. With no
// layout asked for (`on` false), every call does nothing and a record's source is 0.
class TextLayoutCapture {
public:
	TextLayoutCapture(SourceEncoding encoding, bool on);
	bool on() const { return layout_ != nullptr; }
	// A record of a list was read from `node`: the record's source.
	uint64_t record(const mnu_xml::Node &node);
	// One of a kind (`key`, the WrittenElement key) was read from `node` (again, for a repeated
	// element read into the same record).
	void keep(const mnu_xml::Node &node, const std::string &key);
	// `node` is read for nothing while `by` stands as read: replaced by `by`, a later one; a repeat
	// of `by`, the first; past the stop of `by`'s parse.
	void tie(const mnu_xml::Node &node, const mnu_xml::Node &by);
	// After the read: the layout of the text the reader read (`text`, the decoded source; `bom`,
	// the bytes before it), modeled against the writer's words for the records as read; null when
	// none was asked for.
	std::shared_ptr<const TextLayout> finish(const std::u32string &text, const mnu_xml::Document &xml,
	                                         const std::vector<WrittenElement> &screens, std::string bom);

private:
	std::shared_ptr<TextLayout> layout_;
	std::map<const mnu_xml::Node *, std::string> keys_;
	std::map<const mnu_xml::Node *, const mnu_xml::Node *> tied_; // each tied node and its `by`
	std::vector<const mnu_xml::Node *> record_nodes_;
	friend class TextLayoutBuilder;
};

// Whether `doc` is written in its text layout: it has one, read as the encoding it holds now.
bool text_layout_applies(const Document &doc);
// The document in its text layout (mnu_text_layout_write.cpp); `canonical` is the writer's own
// style for a file that names none.
std::string text_layout_generate(const TextLayout &layout, const std::vector<WrittenElement> &screens,
                                 const WrittenStyle &canonical);

} // namespace opennova::mnu
