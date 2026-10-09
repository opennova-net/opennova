// The text layout of a .mnu file, modeled beside the records the game reads (D-MNU-22). ADR 0003
// holds, under the maintainer's ruling of 2026-10-04 ("model it, generate it"): no element's
// text is kept to be put back. Each element of the file is modeled as what the reader made of it
// (an element the writer puts down for a record, `key` naming which; or one the game reads
// nothing of), and each of its tokens against the writer's own token for it (mnu_write.h): its
// name's spelling (case, an alias such as ULX or SCROLLLEFT, a close tag that names another
// element), each attribute's blanks, name, quoting and value spelling, its text's spelling
// (entities, RAW_TEXT), each beside the writer's token as read. The blanks and comments between
// elements are kept as the tokens they are, each going with the element it stands by (below).
// The writer generates every byte from the records and this data: a token in the file's
// spelling while the writer's token is the one it wrote for the record as read, else the
// writer's own; so a file read and written again comes out as it was because the writer makes it
// so, and an edit changes only its own tokens. What the game reads nothing of is carried as
// tokens for the save alone; nothing shows it.
//
// The rules the generation keeps (mnu_text_layout_write.cpp):
// - A record the layout does not hold (made in code, pasted from another document, a second copy
//   of one) is written in the writer's own layout in the file's style (its line ending and
//   indent step), placed after the nearest record before it in the writer's order that the file
//   holds, at that record's indentation.
// - A removed record takes its element with it, and the text and comments that go with it: the
//   blanks and comments before an element since the line the element before it ends on, and a
//   comment on the line an element ends on. What stands before a close tag after the last
//   element's line is its parent's and stays.
// - Retail reads each list in document order, so a list the model reorders is written in the
//   model's order: the records that keep their order stay in place, the others move with their
//   own layout.
// - A token the game reads nothing of whatever else the file holds (a comment, an attribute or
//   element no parse reads, a WINDOW retail never creates) is kept always. One that only the
//   rest of its element makes it read nothing of (an attribute's repeat, an element a later one
//   replaces, what a stopped parse never reached) is kept while that element is written as read,
//   and dropped once it changes, so it can never come to be read.
// - Where the reader reads a value relative to its neighbours, the writer generates it from the
//   model at its place: a POSITION's WIDTH and HEIGHT from the edges before them; a HEADER,
//   BODY or SUBST leaves its COLUMN out while the running index carries it; a table sort key
//   stays where the file has it while retail's walk still reads it as the model's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
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
	Tied,    // read for nothing as its element stands (a repeat, a replaced element, past a parse's
	         // stop): kept while that element is written as read
	Inert,   // read for nothing whatever else the file holds: kept always
};

// A run of an element's content (or the document's).
struct NotedPiece {
	enum class Kind : uint8_t { Text, RawText, Comment, Element };
	Kind kind = Kind::Text;
	std::string text;     // Text, RawText, Comment: as spelled
	uint32_t element = 0; // Element: its index in TextLayout::elements
	// The element piece (an index into the same content) this piece goes with, or -1 for the
	// content's own: the blanks and comments before an element since the line the element before it
	// ends on, and a comment on the line an element ends on, go with that element.
	int32_t owner = -1;
	// RawText: the reader gave the element created last attributes from its closing tag.
	bool carries = false;
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
	std::string key;               // Written: the WrittenElement key it spells within its owner
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
};

// A file's text layout: its document content (the blanks and comments around its top-level
// elements, and those elements), every element, and what the writer needs to put down an element
// the file does not hold beside its neighbours.
struct TextLayout {
	uint32_t stamp = 0;            // tells one file's layout from another's (Window::source names it)
	SourceEncoding encoding = SourceEncoding::CodePage;
	std::string bom;               // the bytes before the text the loader skips
	std::vector<NotedPiece> content;
	std::string after_end;         // what follows the reader's end (the first NUL), never read
	std::vector<NotedElement> elements;
	std::vector<uint32_t> records; // a record's source -> its element
	bool styled = false;           // the file breaks lines (else one line: new elements inline)
	std::string eol;               // its line ending
	std::string unit;              // its indent step
	bool has_unit = false;
	// The element a record's source names in this layout; null for one it does not name.
	const NotedElement *element(uint64_t source) const;
	uint32_t index(uint64_t source) const; // UINT32_MAX when it names none
};

// The record name `source` carries for the `n`th record (1-based) of the layout with `stamp`.
inline constexpr uint64_t text_layout_source(uint32_t stamp, uint32_t n) { return (uint64_t(stamp) << 32) | n; }

// The reader's recorder (mnu.cpp's TreeReader): told which element each record and singleton
// was read from, and which elements are read for nothing only as their element stands.
class TextLayoutCapture {
public:
	explicit TextLayoutCapture(SourceEncoding encoding);
	// A record of a list was read from `node`: the record's source.
	uint64_t record(const mnu_xml::Node &node);
	// One of a kind (`key`, the WrittenElement key) was read from `node` (again, for a repeated
	// element read into the same record).
	void keep(const mnu_xml::Node &node, const std::string &key);
	// `node` is read for nothing as its element stands (replaced by a later one, past a stop).
	void tie(const mnu_xml::Node &node);
	// After the read: the layout of the text the reader read (`text`, the decoded source; `bom`,
	// the bytes before it), modeled against the writer's words for the records as read.
	std::shared_ptr<const TextLayout> finish(const std::u32string &text, const mnu_xml::Document &xml,
	                                         const std::vector<WrittenElement> &screens, std::string bom);

private:
	std::shared_ptr<TextLayout> layout_;
	std::map<const mnu_xml::Node *, std::string> keys_;
	std::set<const mnu_xml::Node *> tied_;
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
