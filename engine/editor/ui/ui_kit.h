#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <editor/model/diagnostic.h>
#include <editor/model/document.h>

// ImGui's colour, which the colour functions below return: declared, not included, so this
// header stays free of ImGui like every other header of the editor's windows.
struct ImVec4;

// The small pieces the editor's windows draw with (ADR 0046 S11): a row of controls that
// wraps whole controls instead of running past the window's edge, the tools of a list's
// selected record on such a row, a filter box, text cut to its cell with the whole of it in
// a tooltip, a finding's severity as a drawn mark (alone, or with a count), the
// unsaved-changes dot, a record's change since the last save as a dot before its name, and
// what a view shows when it has nothing to list.
namespace opennova::editor::ui_kit {

// A row of controls that wraps whole controls: before each control, next(width) keeps it
// on the line when it fits in what is left of the line; otherwise the control starts the
// next line. The width is the control's own (the measures below).
class WrapRow {
public:
	WrapRow();
	void next(float width);

private:
	float right_ = 0.0f; // the right edge of the row's lines, in screen space
	bool first_ = true;
};

// How wide a control is, for WrapRow::next: a button (a "##" suffix not drawn), a check
// box with its label, a field `width` wide with its label on its right (none for "" or a
// "##" label), a line of text.
float button_width(const char *label);
float checkbox_width(const char *label);
float field_width(float width, const char *label);
float text_width(const char *text);

// A button of a row of tools, on `row`: pressed only while `enabled` (a disabled one never
// acts, however it is activated), its tooltip what it does, or while it cannot act, why
// not. Small: the tools of a list inside a panel (the inspector's, the outline's). A label
// wider than the row's whole line is cut to it ("..."), the button then found by "###label".
bool tool(WrapRow &row, const char *label, bool enabled, const std::string &tip, bool small = false);

// The tools of a list's selected record: Add, Duplicate, Remove, Up and Down, each disabled
// while it cannot act, with its tooltip saying what it does or why it cannot. A label may
// name what it acts on ("Add screen", "Remove screen..."); a null one leaves the tool out.
enum class RowTool { None, Add, Duplicate, Remove, Up, Down };
struct RowTools {
	size_t count = 0;           // the records the list has
	size_t selected = SIZE_MAX; // the selected one's index among them (SIZE_MAX: none)
	size_t max = 0;             // the most it holds (0: any number): full, nothing is added or duplicated
	const char *add = "Add";
	const char *duplicate = "Duplicate";
	const char *remove = "Remove";
	const char *up = "Up";
	const char *down = "Down";
	std::string add_tip = "Adds one at the end.";
	std::string duplicate_tip = "A copy of the selected one, right after it.";
	std::string remove_tip = "Removes the selected one.";
	const char *pick = "Select one first."; // what the tools of the selected one say while none is
	const char *keeps = nullptr;  // why the selected one may not go ("A menu keeps at least one screen.")
	const char *locked = nullptr; // why it stays where and as it is: no Duplicate, Remove, Up or Down
	bool small = false;
};
// The tools on `row`; the one pressed (None: none).
RowTool row_tools(WrapRow &row, const RowTools &tools);

// A filter box `width` wide in all (0: the rest of the line): the hint in it while it is
// empty (`tip`, when given, in its tooltip), then an x (drawn: the font has no symbols) that
// clears it. Ctrl+F in the window that has the focus gives the keyboard to its filter, unless
// `ctrl_f` is false (a Document tab's view: Ctrl+F there opens the find bar). True when its
// text changed.
bool filter_box(const char *id, char *text, size_t size, const char *hint, float width = 0.0f,
                const char *tip = nullptr, bool ctrl_f = true);

// A text field's buffer over a value the session holds (the workspace's, the MCP gaps lane): the field
// takes the value whenever it moved from the value it last took (`follow`, each frame before the field
// draws), and what the person typed goes to the session (`sent`, once the field says it changed), so the
// session's value is the field's whoever set it, a person or the editor MCP. What was typed stays shown
// until the session's value moves (a field being typed in keeps its own text the while: ImGui's).
template <size_t N> struct HeldText {
	char text[N] = "";
	std::string seen;
	void follow(const std::string &value) {
		if (value == seen) return;
		seen = value;
		const size_t n = std::min(value.size(), N - 1);
		std::memcpy(text, value.data(), n);
		text[n] = '\0';
	}
	std::string sent() const { return text; }
};

// Any control's value over one the session holds (a toggle, a choice, a mask): `follow` takes the session's
// value when it moved from the one last taken (true then: the control shows it), each frame before the
// control draws; what the person picks goes to the session, the control showing it the while.
template <class T> struct Held {
	T seen{};
	bool known = false;
	bool follow(const T &value) {
		if (known && value == seen) return false;
		known = true;
		seen = value;
		return true;
	}
};

// A popup whose being open is the session's (a part of the workspace, the MCP gaps lane): opened while the
// session holds it open (`held`), closed as the session closes it. begin() draws it (true: drawing, the
// caller's EndPopup after; `modal` a modal, `flags` ImGui's window flags, `closable` a modal's title-bar
// close button). `dismissed()` after a begin that did not draw: ImGui closed it itself (a click outside it,
// the close button) while the session holds it open, and the caller asks the session to close it, once. A
// close the caller makes itself (Cancel, Create: its own request asks the session) is close(), inside the
// popup: it is not opened again until the session has closed it.
class HeldPopup {
public:
	bool begin(const char *id, bool held, bool modal = false, int flags = 0, bool closable = false);
	bool dismissed() const { return dismissed_; }
	// Drawn the frame before (a popup several owners share by its name begins only its asker's).
	bool shown() const { return shown_; }
	void close();

private:
	bool shown_ = false;   // drawn the frame before
	bool closing_ = false; // closed here, the session holding it open still
	bool dismissed_ = false;
};

// A number of bytes as a list's cell says it: "512 B", "3.4 KB", "12.0 MB".
std::string size_text(uint64_t bytes);
// The first line of `text`, cut to `width` with "..." where it is cut (narrower than the
// "...", what fits of the text alone).
std::string fit(const std::string &text, float width);
// `text` cut to `width` in its middle ("C:/mods/...ilds/My Mod"), its start and its end kept: a path,
// whose end tells two apart.
std::string fit_middle(const std::string &text, float width);
// A button at most `width` wide: its label cut to fit, its id `id` whatever the cut leaves.
bool fitted_button(const std::string &label, const char *id, float width);
// The first line of `text` in what is left of the line, cut to fit. Hovered, it shows `tip`
// when one is given, else the whole text when it was cut.
void clipped_text(const std::string &text, const std::string &tip = std::string());
// The tooltip of the item just drawn (a disabled one too), wrapped: a finding's message and
// a fix's detail run long. None for "". It shows in place of any tooltip set before it in the
// frame (as ImGui::SetTooltip does: a table's header sets its own for a label it cut), so an
// item shows one. Every window's hover text goes through it.
void tooltip(const std::string &text);
// Whether the item just drawn shows its tooltip now: hovered, a disabled one too.
bool tooltip_hovered();
// The same tooltip, its text made only while it shows (`make()` returns it): a row's tip that
// would cost a string a frame (a file's path, kind, size and counts).
template <class Make> void tooltip_lazy(Make &&make) {
	if (tooltip_hovered()) tooltip(make());
}

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

// What a view shows when it has nothing to list: a line of disabled text and, optionally,
// a second saying what to do.
void empty_state(const char *text, const char *hint = nullptr);

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

// While one lives, a tooltip (`tooltip`) shows above its item, its bottom edge on the item's top
// edge, rather than below the pointer; the pointer's own place where there is no room above. For the
// items that sit over the rect of a Godot Control placed over the window (a text tab's toolbar over
// the script device), whose tips below them would draw under it.
class TipsAbove {
public:
	TipsAbove();
	~TipsAbove();
	TipsAbove(const TipsAbove &) = delete;
	TipsAbove &operator=(const TipsAbove &) = delete;

private:
	bool before_ = false;
};

} // namespace opennova::editor::ui_kit
