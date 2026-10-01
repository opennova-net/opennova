#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/text_document.h>
#include <editor/preview/shown_text.h>
#include <editor/preview/viewport_model.h>
#include <editor/session/view/view_revisions.h>

namespace opennova::editor {

class TextBurst;

// What a script viewport shows, and why not (ADR 0046 S13 V10): the kind's reason.
enum class ScriptViewStatus : uint8_t {
	NoText, // no text document is open at its path
	Ready,
};
// "no_text", "ready": its token on the wire.
const char *script_view_status_token(ScriptViewStatus status);

// A finding about a line, one of a gutter mark's (ScriptMark): its code (its row's token), its
// severity, its column (from 1; 0 for none) and its message.
struct ScriptMarkFinding {
	std::string code;
	DiagnosticSeverity severity = DiagnosticSeverity::Info;
	size_t column = 0;
	std::string message;
};

// A line's gutter mark (ADR 0046 S13 V10): the findings about a place on the line (each finding's
// line and column, as its row places it in the document's content: a finding about the file as a
// whole is no mark), the worst one's severity its icon, every message its tip, in the findings'
// order. `line` counts from 1 (the document's, which are the control's one for one).
struct ScriptMark {
	size_t line = 0;
	DiagnosticSeverity severity = DiagnosticSeverity::Info;
	std::vector<ScriptMarkFinding> findings;
	// The messages, one a line: the mark's tip.
	std::string tip() const;
};

// A run the control colours, in its own places (lines and columns from 0, characters as it holds
// them: ShownText's): a word of the language the type's reader knows (TextHighlight).
struct ScriptHighlight {
	size_t line = 0;
	size_t column = 0;
	size_t length = 0;
	TextHighlightKind kind = TextHighlightKind::Keyword;
};

// A place the control shows and selects (a RevealText's, ADR 0046 S13 V10), in its own places:
// from (line, column) to (end_line, end_column), an empty selection where they meet (a caret).
// `seq` is the event's: a device takes each once, by it (0: none yet).
struct ScriptReveal {
	uint64_t seq = 0;
	size_t line = 0, column = 0;
	size_t end_line = 0, end_column = 0;
	// The span in the document (its line and column from 1, its length), the envelope's.
	TextSpan span;
};

// The script device's viewport (ADR 0046 S13 V10; ViewportKind::Script, the Main role of every text
// type; CONTEXT.md "Script device"): a text document's text as it stands (never as saved: the text
// is the document's), which the Shell's device shows in a Godot CodeEdit that owns the input in the
// rect the tab reserves. It follows its document into what the device shows: the text as the
// control holds it (ShownText), the gutter marks of the findings at the document's places, the
// highlights its type's reader knows (DocumentType::highlights: a script's keywords, commands and
// operands), whether the document takes an edit now (none when it is held read only, a music
// script's message handler or a credits file its text form cannot carry, nor while an operation
// holds the documents), and the place a RevealText asks it to show (a Go to's span, a Problems
// row's place: selected where a reference or a word of the language starts there, the caret alone
// elsewhere). A Rebuild sets the control's text anew (the document read, read again or first
// shown); an Update brings what changed (an edit, an undo, the findings, the gate, a reveal), the
// control taking the document's text again where it holds another (the document's text is the
// truth). Its state is the base's alone (the size its device draws at where no canvas sizes it);
// it has no options and no camera. Every edit the control makes is a request: `edit` plans the
// span replacement that takes the document to what the control holds, under the keystroke burst's
// gesture.
class ScriptViewport final : public ViewportModel {
public:
	explicit ScriptViewport(std::string path);
	static std::unique_ptr<ViewportModel> make(const std::string &path);

	ScriptViewStatus view_status() const { return reason_; }
	// The text as the control holds it (as of the last follow).
	const ShownText &shown_text() const { return shown_; }
	// Whether the document takes an edit (as of the last follow), and why not ("" when it does).
	bool editable() const { return editable_; }
	const std::string &read_only() const { return read_only_; }
	const std::vector<ScriptMark> &marks() const { return marks_; }
	const std::vector<ScriptHighlight> &highlights() const { return highlights_; }
	const ScriptReveal &reveal() const { return reveal_; }
	// Serials that move when the marks and the highlights are made again (a device sets them again
	// then), and how many times the text was shown anew (made: the document read or changed).
	uint64_t marks_serial() const { return marks_serial_; }
	uint64_t highlights_serial() const { return highlights_serial_; }
	uint64_t texts_made() const { return texts_made_; }

	// The span replacement that takes the document to `control`, what the control holds after an edit
	// (its caret at `caret`, shown offset), at `now` (seconds): an EditRecord of one TextSpanEdit raised
	// into `out`, carrying `burst`'s token; the burst ended first (its EndEdit) when the edit does not
	// go on from it. Nothing raised when the control holds the document's text. False, with `error`,
	// and nothing raised, for an edit the document does not take now (no text open, held read only,
	// an operation holding the documents) or the code page cannot hold.
	bool edit(const ViewportContext &context, std::u32string_view control, size_t caret, double now,
			TextBurst &burst, CanvasRequests &out, std::string &error) const;

	ViewportStatus status() const override;
	const char *reason() const override { return script_view_status_token(reason_); }
	std::string message() const override;
	const std::string &detail() const override { return detail_; }
	const char *units() const override { return "pixels"; }
	ViewportLayout layout() const override { return ViewportLayout(); }
	// No canvas: the control owns the input in its rect (null).
	std::unique_ptr<CanvasHalf> make_canvas() const override;
	ViewportHit hit(const ViewportContext &context, float x, float y) const override;
	bool drag(const ViewportContext &context, const ViewportDrag &drag, CanvasRequests &out,
			std::string &error) const override;
	bool command(const ViewportContext &context, const std::string &name,
			const std::vector<NodeId> &ids, CanvasRequests &out, std::string &error) const override;
	io::JsonValue options_json() const override;
	io::JsonValue body_json(const ViewportInput &input) const override;
	// Its gutter marks: each {line, severity, findings: [{code, severity, column, message}]}.
	io::JsonValue items_json(const ViewportInput &input) const override;
	void receive(const ViewEvent &event) override;

protected:
	ViewportAction follow_(const ViewportInput &input, PreviewClock &clock) override;
	bool takes_(const std::string &member) const override;
	bool check_(const io::JsonValue &json, std::string &error) const override;
	void apply_(const io::JsonValue &json, PreviewClock &clock) override;

private:
	void make_text_(const TextDocument &document);
	void make_marks_(const SessionView &view, const TextDocument &document);
	void place_reveal_(const TextDocument &document);

	ScriptViewStatus reason_ = ScriptViewStatus::NoText;
	std::string detail_;
	ShownText shown_;
	bool editable_ = false;
	std::string read_only_;
	std::vector<ScriptMark> marks_;
	std::vector<ScriptHighlight> highlights_;
	std::vector<TextReference> references_; // the type's references, where a reveal selects a span
	std::vector<TextHighlight> words_;      // the type's highlights, in the document's places
	RevisionKey marks_key_;                 // the findings the marks were made from
	uint64_t marks_serial_ = 0, highlights_serial_ = 0, texts_made_ = 0;
	ScriptReveal reveal_;
	// A RevealText received and not placed yet: its seq and its place in the document.
	uint64_t pending_seq_ = 0;
	size_t pending_line_ = 0, pending_column_ = 0;
};

} // namespace opennova::editor
