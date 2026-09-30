#include <editor/ui/document_window.h>

#include <algorithm>
#include <map>

#include <editor/documents/def_catalog_document.h>
#include <editor/documents/mns_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/strings_document.h>
#include <editor/project/project_files.h>
#include <editor/session/session_view.h>
#include <editor/ui/document_outline.h>
#include <editor/ui/document_toolbar.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/welcome_view.h>

#include <imgui.h>

namespace opennova::editor {

void DocumentWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &view = workspace_.view();
	if (!view.project_open || view.documents.empty()) {
		// No tab bar: the next one follows the active document from its first frame.
		followed_.clear();
		raised_.clear();
		if (!view.project_open) draw_welcome(workspace_, form_);
		else ui_kit::empty_state("Double-click a file in Files to open it, or make one with New.");
		return;
	}
	draw_tabs(view);
}

void DocumentWindow::draw_tabs(const SessionView &view) {
	// The active document's tab is selected when the active document changes, and only then,
	// so a click is never fought. ImGui shows it from the next frame: on this one the tab
	// shown is still the one before, which says nothing of the user's choice.
	const bool follow = view.active_document != followed_;
	followed_ = view.active_document;
	// A file name two open documents share is told apart by the path.
	std::map<std::string, int> names;
	for (const auto &document : view.documents) ++names[basename_of(document->path())];
	if (!ImGui::BeginTabBar("documents", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll |
	                                          ImGuiTabBarFlags_TabListPopupButton))
		return;
	std::string shown; // the document whose tab shows
	for (const auto &document : view.documents) {
		const std::string &path = document->path();
		// A tab is known by its document's path. A close goes through the session: a file with
		// unsaved changes keeps its tab while the session asks (UnsavedDocument), a saved one
		// always closes.
		ImGuiTabItemFlags flags = ImGuiTabItemFlags_NoTooltip;
		if (document->dirty()) flags |= ImGuiTabItemFlags_UnsavedDocument;
		if (follow && path == view.active_document) flags |= ImGuiTabItemFlags_SetSelected;
		const std::string name = basename_of(path);
		const std::string label = (names[name] > 1 ? path : name) + "###" + path;
		bool open = true;
		const bool visible = ImGui::BeginTabItem(label.c_str(), &open, flags);
		ui_kit::tooltip(path);
		if (!open) workspace_.request(make_request(EditorRequestKind::CloseDocument, path));
		if (!visible) continue;
		shown = path;
		// Its view once it is the active document: the selection and the inspector are the
		// active document's (a tab a click just showed waits the frame its OpenDocument takes).
		// The find bar first: its Ctrl+F comes before the view's own filters'.
		if (path == view.active_document) {
			draw_find(*document);
			draw_view(*document);
		}
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
	if (shown == view.active_document) {
		raised_.clear();
	} else if (!follow && !shown.empty() && shown != raised_) {
		// A tab the user chose (a click, the tab list) shows another document: it becomes the
		// active one, once.
		raised_ = shown;
		workspace_.request(make_request(EditorRequestKind::OpenDocument, shown));
	}
}

void DocumentWindow::draw_modals() { menu_.draw_remove_prompt(workspace_); }

void DocumentWindow::open_find() {
	find_.open = true;
	find_.focus = true;
	request_focus();
}

void DocumentWindow::show_hit(const Document &document, size_t index) {
	const DocumentHit *hit = find_.cursor.show(index);
	if (!hit) return;
	find_.scroll = true;
	ReferenceTarget target;
	target.file = document.path();
	target.locator = hit->locator;
	target.field = hit->field;
	target.editable = true;
	window_requests::go_to(workspace_, target);
}

// The find bar over the active tab's view (open_find): the text, Aa (case), previous and next, the
// hit shown of how many, a close; the hits listed under it, each its record and field and the
// value as shown.
void DocumentWindow::draw_find(const Document &document) {
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F)) open_find();
	if (!find_.open) return;
	// Found again when the text, the option or the document moved.
	FindCursor &cursor = find_.cursor;
	cursor.refresh(document, find_.text, find_.match_case);
	const size_t count = cursor.hits().size();
	const bool on_hit = cursor.on_hit();
	const auto step = [&](bool forward) {
		const size_t next = cursor.step(forward);
		if (next != SIZE_MAX) show_hit(document, next);
	};
	ImGui::PushID("find");
	const ImGuiStyle &style = ImGui::GetStyle();
	const std::string of = std::to_string(cursor.current() + 1) + " of " + std::to_string(count);
	const std::string where = !find_.text[0] ? std::string()
	                          : !count       ? std::string("No match")
	                          : on_hit       ? of
	                                         : std::to_string(count) + (count == 1 ? " match" : " matches");
	const float tools = ui_kit::checkbox_width("Aa") + ui_kit::button_width("<") + ui_kit::button_width(">") +
	                    ui_kit::text_width("999 of 999") + ui_kit::button_width("x") + style.ItemSpacing.x * 5.0f;
	ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 8.0f, ImGui::GetContentRegionAvail().x - tools));
	if (find_.focus) {
		ImGui::SetKeyboardFocusHere();
		find_.focus = false;
	}
	const bool entered = ImGui::InputTextWithHint("##text", "Find in this file", find_.text, sizeof(find_.text),
	                                              ImGuiInputTextFlags_EnterReturnsTrue);
	const bool typing = ImGui::IsItemActive();
	if (entered) {
		step(!ImGui::GetIO().KeyShift);
		ImGui::SetKeyboardFocusHere(-1); // the keyboard stays in the text
	}
	const bool escape = (typing || ImGui::IsItemDeactivated()) && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	ui_kit::tooltip("Every field whose value, as the Inspector shows it, holds the text. Enter: the next; Shift+Enter: "
	                "the previous; Escape closes.");
	ImGui::SameLine();
	ImGui::Checkbox("Aa", &find_.match_case);
	ui_kit::tooltip(find_.match_case ? "Case matters. Untick to find any case." : "Any case. Tick to match the case.");
	ImGui::SameLine();
	ImGui::BeginDisabled(!count);
	if (ImGui::Button("<")) step(false);
	ui_kit::tooltip("The previous match (Shift+Enter).");
	ImGui::SameLine();
	if (ImGui::Button(">")) step(true);
	ui_kit::tooltip("The next match (Enter).");
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	ImGui::TextDisabled("%s", where.c_str());
	ImGui::SameLine();
	if (ImGui::Button("x") || escape) find_.open = false;
	ui_kit::tooltip("Close the find bar (Escape).");
	// The hits, a few lines of them at most: each its record, its field and the value as shown.
	if (find_.open && count) {
		const float line = ImGui::GetTextLineHeightWithSpacing();
		ImGui::BeginChild("hits", ImVec2(0.0f, line * float(std::min<size_t>(count, 5)) + style.WindowPadding.y),
		                  ImGuiChildFlags_Borders);
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(count));
		if (on_hit) clipper.IncludeItemByIndex(static_cast<int>(cursor.current()));
		while (clipper.Step())
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
				const DocumentHit &hit = cursor.hits()[size_t(i)];
				ImGui::PushID(i);
				const std::string title = document.record_title(hit.address);
				const std::string line_text = title + " - " + hit.label + ": " + hit.text.substr(0, hit.text.find('\n'));
				const bool shown = on_hit && size_t(i) == cursor.current();
				if (ImGui::Selectable((ui_kit::fit(line_text, ImGui::GetContentRegionAvail().x) + "###hit").c_str(), shown))
					show_hit(document, size_t(i));
				if (shown && find_.scroll) {
					ImGui::SetScrollHereY(0.5f);
					find_.scroll = false;
				}
				ui_kit::tooltip_lazy(
				        [&] { return hit.record + "\n" + hit.field + "\n" + hit.text; });
				ImGui::PopID();
			}
		ImGui::EndChild();
	}
	ImGui::PopID();
	ImGui::Separator();
}

// The view a document's tab shows, by its type: a catalog, a string table, a stylesheet and
// a menu have their own; a model, a clip and an animation table are their records as an
// outline (which adds, duplicates, removes and moves the rows a file adds).
void DocumentWindow::draw_view(const Document &document) {
	if (const auto *catalog = dynamic_cast<const DefCatalogDocument *>(&document)) {
		catalog_.draw(workspace_, *catalog);
	} else if (const auto *strings = dynamic_cast<const StringsDocument *>(&document)) {
		strings_.draw(workspace_, *strings);
	} else if (const auto *styles = dynamic_cast<const MnsDocument *>(&document)) {
		styles_.draw(workspace_, *styles);
	} else if (const auto *menu = dynamic_cast<const MnuDocument *>(&document)) {
		menu_.draw(workspace_, *menu);
	} else {
		draw_document_toolbar(workspace_, document);
		draw_document_outline(workspace_, document, outline_);
	}
}

} // namespace opennova::editor
