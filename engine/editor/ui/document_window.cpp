#include <editor/ui/document_window.h>

#include <algorithm>
#include <filesystem>
#include <map>

#include <editor/documents/def_catalog_document.h>
#include <editor/documents/mns_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/strings_document.h>
#include <editor/ui/document_outline.h>
#include <editor/ui/document_toolbar.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/welcome_view.h>

#include <imgui.h>

namespace opennova::editor {

void DocumentWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &view = host_.view();
	if (!view.project_open || view.documents.empty()) {
		// No tab bar: the next one follows the active document from its first frame.
		followed_.clear();
		raised_.clear();
		if (!view.project_open) draw_welcome(host_, form_);
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
	for (const auto &document : view.documents) ++names[std::filesystem::path(document->path()).filename().generic_string()];
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
		const std::string name = std::filesystem::path(path).filename().generic_string();
		const std::string label = (names[name] > 1 ? path : name) + "###" + path;
		bool open = true;
		const bool visible = ImGui::BeginTabItem(label.c_str(), &open, flags);
		ui_kit::tooltip(path);
		if (!open) host_.request(make_request(EditorRequestKind::CloseDocument, path));
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
		host_.request(make_request(EditorRequestKind::OpenDocument, shown));
	}
}

void DocumentWindow::draw_modals() { menu_.draw_remove_prompt(host_); }

void DocumentWindow::open_find() {
	find_.open = true;
	find_.focus = true;
	request_focus();
}

void DocumentWindow::show_hit(const Document &document, size_t index) {
	if (index >= find_.hits.size()) return;
	find_.current = index;
	find_.on_hit = true;
	find_.scroll = true;
	const DocumentHit &hit = find_.hits[index];
	find_.hit_address = hit.address;
	find_.hit_field = hit.field;
	ReferenceTarget target;
	target.file = document.path();
	target.locator = hit.locator;
	target.field = hit.field;
	target.editable = true;
	window_requests::go_to(host_, target);
}

// The find bar over the active tab's view (open_find): the text, Aa (case), previous and next, the
// hit shown of how many, a close; the hits listed under it, each its record and field and the
// value as shown.
void DocumentWindow::draw_find(const Document &document) {
	if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F)) open_find();
	if (!find_.open) return;
	// Found again when the text, the option or the document moved.
	if (find_.identity != document.identity() || find_.revision != document.revision() || find_.searched != find_.text ||
	    find_.searched_case != find_.match_case) {
		const bool same_search = find_.identity == document.identity() && find_.searched == find_.text &&
		                         find_.searched_case == find_.match_case;
		SearchOptions options;
		options.match_case = find_.match_case;
		std::vector<DocumentHit> hits = find_in_document(document, find_.text, options);
		// The hit shown found again by its record and field; when it no longer matches, the next
		// goes to the first hit after it that still does (none: the next the first).
		const auto index_of = [&hits](const NodeAddress &address, const std::string &field) {
			for (size_t i = 0; i < hits.size(); ++i)
				if (hits[i].address == address && hits[i].field == field) return i;
			return SIZE_MAX;
		};
		size_t current = SIZE_MAX;
		bool on_hit = false;
		if (same_search && find_.on_hit) {
			current = index_of(find_.hit_address, find_.hit_field);
			on_hit = current != SIZE_MAX;
			for (size_t i = find_.current + 1; !on_hit && current == SIZE_MAX && i < find_.hits.size(); ++i)
				current = index_of(find_.hits[i].address, find_.hits[i].field);
			if (!on_hit && current == SIZE_MAX) current = hits.size();
		} else if (same_search && find_.current < find_.hits.size()) {
			for (size_t i = find_.current; current == SIZE_MAX && i < find_.hits.size(); ++i)
				current = index_of(find_.hits[i].address, find_.hits[i].field);
		}
		find_.hits = std::move(hits);
		find_.current = current;
		find_.on_hit = on_hit;
		find_.identity = document.identity();
		find_.revision = document.revision();
		find_.searched = find_.text;
		find_.searched_case = find_.match_case;
	}
	const size_t count = find_.hits.size();
	const bool on_hit = find_.on_hit && find_.current < count;
	const auto step = [&](bool forward) {
		if (!count) return;
		const size_t next = on_hit                 ? (forward ? (find_.current + 1) % count : (find_.current + count - 1) % count)
		                    : find_.current < count ? (forward ? find_.current : (find_.current + count - 1) % count)
		                                            : (forward ? 0 : count - 1);
		show_hit(document, next);
	};
	ImGui::PushID("find");
	const ImGuiStyle &style = ImGui::GetStyle();
	const std::string where = !find_.text[0] ? std::string()
	                          : !count       ? std::string("No match")
	                          : on_hit       ? std::to_string(find_.current + 1) + " of " + std::to_string(count)
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
		if (on_hit) clipper.IncludeItemByIndex(static_cast<int>(find_.current));
		while (clipper.Step())
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
				const DocumentHit &hit = find_.hits[size_t(i)];
				ImGui::PushID(i);
				const std::string title = document.record_title(hit.address);
				const std::string line_text = title + " - " + hit.label + ": " + hit.text.substr(0, hit.text.find('\n'));
				const bool shown = on_hit && size_t(i) == find_.current;
				if (ImGui::Selectable((ui_kit::fit(line_text, ImGui::GetContentRegionAvail().x) + "###hit").c_str(), shown))
					show_hit(document, size_t(i));
				if (shown && find_.scroll) {
					ImGui::SetScrollHereY(0.5f);
					find_.scroll = false;
				}
				ui_kit::tooltip(hit.record + "\n" + hit.field + "\n" + hit.text);
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
		catalog_.draw(host_, *catalog);
	} else if (const auto *strings = dynamic_cast<const StringsDocument *>(&document)) {
		strings_.draw(host_, *strings);
	} else if (const auto *styles = dynamic_cast<const MnsDocument *>(&document)) {
		styles_.draw(host_, *styles);
	} else if (const auto *menu = dynamic_cast<const MnuDocument *>(&document)) {
		menu_.draw(host_, *menu);
	} else {
		draw_document_toolbar(host_, document);
		draw_document_outline(host_, document, outline_);
	}
}

} // namespace opennova::editor
