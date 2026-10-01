#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <editor/model/change_set.h>

namespace opennova::editor {

class TextDocument;

// The fewest characters replaced that take one text to another (ADR 0046 S13 V10): `removed`
// characters from `from` gave way to `inserted` characters. Where runs of that size in several
// places would do it (a character typed into a run of the same character), the one ending at the
// caret in the second text when one does (where a keystroke leaves the caret), else the latest.
struct ShownDiff {
	size_t from = 0;
	size_t removed = 0;
	size_t inserted = 0;
	bool empty() const { return removed == 0 && inserted == 0; }
};

// One edit of a text document planned from what its control holds (ShownText::edit): the span of
// the document it replaces and the text that takes its place, in the document's characters (its
// code page, one byte each), and where the change lies in the shown text.
struct ShownTextEdit {
	TextSpan span;
	std::string text;
	ShownDiff shown;
	bool empty() const { return shown.empty(); }
};

// A text document's text as the script device's control shows it (ADR 0046 S13 V10; CONTEXT.md
// "Script device"): its characters as code points (the code page's, cp1252_decode_byte), one LF
// where a line ends whatever ends it in the document (an LF, a CR LF), and nothing for a byte the
// control cannot hold (a CR alone, which a Godot text control drops as it takes text, and a NUL),
// which stays in the document where it is. Its lines are the document's, one for one.
//
// It plans the one span replacement that takes the document to what the control holds after an
// edit (edit): the fewest characters, byte-exact against the document, every byte the edit does
// not reach kept as the document holds it (a CR LF stays a CR LF, a hidden byte stays where it is
// unless the edit covers what lies on both sides of it); a line end the edit puts in written as the
// document's lines end (CR LF for a type whose game reader ends a line at a CR or at a CR LF, a
// script's and a credits text's; else as the document's first line end, an LF in a text with
// none); a character the code page has no byte for refusing the edit whole.
class ShownText {
public:
	// An empty text (no document shown yet).
	ShownText() = default;
	explicit ShownText(const TextDocument &document);

	const std::u32string &text() const { return text_; }
	// What a line end the control puts in is written as in the document ("\r\n" or "\n").
	const std::string &line_end() const { return line_end_; }

	// The replacement that takes the document to `control` (the control's text after an edit, its
	// caret at `caret`, which picks among runs of one size: ShownDiff). An empty edit when the
	// control holds the document's text; false, with `error`, for one the code page cannot hold.
	bool edit(std::u32string_view control, size_t caret, ShownTextEdit &out, std::string &error) const;

	// The shown offset of a byte offset of the document: the first character shown at or after it
	// (the text's end past the last). A line's start is its line's in the control: the lines are the
	// document's, one for one.
	size_t shown_at(size_t document_offset) const;
	// The document's byte offset where shown character `index` begins (past any byte not shown
	// before it); the document's size for the text's end.
	size_t document_offset(size_t index) const;

	// The fewest characters replaced that take `from` to `to`, among runs of one size the one ending
	// at `caret` in `to` when one does.
	static ShownDiff diff(std::u32string_view from, std::u32string_view to, size_t caret);
	// A shown offset's line and column in a text (both from 0, as a Godot text control counts
	// them), and back (a place past its line's end taken as the line's end, a line past the last
	// as the text's end).
	static void place_of(std::u32string_view text, size_t offset, size_t &line, size_t &column);
	static size_t offset_of(std::u32string_view text, size_t line, size_t column);

private:
	// The document's byte offset just past shown character `index`.
	size_t document_end(size_t index) const;

	std::u32string text_;
	// Each shown character's first byte in the document, then the document's size: one more entry
	// than the text has characters.
	std::vector<size_t> begin_{0};
	// The shown indices of the line ends a CR LF ends, in order (each spans two bytes).
	std::vector<size_t> crlf_;
	std::string line_end_ = "\n";
};

} // namespace opennova::editor
