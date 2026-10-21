// The tool kit both products' windows draw with (engine/runtime/devtools/ui_kit.h, ADR 0046 d11),
// over a null ImGui backend: the row that wraps whole controls and each control's measure, the text
// cut to a width (at its end, in its middle, a fitted button), a tool that acts only while it is
// enabled, the controls over a value their owner holds, and the quiet pieces of the prose rule (an
// empty state's hint, a quiet line's whole text and a help mark's how-to in tooltips; a filter's
// nothing with its Clear; a selectable cut to its line; a tool that opens its popup).
#include <runtime/devtools/ui_kit.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "imgui_null_backend.h"

namespace ui_kit = opennova::devtools::ui_kit;
using imgui_test::NullBackend;

namespace {

int g_failures = 0;

#define CHECK(cond, message)                                                          \
	do {                                                                              \
		if (!(cond)) {                                                                \
			std::printf("FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, message, #cond); \
			++g_failures;                                                             \
		}                                                                             \
	} while (0)

// The row that wraps whole controls and the text cut to a width: in a window 220 wide, six
// buttons wrap onto more lines and none runs past its edge; each control is as wide as its
// measure says; a text is cut with "...", its first line only, and to what fits of it when
// even the "..." does not; a path is cut in its middle; a fitted button stays within its width.
void test_rows_and_fits() {
	NullBackend backend;
	ImGui::NewFrame();
	// A path cut in its middle, its start and its end kept (the end tells two folders apart).
	{
		const std::string path = "C:/Users/someone/Documents/My Mods/Builds/For Players";
		const float narrow = ImGui::CalcTextSize("C:/Users/some...For Players").x;
		const std::string cut = ui_kit::fit_middle(path, narrow);
		CHECK(cut.size() < path.size() && cut.find("...") != std::string::npos && cut.rfind("C:/", 0) == 0 &&
		              cut.size() >= 7 && cut.compare(cut.size() - 7, 7, "Players") == 0 &&
		              ImGui::CalcTextSize(cut.c_str()).x <= narrow + 0.5f,
		      "a path cut in its middle");
		CHECK(ui_kit::fit_middle(path, 10000.0f) == path, "a path that fits is whole");
	}
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(220.0f, 400.0f));
	ImGui::Begin("kit");
	const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
	ui_kit::WrapRow row;
	float widest = 0.0f;
	std::vector<float> tops;
	for (int i = 0; i < 6; ++i) {
		const std::string label = "Button " + std::to_string(i);
		row.next(ui_kit::button_width(label.c_str()));
		ImGui::Button(label.c_str());
		CHECK(std::fabs(ImGui::GetItemRectSize().x - ui_kit::button_width(label.c_str())) < 0.5f, "a button is as wide as measured");
		widest = std::max(widest, ImGui::GetItemRectMax().x);
		tops.push_back(ImGui::GetItemRectMin().y);
	}
	CHECK(widest <= right, "no button runs past the window's edge");
	CHECK(tops[1] == tops[0] && tops.back() > tops.front(), "whole buttons wrap onto the next line");
	bool flag = false;
	ImGui::Checkbox("Only fixable", &flag);
	CHECK(std::fabs(ImGui::GetItemRectSize().x - ui_kit::checkbox_width("Only fixable")) < 0.5f, "a check box's measure");
	ImGui::SetNextItemWidth(100.0f);
	if (ImGui::BeginCombo("Scope", "Project")) ImGui::EndCombo();
	CHECK(std::fabs(ImGui::GetItemRectSize().x - ui_kit::field_width(100.0f, "Scope")) < 0.5f, "a field and its label");
	const std::string text = "A finding whose message runs much longer than the cell it is drawn in.";
	const std::string cut = ui_kit::fit(text, 100.0f);
	CHECK(cut.size() > 3 && cut.compare(cut.size() - 3, 3, "...") == 0 && ImGui::CalcTextSize(cut.c_str()).x <= 100.0f &&
	              text.compare(0, cut.size() - 3, cut, 0, cut.size() - 3) == 0,
	      "cut to the width, the rest an ellipsis");
	CHECK(ui_kit::fit("Short.", 100.0f) == "Short.", "a text that fits stays whole");
	CHECK(ui_kit::fit("One line.\nAnother.", 200.0f) == "One line....", "the first line only");
	const float dots = ImGui::CalcTextSize("...").x;
	const std::string narrow = ui_kit::fit(text, dots - 1.0f);
	CHECK(narrow.find("...") == std::string::npos && ImGui::CalcTextSize(narrow.c_str()).x <= dots - 1.0f &&
	              ui_kit::fit(text, 0.0f).empty(),
	      "narrower than the ellipsis: what fits of the text alone");
	ui_kit::fitted_button(text, "long", 90.0f);
	CHECK(ImGui::GetItemRectSize().x <= 90.0f, "a fitted button stays within its width");
	ImGui::End();
	ImGui::Render();
}

// One frame of a window holding one tool; where the tool is drawn, and whether it acted.
bool tool_frame(bool enabled, ImVec2 &centre) {
	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(220.0f, 120.0f));
	ImGui::Begin("tools");
	ui_kit::WrapRow row;
	const bool pressed = ui_kit::tool(row, "Act", enabled, enabled ? "Acts." : "Nothing to act on.");
	centre = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
	                (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
	ImGui::End();
	ImGui::Render();
	return pressed;
}

// A tool clicked with the mouse (moved over it, pressed, released): whether it acted.
bool click_tool(bool enabled) {
	NullBackend backend;
	ImVec2 centre;
	bool pressed = tool_frame(enabled, centre);
	ImGui::GetIO().AddMousePosEvent(centre.x, centre.y);
	pressed = tool_frame(enabled, centre) || pressed;
	ImGui::GetIO().AddMouseButtonEvent(0, true);
	pressed = tool_frame(enabled, centre) || pressed;
	ImGui::GetIO().AddMouseButtonEvent(0, false);
	pressed = tool_frame(enabled, centre) || pressed;
	return tool_frame(enabled, centre) || pressed;
}

void test_tool_acts_only_enabled() {
	CHECK(click_tool(true), "an enabled tool acts on a click");
	CHECK(!click_tool(false), "a disabled tool never acts");
}

// The controls over a value their owner holds: each takes the owner's value only when it moved
// from the one it last took, so what a person typed or picked stays shown until then.
void test_held_values() {
	ui_kit::Held<int> held;
	CHECK(held.follow(3), "the first value is taken");
	CHECK(!held.follow(3), "the same value again is not");
	CHECK(held.follow(4) && held.seen == 4, "a moved value is taken");

	ui_kit::HeldText<8> text;
	text.follow("abc");
	CHECK(text.sent() == "abc", "the owner's text is the field's");
	std::snprintf(text.text, sizeof(text.text), "typed");
	text.follow("abc");
	CHECK(text.sent() == "typed", "what was typed stays while the owner's text stands");
	text.follow("a longer text");
	CHECK(text.sent() == "a longe", "a moved text is taken, cut to the buffer");
}

// The kit's quiet pieces, frame by frame in a window 220 wide with the pointer at `mouse`: what
// `draw` writes as text (ImGui's log, the tooltips drawn included), and the centre of the item it
// names by `at` (the last one drawn when it is not set otherwise).
template <class Draw> std::string quiet_frame(ImVec2 mouse, Draw &&draw, ImVec2 *at = nullptr) {
	ImGui::GetIO().AddMousePosEvent(mouse.x, mouse.y);
	ImGui::NewFrame();
	ImGui::LogToBuffer();
	ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(220.0f, 200.0f));
	ImGui::Begin("quiet");
	draw();
	if (at)
		*at = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
		             (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
	ImGui::End();
	const std::string text = GImGui->LogBuffer.c_str();
	ImGui::LogFinish();
	ImGui::Render();
	return text;
}

// The text drawn by `draw` with the pointer away, then over its item: a hint or the whole of a
// cut text is the item's tooltip, never a line of its own.
template <class Draw> void away_and_over(Draw &&draw, std::string &away, std::string &over) {
	ImVec2 at;
	away = quiet_frame(ImVec2(-100.0f, -100.0f), draw, &at);
	quiet_frame(at, draw);
	over = quiet_frame(at, draw);
}

void test_quiet_lines() {
	NullBackend backend;
	std::string away, over;
	// An empty state: one line, what to do in its tooltip.
	away_and_over([] { ui_kit::empty_state("Nothing yet.", "Add one with New."); }, away, over);
	CHECK(away.find("Nothing yet.") != std::string::npos && away.find("Add one with New.") == std::string::npos,
	      "an empty state's hint is no line of its own");
	CHECK(over.find("Add one with New.") != std::string::npos, "the hint is the line's tooltip");
	// A quiet line cut to the window, whole in its tooltip; with a tip, the tip.
	const std::string text = "A note that runs much longer than the window it is drawn in, cut.";
	away_and_over([&] { ui_kit::quiet_text(text); }, away, over);
	CHECK(away.find("...") != std::string::npos && away.find(text) == std::string::npos, "a quiet line is cut to the window");
	CHECK(over.find(text) != std::string::npos, "whole in its tooltip");
	away_and_over([] { ui_kit::quiet_text("Short.", "Why it is so."); }, away, over);
	CHECK(away.find("Why it is so.") == std::string::npos && over.find("Why it is so.") != std::string::npos,
	      "a quiet line's tip in its tooltip");
	// A help mark: "(?)" on the line, the how-to in its tooltip.
	away_and_over(
			[] {
				ImGui::TextUnformatted("Brush");
				ui_kit::help_mark("Drag over the ground to paint.");
			},
			away, over);
	CHECK(away.find("(?)") != std::string::npos && away.find("Drag over") == std::string::npos &&
	              over.find("Drag over the ground to paint.") != std::string::npos,
	      "a help mark's how-to is its tooltip");
}

// A filtered list's nothing: the filter named on one line, and a Clear (its id "Clear##nothing")
// that empties it.
void test_nothing_matches() {
	NullBackend backend;
	char filter[32] = "zebra";
	bool cleared = false;
	ImGuiID id = 0;
	ImVec2 at;
	const auto draw = [&] {
		cleared = ui_kit::nothing_matches(filter, "field") || cleared;
		id = ImGui::GetItemID();
	};
	const std::string text = quiet_frame(ImVec2(-100.0f, -100.0f), draw, &at);
	CHECK(text.find("No field matches") != std::string::npos, "the filter named");
	CHECK(id == ImHashStr("Clear##nothing", 0, ImHashStr("quiet")), "its Clear keeps its id");
	quiet_frame(at, draw);
	ImGui::GetIO().AddMouseButtonEvent(0, true);
	quiet_frame(at, draw);
	ImGui::GetIO().AddMouseButtonEvent(0, false);
	quiet_frame(at, draw);
	CHECK(cleared && filter[0] == '\0', "Clear empties the filter");
}

// A selectable cut to its line, its id the same whatever the cut; a tool that opens its popup.
void test_fitted_selectable_and_tool_popup() {
	{
		NullBackend backend;
		const std::string line = "A line that goes somewhere much further than the window shows";
		ImGuiID id = 0;
		std::string away, over;
		away_and_over(
				[&] {
					ui_kit::fitted_selectable(line, "jump");
					id = ImGui::GetItemID();
				},
				away, over);
		CHECK(id == ImHashStr("###jump", 0, ImHashStr("quiet")), "a fitted selectable's id is its own whatever the cut");
		CHECK(away.find("...") != std::string::npos && away.find(line) == std::string::npos, "its text is cut to its line");
		CHECK(over.find(line) != std::string::npos, "whole in its tooltip");
	}
	for (const bool enabled : {true, false}) {
		NullBackend backend;
		bool open = false;
		ImVec2 at;
		const auto draw = [&] {
			ui_kit::WrapRow row;
			const bool drawing = ui_kit::tool_popup(row, "Show", "show", enabled, "What the picture draws.");
			if (drawing) {
				ImGui::TextUnformatted("in the popup");
				ImGui::EndPopup();
			}
			open = drawing || open;
		};
		quiet_frame(ImVec2(-100.0f, -100.0f), draw, &at);
		quiet_frame(at, draw, &at);
		ImGui::GetIO().AddMouseButtonEvent(0, true);
		quiet_frame(at, draw);
		ImGui::GetIO().AddMouseButtonEvent(0, false);
		quiet_frame(at, draw);
		quiet_frame(at, draw);
		CHECK(open == enabled, enabled ? "a tool popup opens on a click" : "a disabled tool popup never opens");
	}
}

}  // namespace

int main() {
	test_rows_and_fits();
	test_tool_acts_only_enabled();
	test_held_values();
	test_quiet_lines();
	test_nothing_matches();
	test_fitted_selectable_and_tool_popup();
	if (g_failures != 0) {
		std::printf("%d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("ui_kit: all passed\n");
	return 0;
}
