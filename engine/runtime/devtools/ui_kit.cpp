#include <runtime/devtools/ui_kit.h>

#include <base/io/strutil.h>

#include <imgui.h>
#include <imgui_internal.h>

namespace opennova::devtools::ui_kit {

namespace {

// Whether a TipsAbove lives: Dear ImGui is single threaded, one frame is drawn at a time.
bool g_tips_above = false;

// What a label adds to a control's width when it is drawn on its right.
float label_width(const char *label) {
	const float text = ImGui::CalcTextSize(label, nullptr, true).x;
	return text > 0.0f ? ImGui::GetStyle().ItemInnerSpacing.x + text : 0.0f;
}

}  // namespace

WrapRow::WrapRow() : right_(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x) {}

void WrapRow::next(float width) {
	if (first_) {
		first_ = false;
		return;
	}
	if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= right_) ImGui::SameLine();
}

float button_width(const char *label) {
	return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

float checkbox_width(const char *label) { return ImGui::GetFrameHeight() + label_width(label); }

float field_width(float width, const char *label) { return width + label_width(label); }

float text_width(const char *text) { return ImGui::CalcTextSize(text).x; }

bool tool(WrapRow &row, const char *label, bool enabled, const std::string &tip, bool small) {
	const float width = button_width(label);
	row.next(width);
	// A label wider than the whole of its line (a row wraps whole controls, so one wider than a line
	// would run past the window) is cut to the line, the button's id the whole label's (###).
	const float room = ImGui::GetContentRegionAvail().x;
	const std::string shown = width > room ? fit(label, room - ImGui::GetStyle().FramePadding.x * 2.0f) + "###" + label
	                                       : std::string(label);
	ImGui::BeginDisabled(!enabled);
	const bool pressed = small ? ImGui::SmallButton(shown.c_str()) : ImGui::Button(shown.c_str());
	// Held back by an enclosing BeginDisabled too (a view the busy gate holds back, S13 A3), however
	// the button was activated.
	const bool held = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
	ImGui::EndDisabled();
	tooltip(tip);
	return pressed && enabled && !held;
}

bool check(WrapRow &row, const char *label, bool *value, const std::string &tip) {
	row.next(checkbox_width(label));
	const bool changed = ImGui::Checkbox(label, value);
	tooltip(tip);
	return changed;
}

PathField path_field(const char *label, char *buffer, size_t size, const char *browse, float width, bool enabled) {
	PathField out;
	WrapRow row;
	const float field = width > 0.0f ? width : ImGui::CalcItemWidth();
	row.next(field_width(field, label));
	ImGui::SetNextItemWidth(field);
	out.edited = ImGui::InputText(label, buffer, size);
	out.typing = ImGui::IsItemActive();
	row.next(button_width(browse));
	ImGui::BeginDisabled(!enabled);
	out.browse = ImGui::Button(browse) && enabled;
	ImGui::EndDisabled();
	return out;
}

RowTool row_tools(WrapRow &row, const RowTools &t) {
	const bool chosen = t.selected < t.count;
	const bool full = t.max != 0 && t.count >= t.max;
	const std::string at_most = "It holds " + std::to_string(t.max) + " at most.";
	// Why a tool of the selected one cannot act, the first that holds; "" when it can.
	const auto held = [&](bool also, const std::string &because) -> std::string {
		if (!chosen) return t.pick;
		if (t.locked) return t.locked;
		return also ? because : std::string();
	};
	RowTool pressed = RowTool::None;
	if (t.add && tool(row, t.add, !full, full ? at_most : t.add_tip, t.small)) pressed = RowTool::Add;
	if (t.duplicate) {
		const std::string why = held(full, at_most);
		if (tool(row, t.duplicate, why.empty(), why.empty() ? t.duplicate_tip : why, t.small)) pressed = RowTool::Duplicate;
	}
	if (t.remove) {
		const std::string why = held(t.keeps != nullptr, t.keeps ? t.keeps : "");
		if (tool(row, t.remove, why.empty(), why.empty() ? t.remove_tip : why, t.small)) pressed = RowTool::Remove;
	}
	if (t.up) {
		const std::string why = held(t.selected == 0, "It is the first.");
		if (tool(row, t.up, why.empty(), why.empty() ? "Moves the selected one up." : why, t.small)) pressed = RowTool::Up;
	}
	if (t.down) {
		const std::string why = held(t.selected + 1 >= t.count, "It is the last.");
		if (tool(row, t.down, why.empty(), why.empty() ? "Moves the selected one down." : why, t.small))
			pressed = RowTool::Down;
	}
	return pressed;
}

bool filter_box(const char *id, char *text, size_t size, const char *hint, float width, const char *tip, bool ctrl_f) {
	const float clear = ImGui::GetFrameHeight();
	const float gap = ImGui::GetStyle().ItemInnerSpacing.x;
	const float total = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
	// Ctrl+F reaches the filter of the window that has the focus (the shortcut's route).
	if (ctrl_f && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F)) ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(std::max(1.0f, total - clear - gap));
	bool changed = ImGui::InputTextWithHint(id, hint, text, size);
	if (tip) tooltip(tip);
	ImGui::SameLine(0.0f, gap);
	ImGui::PushID(id);
	const bool empty = text[0] == '\0';
	ImGui::BeginDisabled(empty);
	const ImVec2 at = ImGui::GetCursorScreenPos();
	if (ImGui::Button("##clear", ImVec2(clear, clear)) && !empty) {
		text[0] = '\0';
		changed = true;
	}
	if (ImGui::IsItemVisible()) {
		const float pad = clear * 0.3f;
		ImDrawList *draw = ImGui::GetWindowDrawList();
		const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
		draw->AddLine(ImVec2(at.x + pad, at.y + pad), ImVec2(at.x + clear - pad, at.y + clear - pad), color, 1.5f);
		draw->AddLine(ImVec2(at.x + clear - pad, at.y + pad), ImVec2(at.x + pad, at.y + clear - pad), color, 1.5f);
	}
	ImGui::EndDisabled();
	tooltip(empty ? std::string() : "Clear the filter.");
	ImGui::PopID();
	return changed;
}

