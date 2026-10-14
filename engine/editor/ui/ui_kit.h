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
// (runtime/devtools/ui_kit.h: rows that wrap, tools, filter boxes, held values, cut text, the one
// tooltip, empty states), and on it what is the editor's own: a popup the session holds open, a
// finding's severity as a drawn mark (alone, or with a count), the unsaved-changes dot, a record's
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
// A finding's severity as a mark as high as a control, drawn (the editor's font has no
// symbols): an error a filled red circle, a warning an amber triangle, a note a grey ring.
// Hovered, it says which.
void severity_marker(DiagnosticSeverity severity);
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
