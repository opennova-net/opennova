// The tool kit (ADR 0046 d11): the small pieces the windows on the pass draw with, the game's dev
// tools and the OpenNova Editor's alike. A row of controls that wraps whole controls instead of running
// past the window's edge; a button whose tooltip says what it does, or while it cannot act, why not; the
// tools of a list's selected record; a path's field with its Browse...; a filter box; a control over a
// value its owner holds; text cut to its cell with the whole of it in a tooltip; the one tooltip every
// window's hover text goes through; and what a view shows when it has nothing to list.
//
// The editor's kit (editor/ui/ui_kit.h) is this one plus the marks of the editor's own types. ImGui-free
// like every window's header: ui_kit.cpp draws, built with the pass (OPENNOVA_IMGUI).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace opennova::devtools::ui_kit {

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
// A check box on `row`, its tooltip `tip`: true when it changed `value`.
bool check(WrapRow &row, const char *label, bool *value, const std::string &tip);

// A path's field with its label and Browse... beside it, under it when the row is narrow, `width` wide
// (0: the item width): whether Browse... was pressed (never while not `enabled`), the text edited this
// frame, and the field being typed in.
struct PathField {
	bool browse = false;
	bool edited = false;
	bool typing = false;
};
PathField path_field(const char *label, char *buffer, size_t size, const char *browse, float width = 0.0f,
                     bool enabled = true);

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

// A text field's buffer over a value its owner holds (the editor session's, a pushed record's): the
// field takes the value whenever it moved from the value it last took (`follow`, each frame before the
// field draws), and what the person typed goes to the owner (`sent`, once the field says it changed), so
// the owner's value is the field's whoever set it, a person or an MCP client. What was typed stays shown
// until the owner's value moves (a field being typed in keeps its own text the while: ImGui's).
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

// Any control's value over one its owner holds (a toggle, a choice, a mask): `follow` takes the owner's
// value when it moved from the one last taken (true then: the control shows it), each frame before the
// control draws; what the person picks goes to the owner, the control showing it the while.
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

// What a view shows when it has nothing to list: a line of disabled text and, optionally,
// a second saying what to do.
void empty_state(const char *text, const char *hint = nullptr);
// A sentence muted and wrapped (a type's notes in the Inspector, what a view has nothing of).
void note(const std::string &text);

// While one lives, a tooltip (`tooltip`) shows above its item, its bottom edge on the item's top
// edge, rather than below the pointer; the pointer's own place where there is no room above. For the
// items that sit over the rect of a Godot Control placed over the window (the editor's text tab
// toolbar over its script device), whose tips below them would draw under it.
class TipsAbove {
public:
	TipsAbove();
	~TipsAbove();
	TipsAbove(const TipsAbove &) = delete;
	TipsAbove &operator=(const TipsAbove &) = delete;

private:
	bool before_ = false;
};

}  // namespace opennova::devtools::ui_kit
