#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <editor/model/change_set.h>
#include <editor/model/document_base.h>
#include <editor/model/edit.h>
#include <editor/model/text_history.h>

namespace opennova::editor {

// The one change a text document takes (ADR 0046 S13 D9): a span of its text replaced, made in C++
// (TextDocument::replace; the session reads one from a request's edits, the rename makes one per
// use it rewrites). `span` is where (its line and column, 1-based, and how many characters it
// replaces, a line end counting its own), `text` what takes its place, in the document's own
// characters (the game's code page, one byte each).
struct TextSpanEdit : EditPayload {
	TextSpan span;
	std::string text;
	TextSpanEdit(const TextSpan &at, std::string with) : span(at), text(std::move(with)) {}
	const char *token() const override;
};

// The payload token of a span replaced: every text document's one change.
inline constexpr const char *kTextSpanToken = "text.span";

// A name a text names at a span (ADR 0046 S13 D9: a script's FX:NAME, SS:NAME, AMMO:NAME and TT:KEY
// operands), which the asset graph makes an edge of with the span: the reference kind its lookup is
// in, the name as written there, a second name the lookup takes when the first finds nothing (a
// script's AMMO operand: its name, then "ammo_" and its name), the scope it is looked up in ("" for
// any) and where it is.
struct TextReference {
	ReferenceKind kind = ReferenceKind::None;
	std::string value;
	std::string fallback;
	std::string scope;
	TextSpan span;
	// Whether Rename everywhere rewrites it (GraphEdge::rewritable): false for a use whose lookup the
	// graph does not model as the game makes it (a script's text key).
	bool rewritable = true;
};

// How a type's game reader ends a line, which is how Save writes a line's end (ADR 0046 S13 D9).
// AsWritten: the file is written as its text holds it. CrLf: the reader ends a line at CR LF alone
// (a credits file's ConfigFile reader), so an LF alone, which the editor's lines end at, is written
// CR LF. Cr: the reader ends a line at a CR and reads an LF as a blank (a script's reader [orig:
// Script_Compile @ 0x4F32E0..0x4F3321, its comment run to the next CR @ 0x4F54BA..0x4F54D9]), so
// an LF alone and a CR alone are each written CR LF. Either way, the editor's lines and the game's
// are then one.
enum class TextLineEnds { AsWritten, CrLf, Cr };

// How a text type's file is stored when the file is not its text (a credits file's CBIN form, a
// music script's bytecode, a shader's SCR form): made by the type's decode as it reads the file, it
// writes a text back in that form (serialize), keeping what the form holds beside the text (the
// cipher's key, the string table's order), so the text as it was read writes the bytes it was read
// from; and it carries what its type says of the stored file (a shader stored plain, which its
// loader rejects). A save's read-back makes it again from what the save wrote.
class TextEncoding {
public:
	virtual ~TextEncoding() = default;
	// `text` in the stored form; false with `issues` saying why it does not go in it.
	virtual bool encode(const std::string &text, std::string &stored,
			std::vector<SourceIssue> &issues) const = 0;
};

// A type's reading of its stored file (decoded as the game's loader decodes a stored file) into the
// text the document holds: the text, the encoding that writes it back (left null for a file that is
// its text) and the source's issues (a blocking one: the text cannot carry what the file holds, so
// the document shows it and takes no edit and no save). False with `error` when the bytes do not
// read at all.
using TextDecode = bool (*)(const std::vector<uint8_t> &stored, std::string &text,
		std::shared_ptr<const TextEncoding> &encoding, std::vector<SourceIssue> &issues,
		std::string &error);

// A document of text (ADR 0046 S13 D9; CONTEXT.md "Text document"): a script, a music script, a
// credits file, a configuration, a shader, a text, held as the game reads it, in its code page (one
// byte a character, which is what a column counts), as lines. A line ends at LF, a CR before it part
// of its end; a lone CR is text of its line. An edit replaces a span (an Apply of a TextSpanEdit;
// any other edit is refused, document.payload), a batch of them one undo step, each against the
// text as the edits before it left it; a span that runs outside the text refuses the batch
// (document.span) with nothing committed. The history keeps the replacements (TextHistory) under
// the record documents' budget, a gesture's batches folding into one step, and a typing burst's
// (coalesced batches). changes_since answers the spans that changed (TextChanges). Its places are
// "line:column" (locator), what a finding, a graph edge's span, a Go to and a Problems row name.
// Its type (the registry's row by its kind: documents/text_types.h) reads and validates it; the
// document itself names no format. A type whose game reader ends its lines otherwise than at an LF
// (TextLineEnds) has every line written CR LF by Save, the document taking that as a step of its
// text, so it holds the file it wrote (Undo gives the old line ends back, unsaved). A snapshot copies
// the text alone: its history's state, none of its steps.
class TextDocument : public DocumentBase {
public:
	explicit TextDocument(TextDecode decode = nullptr, TextLineEnds ends = TextLineEnds::AsWritten);