std::string fit(const std::string &text, float width) {
	const size_t end = text.find('\n');
	const std::string line = text.substr(0, end);
	if (end == std::string::npos && ImGui::CalcTextSize(line.c_str()).x <= width) return line;
	const char *const begin = line.c_str();
	const char *stop = begin;
	const float dots = ImGui::CalcTextSize("...").x;
	// Narrower than the ellipsis: what fits of the text, without one.
	const float room = width < dots ? std::max(0.0f, width) : width - dots;
	ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), room, 0.0f, begin, begin + line.size(), &stop);
	return std::string(begin, stop) + (width < dots ? "" : "...");
}

std::string fit_middle(const std::string &text, float width) {
	if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
	const float dots = ImGui::CalcTextSize("...").x;
	if (width <= dots) return fit(text, width);
	// The end takes three fifths of the room, the start the rest; each cut at a whole character.
	const float room = width - dots;
	size_t tail = text.size();
	while (tail > 0) {
		const size_t back = strutil::utf8_cut(text, tail - 1);
		if (ImGui::CalcTextSize(text.c_str() + back, text.c_str() + text.size()).x > room * 0.6f) break;
		tail = back;
	}
	const float tail_width = ImGui::CalcTextSize(text.c_str() + tail, text.c_str() + text.size()).x;
	const char *const begin = text.c_str();
	const char *stop = begin;
	ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), room - tail_width, 0.0f, begin, begin + tail, &stop);
	return std::string(begin, stop) + "..." + text.substr(tail);
}

bool fitted_button(const std::string &label, const char *id, float width) {
	const std::string shown = fit(label, width - ImGui::GetStyle().FramePadding.x * 2.0f);
	return ImGui::Button((shown + "###" + id).c_str());
}

void clipped_text(const std::string &text, const std::string &tip) {
	const std::string shown = fit(text, ImGui::GetContentRegionAvail().x);
	ImGui::TextUnformatted(shown.c_str());
	tooltip(!tip.empty() ? tip : shown != text ? text : std::string());
}

bool tooltip_hovered() { return ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled); }

TipsAbove::TipsAbove() : before_(g_tips_above) { g_tips_above = true; }

TipsAbove::~TipsAbove() { g_tips_above = before_; }

void tooltip(const std::string &text) {
	if (text.empty() || !tooltip_hovered()) return;
	const float wrap = ImGui::GetFontSize() * 40.0f;
	if (g_tips_above) {
		// Above the item, its bottom edge on the item's top edge (a hair clear of it) and its left on
		// the item's, held within the window's viewport; where the room above the item does not hold
		// it, Dear ImGui places it by the pointer as it places any tooltip.
		const ImGuiStyle &style = ImGui::GetStyle();
		const ImVec2 size = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap);
		const float width = size.x + style.WindowPadding.x * 2.0f, height = size.y + style.WindowPadding.y * 2.0f;
		const ImGuiViewport *viewport = ImGui::GetWindowViewport();
		const ImVec2 item = ImGui::GetItemRectMin();
		const float bottom = item.y - 2.0f;
		if (bottom - height >= viewport->Pos.y) {
			const float left = std::min(item.x, viewport->Pos.x + viewport->Size.x - width);
			ImGui::SetNextWindowPos(ImVec2(std::max(left, viewport->Pos.x), bottom), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
		}
	}
	// In place of a tooltip set before it this frame, as SetTooltip does: a table's header sets
	// its own for a label it cut, which BeginTooltip would add this one to.
	ImGui::BeginTooltipEx(ImGuiTooltipFlags_OverridePrevious, ImGuiWindowFlags_None);
	ImGui::PushTextWrapPos(wrap);
	ImGui::TextUnformatted(text.c_str());
	ImGui::PopTextWrapPos();
	ImGui::EndTooltip();
}

void empty_state(const char *text, const char *hint) {
	note(text);
	if (hint) note(hint);
}

void note(const std::string &text) {
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", text.c_str());
	ImGui::PopStyleColor();
}

}  // namespace opennova::devtools::ui_kit
