#include <editor/ui/ui_kit.h>

#include <algorithm>
#include <cstdio>

#include <imgui.h>
#include <imgui_internal.h>

#include <base/io/strutil.h>

namespace opennova::editor::ui_kit {

namespace {

// Whether a TipsAbove lives: Dear ImGui is single threaded, one frame is drawn at a time.
bool g_tips_above = false;

const char *severity_word(DiagnosticSeverity severity) {
	switch (severity) {
	case DiagnosticSeverity::Info: return "Info";
	case DiagnosticSeverity::Warning: return "Warning";
	case DiagnosticSeverity::Error: return "Error";
	}
	return "";
}

// What a label adds to a control's width when it is drawn on its right.
float label_width(const char *label) {
	const float text = ImGui::CalcTextSize(label, nullptr, true).x;
	return text > 0.0f ? ImGui::GetStyle().ItemInnerSpacing.x + text : 0.0f;
}

// A severity's mark centred at `centre` (on the item just drawn): an error a filled
// circle, a warning a triangle, a note a ring, in the severity's colour.
void draw_mark(DiagnosticSeverity severity, ImVec2 centre) {
	if (!ImGui::IsItemVisible()) return;
	ImDrawList *draw = ImGui::GetWindowDrawList();
	const ImU32 color = ImGui::GetColorU32(severity_color(severity));
	const float r = ImGui::GetFontSize() * 0.3f;
	switch (severity) {
	case DiagnosticSeverity::Error: draw->AddCircleFilled(centre, r, color); break;
	case DiagnosticSeverity::Warning:
		draw->AddTriangleFilled(ImVec2(centre.x, centre.y - r * 1.2f), ImVec2(centre.x + r * 1.2f, centre.y + r * 0.9f),
		                        ImVec2(centre.x - r * 1.2f, centre.y + r * 0.9f), color);
		break;
	case DiagnosticSeverity::Info: draw->AddCircle(centre, r, color, 0, 1.5f); break;
	}
}

// A mark's box: a line of text wide, `height` high (a line of text for 0), its centre.
ImVec2 mark_box(float height) {
	const float width = ImGui::GetTextLineHeight();
	const float high = height > 0.0f ? height : width;
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(width, high));
	return ImVec2(at.x + width * 0.5f, at.y + high * 0.5f);
}

} // namespace

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

bool HeldPopup::begin(const char *id, bool held, bool modal, int flags, bool closable, uint64_t opened) {
	dismissed_ = false;
	opened_ = opened;
	if (!held) closing_ = false;
	// A dialog opened again since its close was asked (a client's open served after it): it shows again.
	if (closing_ && opened != closed_at_) closing_ = false;
	if (held && !closing_ && !shown_ && !ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);
	bool open = true;
	const bool drawing = modal ? ImGui::BeginPopupModal(id, closable ? &open : nullptr, flags) : ImGui::BeginPopup(id, flags);
	if (!drawing) {
		// ImGui closed it while the session holds it open: the caller asks the session to close it.
		if (shown_ && held && !closing_) {
			closing_ = true;
			closed_at_ = opened;
			dismissed_ = true;
		}
		shown_ = false;
		return false;
	}
	if (!held || closing_) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		shown_ = false;
		return false;
	}
	shown_ = true;
	return true;
}

