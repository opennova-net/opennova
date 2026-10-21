// The prose rule every window of the editor keeps (the declutter pass; the maintainer's "the UI is cramped
// with overly verbose notes and tips"):
// - Inline: values, counts, names and live state; the one context line under a canvas; a one-line red
//   refusal that disables a control (refusal_text); a one-line amber warning that changes what a button
//   does (warning_line); the text of a confirmation modal.
// - Tooltip: how-to steps, what a control does, the game rule behind a value, the rest of any sentence
//   over one line (help_mark when no control carries it).
// - Cut: anything said twice, click-here tails, prose about the editor's own layout.
// - Empty state: one line (empty_state); the how-to goes in its tooltip.
// - Lengths: an inline line at most ~60 characters, drawn with clipped_text or quiet_text, never wrapped
//   prose (a modal's text is dialog_text; a raw ImGui::TextWrapped is a ratchet, scripts/lint/ratchet_counts.py's
//   editor_ui_wrapped_text); a tooltip at most two sentences.
// - Citations: in no window's words (the one tooltip drops them, a line shows without_citations of its
//   words); the code, the RE records and the MCP's answers keep them.
// - Session strings (a status, a refusal, a document's save words) are asserted by tests and read over
//   MCP: they keep their words, and only where they are drawn changes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <editor/model/diagnostic.h>
#include <editor/model/document.h>
#include <editor/session/view/workspace_view.h>
#include <runtime/devtools/ui_kit.h>

// ImGui's colour, which the colour functions below return: declared, not included, so this
// header stays free of ImGui like every other header of the editor's windows.
struct ImVec4;

