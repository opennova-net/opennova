#pragma once

#include <cstddef>
#include <cstdint>
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
// not. Small: the tools of a list inside a panel (the inspector's, the outline's).
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

// The first line of `text`, cut to `width` with "..." where it is cut (narrower than the
// "...", what fits of the text alone).
std::string fit(const std::string &text, float width);
// A button at most `width` wide: its label cut to fit, its id `id` whatever the cut leaves.
bool fitted_button(const std::string &label, const char *id, float width);
// The first line of `text` in what is left of the line, cut to fit. Hovered, it shows `tip`
// when one is given, else the whole text when it was cut.
void clipped_text(const std::string &text, const std::string &tip = std::string());
// The tooltip of the item just drawn (a disabled one too), wrapped: a finding's message and
// a fix's detail run long. None for "". Every window's hover text goes through it.
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

} // namespace opennova::editor::ui_kit
