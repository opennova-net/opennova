#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <editor/assets/asset_kinds.h>
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

// Why the text document takes no edit now ("" when it does): what holds it read only (its first
// blocking issue: a music script's message handler, a credits file its text form cannot carry), else
// the operation holding the documents (the busy gate). What the viewport's `read_only` says and the
// script view's toolbar shows.
std::string script_read_only_reason(const SessionView &view, const DocumentBase &document);

// A finding about a line, one of a gutter mark's (ScriptMark): its code (its row's token), its
// severity, its column (from 1; 0 for none) and its message.
struct ScriptMarkFinding {
	std::string code;
	DiagnosticSeverity severity = DiagnosticSeverity::Info;
	size_t column = 0;
	std::string message;
	// Its message's first sentence (ADR 0046 S15): what the device writes after the line's text.
	std::string note;
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
	// What the line says after its text: the worst finding's note, and how many more there are.
	std::string note() const;
};
// A message's first sentence: up to its first ". " (the period kept), the whole where it has none.
std::string first_sentence(const std::string &message);

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

// What the control is asked to show of the script's help at a place (the MCP gaps lane; session/
// script_assist): the completion list there (what typing a word's character shows), a word's words there
// (what the pointer over it shows), or neither. Asked by the viewport's option `assist`, the place in the
// document's lines and columns (from 1) and in the control's (from 0); `serial` moves with each ask (a device
// takes each once). An edit of the document closes it, and so does its device once it no longer shows it (its
// report: a person's Escape, a click, a key or the pointer moving on closed it, or the control was made again).
// A completion list's ask takes the caret there, as typing there would; a word's words leave the caret and the
// selection as they are.
enum class ScriptAssistOp : uint8_t { None, Complete, Hover };
// "none", "complete", "hover": an op's token, and back (false for another).
const char *script_assist_op_token(ScriptAssistOp op);
bool script_assist_op_from_token(const std::string &token, ScriptAssistOp &out);
struct ScriptAssistAsk {
	ScriptAssistOp op = ScriptAssistOp::None;
	size_t line = 0, column = 0;           // the document's, from 1
	size_t shown_line = 0, shown_column = 0; // the control's, from 0
	uint64_t serial = 0;
};

// The runs of a text document's text its game reader knows as words of its language, each at its span, which
// the script device colours (ADR 0046 S13 V10, S23 C): which reader a text type's highlights come from is the
// view's one table, keyed by the document's type, from that type's port of its reader alone, so nothing is
// coloured that no reader knows. The script's are the WAC compiler's own record of the words it read
// (documents/script_type's script_highlights: a keyword, a command it emitted, an operand it looked a name up
// for); a credits file's are the ConfigFile reader's (each section's label line, each
// entry's key and each of its values where the reader read it [orig: ConfigFile_ParseText @ 0x7608A0]). A type
// whose reader's port says no word it read has none (a music script, a shader, a particle file, the HUD layout,
// a plain text): null.
using ScriptHighlighter = void (*)(const TextDocument &document, std::vector<TextHighlight> &out);
ScriptHighlighter script_highlighter(DocumentTypeId type);

// The script device's viewport (ADR 0046 S13 V10; ViewportKind::Script, the Main role of every text
// type; CONTEXT.md "Script device"): a text document's text as it stands (never as saved: the text
// is the document's), which the Shell's device shows in a Godot CodeEdit that owns the input in the
// rect the tab reserves. It follows its document into what the device shows: the text as the
// control holds it (ShownText), the gutter marks of the findings at the document's places, the
// highlights its type's reader knows (script_highlighter's table: a script's keywords, commands and
// operands, a ConfigFile's sections, keys and values), whether the document takes an edit now (none when it is held read only, a music
// script's message handler or a credits file its text form cannot carry, nor while an operation
// holds the documents), and the place a RevealText asks it to show (a Go to's span, a Problems
// row's place: selected where a reference or a word of the language starts there, the caret alone
// elsewhere). A Rebuild sets the control's text anew (the document first shown, or one the follow
// calls another document); an Update brings what changed (an edit, an undo or a redo, the findings,
// the gate, a reveal), the control taking the document's text again where it holds another (the
// document's text is the truth, whatever moved it). Its state is the base's (the size its device draws
// at where no canvas sizes it) and one option, `assist` {op, line, column} (the MCP gaps lane: the
// completion list or a word's words shown at a place, as typing or the pointer shows them; an edit of the
// document closes it); it has no camera and no canvas (the viewport kinds' row: `canvas` false), so the
// wire refuses a hit, a drag and a command on it, and a render of a row. Every edit the control makes is
// a request: `edit` plans the span replacements that take the document to what the control holds,
// under the keystroke burst's gesture.
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
	// The help asked shown at a place (its option `assist`), placed within the text as of the last follow.
	const ScriptAssistAsk &assist() const { return assist_; }
	// Serials that move when the marks and the highlights are made again (a device sets them again
	// then), and how many times the text was shown anew (made: the document read or changed).
	uint64_t marks_serial() const { return marks_serial_; }
	uint64_t highlights_serial() const { return highlights_serial_; }
	uint64_t texts_made() const { return texts_made_; }

	// The span replacements that take the document to `control`, what the control holds after an edit
	// (its caret at `caret`, shown offset), at `now` (seconds): an EditRecord of the TextSpanEdits
	// (one a place the edit changes: ShownText::edit) raised into `out`, carrying `burst`'s token.
	// The burst is ended first (its EndEdit) when the edit does not go on from it, and an edit at
	// several places is a step of its own: it ends the burst before it and after it. Nothing raised
	// when the control holds the document's text. False, with `error`, and nothing raised, for an
	// edit the document does not take now (no text open, held read only, an operation holding the
	// documents) or the code page cannot hold.
	bool edit(const ViewportContext &context, std::u32string_view control, size_t caret, double now,
			TextBurst &burst, CanvasRequests &out, std::string &error) const;

	ViewportStatus status() const override;
	const char *reason() const override { return script_view_status_token(reason_); }
	std::string message() const override;
	const std::string &detail() const override { return detail_; }
	const char *units() const override { return "pixels"; }
	ViewportLayout layout() const override { return ViewportLayout(); }
	// No canvas: the control owns the input in its rect (null). So no point of it names anything (the
	// wire refuses a hit before it asks: the kinds' row), and it has no handle and no command to plan.
	std::unique_ptr<CanvasHalf> make_canvas() const override;
	ViewportHit hit(const ViewportContext &context, float x, float y) const override;
	bool handle_point(const ViewportContext &context, NodeId id, const std::string &handle, float &x, float &y,
			std::string &error) const override;
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
	// The device no longer showing the help asked (its report of the ask's serial): the assist closed.
	bool report_(const ViewportDeviceReport &report) override;

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
	// The help asked (`assist`), and the serial the last follow placed it at.
	ScriptAssistAsk assist_;
	uint64_t assist_placed_ = 0;
};

} // namespace opennova::editor
