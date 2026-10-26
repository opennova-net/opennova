#include <editor/ui/ui_kit.h>

#include <algorithm>

#include <imgui.h>
#include <imgui_internal.h>

namespace opennova::editor::ui_kit {

namespace {

const char *severity_word(DiagnosticSeverity severity) {
	switch (severity) {
	case DiagnosticSeverity::Info: return "Info";
	case DiagnosticSeverity::Warning: return "Warning";
	case DiagnosticSeverity::Error: return "Error";
	}
	return "";
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

bool HeldPopup::begin(const char *id, bool held, bool modal, int flags, bool closable, uint64_t opened) {
	dismissed_ = false;
	opened_ = opened;
	if (!held) closing_ = false;
	// A dialog opened again since its close was asked (a client's open served after it): it shows again.
	if (closing_ && opened != closed_at_) closing_ = false;
	// Opened only from a window that drew the frame before as well: one that comes back (reopened, its tab
	// selected again) is focused by its dock node a frame after it first draws, and a focus closes every popup
	// over the window, which would take the dialog away as it showed.
	const int frame = ImGui::GetFrameCount();
	const bool drew_before = drawn_frame_ == frame - 1 || drawn_frame_ == frame;
	drawn_frame_ = frame;
	if (held && !closing_ && !shown_ && !ImGui::IsPopupOpen(id) && drew_before) ImGui::OpenPopup(id);
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

ImVec4 severity_color(DiagnosticSeverity severity) {
	switch (severity) {
	case DiagnosticSeverity::Info: return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
	case DiagnosticSeverity::Warning: return ImVec4(0.95f, 0.80f, 0.40f, 1.0f);
	case DiagnosticSeverity::Error: return ImVec4(0.95f, 0.55f, 0.45f, 1.0f);
	}
	return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
}

ImVec4 warning_color() { return severity_color(DiagnosticSeverity::Warning); }

ImVec4 refusal_color() { return severity_color(DiagnosticSeverity::Error); }

void warning_line(const std::string &text, const std::string &tip) {
	ImGui::PushStyleColor(ImGuiCol_Text, warning_color());
	clipped_text(text, tip);
	ImGui::PopStyleColor();
}

void refusal_text(const std::string &text) {
	ImGui::PushStyleColor(ImGuiCol_Text, refusal_color());
	clipped_text(text);
	ImGui::PopStyleColor();
}

void dialog_text(const std::string &text) {
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 36.0f);
	ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
	ImGui::PopTextWrapPos();
}

const char *reading_references() { return "Reading the project's references..."; }

const char *nothing_names_it(bool file) {
	return file ? "No file of the project names it or what it defines." : "No file of the project names it.";
}

const char *undo_brings_it_back(bool file) {
	return file ? "Edit > Undo file brings it back." : "Undo brings it back.";
}

bool no_uses(size_t count, bool reading, const char *none, const char *how) {
	if (reading) empty_state(reading_references(), count ? "The list may grow." : nullptr);
	else if (!count) empty_state(none, how);
	return count == 0;
}

bool uses_heading(const std::string &title, size_t count, bool reading, const std::string &tip, const char *none,
                  const char *how) {
	const std::string heading = title + " (" + std::to_string(count) + ")";
	ImGui::SeparatorText(fit(heading, ImGui::GetContentRegionAvail().x - ImGui::GetStyle().SeparatorTextPadding.x * 2.0f).c_str());
	tooltip(tip);
	return !no_uses(count, reading, none, how);
}

bool named_action(const std::string &verb, const std::string &name, const char *tail, bool enabled,
                  const std::string &tip, bool small) {
	const ImGuiStyle &style = ImGui::GetStyle();
	const std::string before = verb + " ", after = tail ? tail : "";
	const float room = ImGui::GetContentRegionAvail().x - style.FramePadding.x * 2.0f -
	                   text_width(before.c_str()) - text_width(after.c_str());
	const std::string label = before + fit_middle(name, std::max(0.0f, room)) + after + "###" + verb;
	ImGui::BeginDisabled(!enabled);
	const bool pressed = small ? ImGui::SmallButton(label.c_str()) : ImGui::Button(label.c_str());
	// Held back by an enclosing BeginDisabled too (the busy gate), as a tool is.
	const bool held = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
	ImGui::EndDisabled();
	tooltip(tip);
	return pressed && enabled && !held;
}

void severity_marker(DiagnosticSeverity severity) {
	const float size = ImGui::GetFrameHeight();
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(size, size));
	tooltip(severity_word(severity));
	draw_mark(severity, ImVec2(at.x + size * 0.5f, at.y + size * 0.5f));
}

void finding_line(DiagnosticSeverity severity, const std::string &words, const std::string &tip) {
	severity_marker(severity);
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	clipped_text(without_citations(words), tip);
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