	// --- the text ---------------------------------------------------------------------------------
	const std::string &text() const { return text_; }
	// The lines, at least one (an empty text is one empty line; a text ending with a line end has
	// an empty last line after it).
	size_t line_count() const { return line_starts_.size(); }
	// Line `number` (1-based) without its line end; empty past the last.
	std::string_view line(size_t number) const;
	// The offset of a place: a line (1-based) and a column (1-based, up to one past the line's last
	// character); false for a place outside the text.
	bool offset_of(size_t line, size_t column, size_t &offset) const;
	// The span `length` characters from `offset` names (its line and column).
	TextSpan span_at(size_t offset, size_t length) const;
	// The characters a span covers; false for one that runs outside the text.
	bool span_text(const TextSpan &span, std::string &out) const;
	// The first line end its game reader reads otherwise than its lines say (TextLineEnds: an LF
	// alone; for Cr a CR alone too), which Save writes CR LF: its offset, and how many there are;
	// npos and 0 for none (always for AsWritten).
	size_t odd_line_end(size_t *count = nullptr) const;
	TextLineEnds line_ends() const { return ends_; }
	// The stored form's encoding its type's decode made (null for a file that is its text).
	const TextEncoding *encoding() const { return encoding_.get(); }

	// A place's locator ("12:5") and back (false for anything else).
	static std::string locator(size_t line, size_t column);
	static bool read_locator(const std::string &locator, size_t &line, size_t &column);
	// The edit that replaces a span by `text` (in the document's characters), folding with a
	// gesture's other batches or, `coalesce`, a typing burst's.
	static Edit replace(const TextSpan &span, std::string text, bool coalesce = false,
			uint64_t gesture = 0);

	// --- DocumentBase -----------------------------------------------------------------------------
	void end_edit_group() override { history_.end_edit_group(); }
	bool dirty() const override { return history_.dirty(); }
	bool can_undo() const override { return history_.can_undo(); }
	bool can_redo() const override { return history_.can_redo(); }
	uint64_t revision() const override { return history_.revision(); }
	size_t history_bytes() const override { return history_.bytes(); }
	bool changes_since(uint64_t load_generation, uint64_t revision, ChangeSet &out) const override;
	// The text as the file stores it: the text itself, or through the encoding its type's decode
	// made (a refusal says why the text does not go in the stored form).
	SerializeResult serialize() const override;
	std::unique_ptr<DocumentBase> snapshot() const override;
	const TextDocument *as_text() const override { return this; }
	TextDocument *as_text() override { return this; }

protected:
	// A snapshot's copy: the text, its line index and its encoding, and its history frozen
	// (TextHistory::frozen): the steps are never copied.
	TextDocument(const TextDocument &other);

	bool apply_edits(const std::vector<Edit> &edits, Diagnostic &error) override;
	void undo_step() override;
	void redo_step() override;
	bool read_source(const std::vector<uint8_t> &decoded, bool adopt,
			std::vector<SourceIssue> &issues, Diagnostic &error) override;
	void on_saved() override;

private:
	// `count` characters at `offset` replaced by `with`, the line starts following.
	void replace_at(size_t offset, size_t count, const std::string &with);
	void index_lines();
	// Whether the character at `offset` is a line end its reader reads otherwise (odd_line_end).
	bool odd_at(size_t offset) const;
	// The text with every odd line end written CR LF.
	std::string written_text() const;

	TextDecode decode_ = nullptr;
	TextLineEnds ends_ = TextLineEnds::AsWritten;
	std::shared_ptr<const TextEncoding> encoding_;
	std::string text_;
	std::vector<size_t> line_starts_{0};
	TextHistory history_;
};

// The text document a document is, null for another kind (as records_of is for records).
inline const TextDocument *text_of(const DocumentBase &document) { return document.as_text(); }
inline TextDocument *text_of(DocumentBase &document) { return document.as_text(); }
inline std::shared_ptr<const TextDocument> text_of(const std::shared_ptr<const DocumentBase> &document) {
	const TextDocument *text = document ? document->as_text() : nullptr;
	return text ? std::shared_ptr<const TextDocument>(document, text) : nullptr;
}

} // namespace opennova::editor