// The pieces the editor's windows draw with (ADR 0046 S11): the tool kit both products draw with
// (runtime/devtools/ui_kit.h: rows that wrap, tools, filter boxes, held values, cut text, words without
// their citations, the one tooltip, empty states), and on it what is the editor's own: a popup the session
// holds open, a finding's severity as a drawn mark (alone, or with a count), the unsaved-changes dot, a record's
// change since the last save as a dot before its name, a reference's state, and whether Dear ImGui
// draws over a rect. A name the tool kit declares is never declared here again: it would hide the
// kit's from every `ui_kit::` call.
namespace opennova::editor::ui_kit {

using namespace ::opennova::devtools::ui_kit;

// A popup whose being open is the session's (a part of the workspace, the MCP gaps lane): opened while the
// session holds it open (`held`: the caller passes it only while it may show, the session's shown_modal), closed
// as the session closes it. begin() draws it (true: drawing, the caller's EndPopup after; `modal` a modal,
// `flags` ImGui's window flags, `closable` a modal's title-bar close button; `opened` the workspace's
// WorkspaceView::opened). `dismissed()` after a begin that did not draw: ImGui closed it itself (a click
// outside it, the close button) while the session holds it open, and the caller asks the session to close
// it, once. A close the caller makes itself (Cancel, Create: its own request asks the session) is close(),
// inside the popup: it is not opened again until the session has closed it, or opened a dialog again since
// (`opened` moved past the close: a client's open after it, never lost).
class HeldPopup {
public:
	bool begin(const char *id, bool held, bool modal = false, int flags = 0, bool closable = false, uint64_t opened = 0);
	bool dismissed() const { return dismissed_; }
	// Drawn the frame before (a popup several owners share by its name begins only its asker's).
	bool shown() const { return shown_; }
	void close();

private:
	bool shown_ = false;   // drawn the frame before
	bool closing_ = false; // closed here, the session holding it open still
	bool dismissed_ = false;
	uint64_t opened_ = 0;    // the workspace's opened serial at the last begin
	uint64_t closed_at_ = 0; // ... when the close was made
};

// The colours of a finding's severity (Problems' marks, Output's lines).
ImVec4 severity_color(DiagnosticSeverity severity);
// The warning's colour (a value the game reads otherwise, a line that changes what a button does) and the
// refusal's (why a control cannot act): the Warning and Error severities'.
ImVec4 warning_color();
ImVec4 refusal_color();
// A one-line amber warning that changes what a control does, cut to the line: hovered, `tip` when one is
// given, else the whole text when it was cut.
void warning_line(const std::string &text, const std::string &tip = std::string());
// A one-line red refusal (why the control beside it cannot act), cut to the line, whole in its tooltip.
void refusal_text(const std::string &text);
// The text of a confirmation modal, wrapped at a dialog's width (36 ems): the one wrapped prose the rule keeps,
// never drawn outside a modal (a raw ImGui::TextWrapped is the ratchet's count, scripts/lint/ratchet_counts.py).
void dialog_text(const std::string &text);
// The words a view shows while the project's references are still being read.
const char *reading_references();
// What a list of who uses a thing says when no file of the project names it (`file`: "or what it defines" too).
const char *nothing_names_it(bool file);
// What a confirmation says of its way back: a document's Undo, or a file's chore's Edit > Undo file (`file`).
const char *undo_brings_it_back(bool file);
// Under a list of who uses a thing, one line while it lists none or the project's references are being read: the
// reading's words (`reading`; while it lists some already, that the list may grow in its tooltip), else `none` with
// `how` (what would name the thing) in its tooltip. True when it lists none.
bool no_uses(size_t count, bool reading, const char *none, const char *how = nullptr);
// A list of who uses a thing, headed (a record's Referenced by, a file's Used by, a clip's Played by, the
// missions on a terrain): "<title> (<count>)" on a separator line, `tip` (what it lists) in its tooltip, then
// no_uses. True when there are uses to list under it.
bool uses_heading(const std::string &title, size_t count, bool reading, const std::string &tip, const char *none,
                  const char *how = nullptr);
// A button that names what it acts on, `verb` then `name` ("Import gametext.bin..."; `tail` after the name),
// cut to what is left of the line in its middle (a name's end tells two apart), its id the verb's whatever the
// name: pressed only while `enabled`, `tip` its tooltip.
bool named_action(const std::string &verb, const std::string &name, const char *tail, bool enabled,
                  const std::string &tip, bool small = false);
// A finding's severity as a mark as high as a control, drawn (the editor's font has no
// symbols): an error a filled red circle, a warning an amber triangle, a note a grey ring.
// Hovered, it says which.
void severity_marker(DiagnosticSeverity severity);
// A finding on one line (a refusal of a plan, a record's finding): the mark of its `severity`, then `words`
// without their citations, cut to the line; hovered, `tip` when one is given, else the whole words where cut.
void finding_line(DiagnosticSeverity severity, const std::string &words, const std::string &tip = std::string());
// The same mark over the right end of the item just drawn, inside what the window shows of it (an
// outline's line that spans the window), taking no room: true while the pointer is over it.
bool severity_mark_on_item(DiagnosticSeverity severity);
// How many findings of a severity, after a file's name or in the menu bar: the severity's
// mark, then the number in the severity's colour. The mark sits centred in a box a line of
// text wide and `height` high (a line of text for 0; a control's in the menu bar, whose
// text is centred in one). Its width.
void severity_count(DiagnosticSeverity severity, size_t count, float height = 0.0f);
float severity_count_width(size_t count);
// A file with unsaved changes: a dot (drawn: the font has no bullet) in a box like a
// severity count's mark. Its width.
void unsaved_dot(float height = 0.0f);
float unsaved_dot_width();

// A record's change since the last save, as a dot before its name (drawn): green for a
// record the saved file lacks, amber for one it has otherwise; none while it is unchanged.
// Every row's label starts with kChangeRoom, marked or not, so the names line up; the dot is
// drawn in that room on the item just drawn, whose label's text starts at `x` (screen).
extern const char *const kChangeRoom;
void change_dot(Document::RecordChange change, float x);
ImVec4 change_color(Document::RecordChange change);
// What a change is, for a tooltip ("" for none).
const char *change_words(Document::RecordChange change);

// A reference's state as a word and a colour: Present, Missing, Unverified.
const char *reference_word(ReferenceStatus status);
ImVec4 reference_color(ReferenceStatus status);

// A rect (left, top, right, bottom, in the pass's pixels) of a window, and which window's it is (the
// ID of its root window: a docked or child window's is the host's), kept to ask about again later in
// the frame, once every window has been begun (covered).
struct Cover {
	uint32_t window = 0;
	float left = 0.0f;
	float top = 0.0f;
	float right = 0.0f;
	float bottom = 0.0f;
};
// The rect of the current window, as it is asked about.
Cover cover_of(float left, float top, float right, float bottom);
// Whether Dear ImGui draws anything over the rect: a popup or a modal open anywhere (a menu, a
// dialog), or a window of the same OS window drawn over its window (one floating over a tab; one
// begun after it this frame counts at once, so ask again in the frame's bracket). A tooltip aside
// (a Control placed over the rect, the script device of ADR 0046 S13 V10, is not placed while it is
// covered, and a tooltip comes and goes with every item the pointer crosses: the script view's own
// sit above their items, TipsAbove; another window's that lies over the rect draws under it).
bool covered(const Cover &rect);

} // namespace opennova::editor::ui_kit