void HeldPopup::close() {
	closing_ = true;
	closed_at_ = opened_;
	ImGui::CloseCurrentPopup();
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

ImVec4 severity_color(DiagnosticSeverity severity) {
	switch (severity) {
	case DiagnosticSeverity::Info: return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
	case DiagnosticSeverity::Warning: return ImVec4(0.95f, 0.80f, 0.40f, 1.0f);
	case DiagnosticSeverity::Error: return ImVec4(0.95f, 0.55f, 0.45f, 1.0f);
	}
	return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

void severity_marker(DiagnosticSeverity severity) {
	const float size = ImGui::GetFrameHeight();
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(size, size));
	tooltip(severity_word(severity));
	draw_mark(severity, ImVec2(at.x + size * 0.5f, at.y + size * 0.5f));
}

bool severity_mark_on_item(DiagnosticSeverity severity) {
	if (!ImGui::IsItemVisible()) return false;
	const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
	const float size = max.y - min.y;
	// The item's right end, or the window's where the item runs past what it shows.
	const float right = std::min(max.x, ImGui::GetCurrentWindow()->InnerClipRect.Max.x);
	const ImVec2 centre(right - size * 0.5f, (min.y + max.y) * 0.5f);
	draw_mark(severity, centre);
	return ImGui::IsMouseHoveringRect(ImVec2(centre.x - size * 0.5f, min.y), ImVec2(centre.x + size * 0.5f, max.y));
}

void severity_count(DiagnosticSeverity severity, size_t count, float height) {
	ImGui::BeginGroup();
	const ImVec2 centre = mark_box(height);
	draw_mark(severity, centre);
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
	ImGui::TextColored(severity_color(severity), "%zu", count);
	ImGui::EndGroup();
}

float severity_count_width(size_t count) {
	return ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemInnerSpacing.x + text_width(std::to_string(count).c_str());
}

void unsaved_dot(float height) {
	const ImVec2 centre = mark_box(height);
	if (!ImGui::IsItemVisible()) return;
	ImGui::GetWindowDrawList()->AddCircleFilled(centre, ImGui::GetTextLineHeight() * 0.22f, ImGui::GetColorU32(ImGuiCol_Text));
}

float unsaved_dot_width() { return ImGui::GetTextLineHeight(); }

const char *const kChangeRoom = "  ";

void change_dot(Document::RecordChange change, float x) {
	if (change == Document::RecordChange::Unchanged || !ImGui::IsItemVisible()) return;
	const float room = ImGui::CalcTextSize(kChangeRoom).x;
	const float y = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
	ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(x + room * 0.45f, y), ImGui::GetFontSize() * 0.2f,
	                                            ImGui::GetColorU32(change_color(change)));
}

ImVec4 change_color(Document::RecordChange change) {
	switch (change) {
	case Document::RecordChange::Added: return ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
	case Document::RecordChange::Changed: return ImVec4(0.95f, 0.70f, 0.30f, 1.0f);
	case Document::RecordChange::Unchanged: break;
	}
	return ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

const char *change_words(Document::RecordChange change) {
	switch (change) {
	case Document::RecordChange::Added: return "Added since the last save.";
	case Document::RecordChange::Changed: return "Changed since the last save.";
	case Document::RecordChange::Unchanged: break;
	}
	return "";
}

const char *reference_word(ReferenceStatus status) {
	switch (status) {
	case ReferenceStatus::Present: return "Present";
	case ReferenceStatus::Missing: return "Missing";
	case ReferenceStatus::Unverified: return "Unverified";
	case ReferenceStatus::NotAReference: break;
	}
	return "";
}

ImVec4 reference_color(ReferenceStatus status) {
	switch (status) {
	case ReferenceStatus::Present: return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
	case ReferenceStatus::Missing: return ImVec4(0.95f, 0.60f, 0.45f, 1.0f);
	case ReferenceStatus::Unverified:
	case ReferenceStatus::NotAReference: break;
	}
	return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
}

void empty_state(const char *text, const char *hint) {
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", text);
	if (hint) ImGui::TextWrapped("%s", hint);
	ImGui::PopStyleColor();
}

Cover cover_of(float left, float top, float right, float bottom) {
	return Cover{ ImGui::GetCurrentWindowRead()->RootWindow->ID, left, top, right, bottom };
}

bool covered(const Cover &cover) {
	if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return true;
	const ImGuiContext &g = *GImGui;
	const ImGuiWindow *own = ImGui::FindWindowByID(cover.window);
	if (!own) return false;
	const ImRect rect(cover.left, cover.top, cover.right, cover.bottom);
	// The windows are in display order, back to front: those after the rect's window's root are over
	// it, drawn this frame (begun already, or drawn the last frame and begun after this one). A tooltip
	// is left aside, and so are Dear ImGui's implicit window (begun every frame, drawn only where
	// something is written to it, which nothing is) and the dock space's own windows: its host stays
	// behind everything, and the windows docked in it tile its room, never over one another.
	bool above = false;
	for (const ImGuiWindow *window : g.Windows) {
		if (window == own) {
			above = true;
			continue;
		}
		if (!above || window->IsFallbackWindow || !(window->Active || window->WasActive) || window->Hidden ||
				window->RootWindow == own)
			continue;
		if (window->Flags & (ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_DockNodeHost | ImGuiWindowFlags_NoBringToFrontOnFocus))
			continue;
		if (window->DockIsActive && window->RootWindowDockTree == own->RootWindowDockTree) continue;
		if (window->Viewport != own->Viewport) continue;
		if (window->Rect().Overlaps(rect)) return true;
	}
	return false;
}

} // namespace opennova::editor::ui_kit
