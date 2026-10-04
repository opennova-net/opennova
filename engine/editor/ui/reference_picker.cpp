#include "reference_picker.h"

#include <editor/graph/display_names.h>
#include <editor/graph/reference_queries.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/view/session_view.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/texture_preview.h>
#include <editor/ui/ui_kit.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <optional>
#include <set>

#include <base/io/strutil.h>

#include <imgui.h>
#include <imgui_internal.h>

namespace opennova::editor {
namespace {

// A choice's line (ADR 0046 S15): what it names, its words (an item's name, an entity's title, a
// register's NAME; S13 D8's label), then its name muted where the words are not it, then where it is
// defined, dimmed.
const std::string &words_of(const ReferenceChoice &choice) { return choice.label.empty() ? choice.name : choice.label; }
std::string where_of(const ReferenceChoice &choice) {
	if (choice.record.empty()) return choice.file;
	return choice.file + ": " + choice.record;
}

// What a choice's tooltip says: its words and its name, where, what it points at (an item's model, a
// string id's text: symbol_preview, made only while it shows), what the field would reference, why a
// lookup never finds it.
std::string choice_tip(const ReferenceChoice &choice, const AssetGraph *graph, const std::string &scope) {
	std::string tip = words_of(choice) + (choice.label.empty() ? std::string() : "\n" + choice.name) + "\n" +
	                  (choice.record.empty() ? "The file " : "Defined in ") + where_of(choice);
	// A choice named otherwise (a text key's number) previews the symbol it stands for.
	const bool other = !choice.symbol.empty();
	if (graph)
		if (const std::string preview = symbol_preview(*graph, choice.kind, other ? choice.symbol : choice.name,
		                                               other ? choice.symbol_scope : scope);
		    !preview.empty())
			tip += "\n" + preview;
	if (choice.status == ReferenceStatus::Missing) tip += "\nSet here, the game would not find it: Missing.";
	if (choice.inert) tip += "\nNo lookup of the game finds this definition: " + choice.reason + ".";
	return tip;
}

// Whether a typed text is a value the field takes as it is (the picker's "Use"): a whole number for a
// number field, any text for a text one.
bool typed_takes(const FieldUse &field, const std::string &typed) {
	if (typed.empty()) return false;
	if (field.schema->type == FieldType::Text) return true;
	const std::optional<int> number = strutil::parse_int(typed);
	return number && (!field.schema->ranged || (double(*number) >= field.schema->min && double(*number) <= field.schema->max));
}

} // namespace

ReferencePicker::ReferencePicker() = default;

ReferencePicker::~ReferencePicker() = default;

ReferencePicker::ListKey ReferencePicker::cache_key(const SessionView &view,
		const Document &document) {
	const RevisionKey reads = revision_key(view.revisions, {ViewConcern::Graph, ViewConcern::Files,
			ViewConcern::Project, ViewConcern::Preferences});
	return {reads, document.identity(), document.revision()};
}

void ReferencePicker::refresh(Popup &popup, const SessionView &view, const Document &document, const NodeAddress &record,
                              const FieldUse &field, const Value &value, bool others) {
	const ListKey key = cache_key(view, document);
	if (popup.view == &view && popup.key == key) return;
	popup.view = &view;
	popup.key = key;
	popup.field = field;
	++lists_made_;
	// Each name by what it names (ADR 0046 S15: an item id by its catalog's name, an SSN by its entity, the
	// player first), the name muted beside the words: picker_choices, as the wire's reference_choices.
	std::optional<GraphNameSource> names;
	if (view.findings.graph) names.emplace(*view.findings.graph);
	popup.choices = picker_choices(view.findings.graph.get(), document, record, field, names ? &*names : nullptr);
	if (!others)
		popup.choices.erase(std::remove_if(popup.choices.begin(), popup.choices.end(),
		                                   [&](const ReferenceChoice &choice) { return choice.kind != field.reference; }),
		                    popup.choices.end());
	popup.fixes.clear();
	// The finding the graph makes of this value, as Problems shows it, for its fixes (a %NAME% the
	// stylesheets do not define: the variable's).
	Diagnostic finding;
	popup.missing = view.findings.graph &&
			missing_finding(*view.findings.graph, document, record, field, value, finding);
	if (popup.missing) popup.fixes = fixes_for(finding, view);
}

void ReferencePicker::drop_list(Popup &popup) {
	popup.view = nullptr;
	popup.key = ListKey();
	popup.choices = std::vector<ReferenceChoice>();
	popup.missing = false;
	popup.fixes = std::vector<ProblemFix>();
}

void ReferencePicker::let_go(int frame) {
	for (size_t i = 0; i < held_.size();) {
		const auto it = popups_.find(held_[i]);
		if (it != popups_.end() && it->second.view && it->second.drawn + 1 >= frame) {
			++i;
			continue;
		}
		if (it != popups_.end()) drop_list(it->second);
		held_.erase(held_.begin() + std::ptrdiff_t(i));
	}
}

size_t ReferencePicker::lists_held() const {
	size_t held = 0;
	for (const auto &popup : popups_) held += popup.second.view ? 1 : 0;
	return held;
}

void ReferencePicker::prune(const SessionView &view) {
	const RevisionKey key = revision_key(view.revisions, {ViewConcern::DocumentSet});
	if (pruned_view_ == &view && pruned_key_ == key) return;
	pruned_view_ = &view;
	pruned_key_ = key;
	std::set<uint64_t> open;
	for (const auto &document : view.documents.open)
		if (document) open.insert(document->identity());
	for (auto it = popups_.begin(); it != popups_.end();)
		it = open.count(it->first.document) ? std::next(it) : popups_.erase(it);
}

bool ReferencePicker::draw(Workspace &workspace, const Document &document, const NodeAddress &record, const FieldUse &field,
                           const Value &value, bool compact, std::string &picked, bool others) {
	if (ImGui::SmallButton(compact ? "..." : "Pick")) ImGui::OpenPopup("references");
	ui_kit::tooltip(reference_row(field.reference).also_offers == ReferenceKind::StyleVar
	                  ? "Pick a file of the project or a variable of the stylesheet."
	                  : "Pick a name the project has.");
	// The popup's own id, the document and the record key its state: another record's field of
	// the same place in the window has its own.
	const Key key{document.identity(), record.row, record.kind, record.child, ImGui::GetID("references")};
	// A list whose popup is not drawn open goes as any popup of the picker draws: one closed, and
	// one whose picker the window no longer draws (another record's, of the same popup id or not).
	const int frame = ImGui::GetFrameCount();
	let_go(frame);
	if (!ImGui::BeginPopup("references")) {
		// Closed (or never opened): a list it held goes now, made again as it opens.
		const auto kept = popups_.find(key);
		if (kept != popups_.end() && kept->second.view) drop_list(kept->second);
		return false;
	}
	prune(workspace.view());
	Popup &popup = popups_[key];
	popup.drawn = frame;
	if (!popup.view) held_.push_back(key);
	refresh(popup, workspace.view(), document, record, field, value, others);
	const bool done = draw_popup(workspace, popup, picked, false);
	ImGui::EndPopup();
	return done;
}

bool ReferencePicker::draw_field(Workspace &workspace, const Document &document, const NodeAddress &record,
                                 const FieldUse &field, const Value &value, const DisplayName &words, bool mixed,
                                 std::string &picked) {
	// What the frame shows: the words (or the value where it has none), the value muted after them.
	const std::string raw = std::holds_alternative<std::string>(value)   ? std::get<std::string>(value)
	                        : std::holds_alternative<int64_t>(value) ? std::to_string(std::get<int64_t>(value))
	                                                                     : std::string();
	const std::string shown = mixed ? std::string("(mixed)") : words.text.empty() ? raw : words.text;
	const std::string muted = mixed || words.text.empty() || words.raw.empty() ? std::string() : words.raw;
	const Key key{document.identity(), record.row, record.kind, record.child, ImGui::GetID("##value")};
	const int frame = ImGui::GetFrameCount();
	let_go(frame);
	bool done = false;
	if (ImGui::BeginCombo("##value", "", int(ImGuiComboFlags_CustomPreview) | int(ImGuiComboFlags_HeightLarge))) {
		prune(workspace.view());
		Popup &popup = popups_[key];
		popup.drawn = frame;
		if (!popup.view) held_.push_back(key);
		// Opened afresh, nothing typed yet: a pick by name starts from every name (the last pick's
		// words are not kept as a filter).
		if (ImGui::IsWindowAppearing()) popup.filter[0] = '\0';
		refresh(popup, workspace.view(), document, record, field, value, true);
		done = draw_popup(workspace, popup, picked, true);
		ImGui::EndCombo();
	} else {
		const auto kept = popups_.find(key);
		if (kept != popups_.end() && kept->second.view) drop_list(kept->second);
	}
	if (ImGui::BeginComboPreview()) {
		const float room = ImGui::GetContentRegionAvail().x;
		const float tail = muted.empty() ? 0.0f : ui_kit::text_width(muted.c_str()) + ImGui::GetStyle().ItemSpacing.x;
		const std::string fitted = ui_kit::fit(shown, std::max(room - tail, room * 0.5f));
		if (words.dangling) ImGui::TextColored(ui_kit::reference_color(ReferenceStatus::Missing), "%s", fitted.c_str());
		else ImGui::TextUnformatted(fitted.c_str());
		if (!muted.empty()) {
			ImGui::SameLine();
			ImGui::TextDisabled("%s", ui_kit::fit(muted, ImGui::GetContentRegionAvail().x).c_str());
		}
		ImGui::EndComboPreview();
	}
	ui_kit::tooltip_lazy([&] {
		std::string tip = shown;
		if (!raw.empty() && raw != shown) tip += "\nThe file holds " + raw;
		if (!words.source.empty()) tip += "\nFrom " + words.source;
		return tip + "\nPick by name: type to find one.";
	});
	return done;
}

bool ReferencePicker::draw_popup(Workspace &workspace, Popup &popup, std::string &picked, bool typed_value) {
	const float width = ImGui::GetFontSize() * 26.0f;
	if (ImGui::IsWindowAppearing()) {
		popup.cursor = 0;
		ImGui::SetKeyboardFocusHere();
	}
	// Escape closes, and the filter typed stays: the text box, which has the keyboard, would put
	// back the text it had when it took it.
	const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
	char typed[sizeof(popup.filter)];
	std::memcpy(typed, popup.filter, sizeof(typed));
	if (ui_kit::filter_box("##find", popup.filter, sizeof(popup.filter), "Filter", width)) popup.cursor = 0;
	if (escape) {
		std::memcpy(popup.filter, typed, sizeof(typed));
		ImGui::CloseCurrentPopup();
	}
	size_t inert = 0;
	for (const ReferenceChoice &choice : popup.choices) inert += choice.inert ? 1 : 0;
	if (inert) {
		const std::string label = "Show unreachable (" + std::to_string(inert) + ")";
		if (ImGui::Checkbox(label.c_str(), &popup.unreachable)) popup.cursor = 0;
		ui_kit::tooltip("Names defined only where no lookup of the game finds them.");
	}
	// The rows shown: the filter's matches by name or label (a register by its index or its NAME),
	// the unreachable ones while shown.
	std::vector<const ReferenceChoice *> shown;
	for (const ReferenceChoice &choice : popup.choices)
		if ((!choice.inert || popup.unreachable) &&
		    (!popup.filter[0] || window_requests::matches(choice.name, popup.filter) ||
		     (!choice.label.empty() && window_requests::matches(choice.label, popup.filter))))
			shown.push_back(&choice);
	// The keys: the arrows move the highlighted row, Enter picks it, Escape closes.
	popup.moved = false;
	if (!shown.empty() && ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
		popup.cursor = std::min(popup.cursor + 1, shown.size() - 1);
		popup.moved = true;
	}
	if (!shown.empty() && ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
		popup.cursor = popup.cursor ? popup.cursor - 1 : 0;
		popup.moved = true;
	}
	popup.cursor = std::min(popup.cursor, shown.empty() ? size_t(0) : shown.size() - 1);
	const bool enter = !escape && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
	bool chosen = enter && !shown.empty();
	if (chosen) picked = shown[popup.cursor]->name;
	// A value typed that no name is, taken as typed where the field takes it (a field picked by name:
	// an SSN the mission has no entity of yet, an item id no catalog defines).
	if (typed_value && popup.field.schema && popup.filter[0]) {
		const std::string token = popup.filter;
		const bool known = std::any_of(popup.choices.begin(), popup.choices.end(),
		                               [&](const ReferenceChoice &choice) { return strutil::iequals(choice.name, token); });
		if (!known && typed_takes(popup.field, token)) {
			if (ImGui::Selectable(("Use \"" + token + "\"").c_str()) || (enter && shown.empty())) {
				picked = token;
				chosen = true;
			}
			ui_kit::tooltip("Written as typed: no name the project has is this value.");
		}
	}
	// A texture's names (ADR 0046 S18) each with its thumbnail, the file the reference set to it loads, two
	// lines high; the highlighted or hovered one larger in a panel beside the list.
	const bool textures = is_texture_reference(popup.field.reference);
	const float thumb = textures ? ImGui::GetTextLineHeight() * 2.0f : 0.0f;
	const float line = textures ? thumb + ImGui::GetStyle().ItemSpacing.y : ImGui::GetTextLineHeightWithSpacing();
	const size_t rows = std::clamp<size_t>(shown.size(), 3, textures ? 8 : 14);
	ImGui::BeginChild("names", ImVec2(width, line * float(rows) + ImGui::GetTextLineHeightWithSpacing() * 0.5f),
	                  ImGuiChildFlags_Borders);
	int hovered = -1;
	// Only the names that show draw (a project's thousands of textures), and the highlighted one
	// when the keys moved it, to scroll to.
	ImGuiListClipper clipper;
	clipper.Begin(static_cast<int>(shown.size()));
	if (popup.moved && popup.cursor < shown.size()) clipper.IncludeItemByIndex(static_cast<int>(popup.cursor));
	while (clipper.Step())
		for (int row = clipper.DisplayStart; row < clipper.DisplayEnd && !chosen; ++row) {
			const size_t i = size_t(row);
			const ReferenceChoice &choice = *shown[i];
			ImGui::PushID(row);
			const float x = ImGui::GetCursorPosX();
			if (ImGui::Selectable("##choice", i == popup.cursor, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, thumb))) {
				picked = choice.name;
				chosen = true;
			}
			if (ImGui::IsItemHovered()) hovered = row;
			if (i == popup.cursor && popup.moved) ImGui::SetScrollHereY(0.5f);
			ui_kit::tooltip_lazy([&] {
				return choice_tip(choice, popup.view ? popup.view->findings.graph.get() : nullptr, popup.field.scope);
			});
			ImGui::SameLine(0.0f, 0.0f);
			ImGui::SetCursorPosX(x);
			if (textures) {
				// Its thumbnail (none for a name its loader opens no file of), then its words centred on it.
				const float top = ImGui::GetCursorPosY();
				if (!choice.served.empty()) texture_preview::picture(workspace, choice.served, TextureLoadTransform::None, thumb);
				else ImGui::Dummy(ImVec2(thumb, thumb));
				ImGui::SameLine();
				ImGui::SetCursorPosY(top + (thumb - ImGui::GetTextLineHeight()) * 0.5f);
			}
			// The words, the name muted where it is not them, then where it is defined in what is left,
			// then what it would be when not found.
			const bool found = choice.status == ReferenceStatus::Present || choice.status == ReferenceStatus::Unverified;
			const char *word = found ? "" : ui_kit::reference_word(choice.status);
			const float room = ImGui::GetContentRegionAvail().x -
			                   (found ? 0.0f : ui_kit::text_width(word) + ImGui::GetStyle().ItemSpacing.x);
			const std::string name = ui_kit::fit(words_of(choice), room * 0.6f);
			if (choice.inert) ImGui::TextDisabled("%s", name.c_str());
			else ImGui::TextUnformatted(name.c_str());
			float used = ui_kit::text_width(name.c_str());
			if (!choice.label.empty()) {
				ImGui::SameLine();
				const float spacing = ImGui::GetStyle().ItemSpacing.x;
				const std::string muted = ui_kit::fit(choice.name, room - used - spacing);
				ImGui::TextDisabled("%s", muted.c_str());
				used += ui_kit::text_width(muted.c_str()) + spacing;
			}
			const std::string where = ui_kit::fit(where_of(choice), room - used - ImGui::GetStyle().ItemSpacing.x);
			if (!where.empty()) {
				ImGui::SameLine();
				ImGui::TextDisabled("%s", where.c_str());
			}
			if (!found) {
				ImGui::SameLine();
				ImGui::TextColored(ui_kit::reference_color(choice.status), "%s", word);
			}
			ImGui::PopID();
		}
	if (popup.choices.empty()) ui_kit::empty_state("The project has no names of this kind yet.");
	else if (shown.empty()) ui_kit::empty_state(popup.filter[0] ? "Nothing matches the filter." : "Every name is unreachable.");
	ImGui::EndChild();
	// The hovered texture's (else the highlighted one's) picture, larger, with what it is, as the
	// field's loader would load it.
	const size_t previewed = hovered >= 0 ? size_t(hovered) : popup.cursor;
	if (textures && previewed < shown.size() && popup.view && popup.view->findings.graph) {
		const ReferenceChoice &choice = *shown[previewed];
		ImGui::SameLine();
		ImGui::BeginGroup();
		const float side = ImGui::GetFontSize() * 12.0f;
		if (choice.served.empty()) {
			ImGui::Dummy(ImVec2(side, side));
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + side);
			ImGui::TextDisabled("The game loads no file of the project for %s.", choice.name.c_str());
			ImGui::PopTextWrapPos();
		} else {
			const TextureReferenceLoad load = texture_reference(*popup.view->findings.graph, popup.field.reference, choice.name,
			                                                    popup.field.scope, popup.field.loader_arg);
			texture_preview::picture(workspace, choice.served, load.transform, side);
			texture_preview::facts_block(workspace, choice.served, load.transform, side);
		}
		ImGui::EndGroup();
	}
	// The value's own fixes while it is missing, as Problems offers them.
	if (popup.missing) {
		ImGui::Separator();
		ImGui::TextColored(ui_kit::reference_color(ReferenceStatus::Missing), "%s",
		                   popup.fixes.empty() ? "The value is missing; nothing here makes it." : "The value is missing:");
		for (const ProblemFix &fix : popup.fixes) {
			ImGui::PushID(fix.label.c_str());
			if (ui_kit::fitted_button(fix.label, "fix", width)) {
				workspace.request(fix.request);
				ImGui::CloseCurrentPopup();
			}
			ui_kit::tooltip(fix.detail);
			ImGui::PopID();
		}
	}
	if (chosen) ImGui::CloseCurrentPopup();
	return chosen;
}

bool ReferencePicker::accept_file(const SessionView &view, const FieldUse &field, std::string &picked) {
	if (!ImGui::BeginDragDropTarget()) return false;
	bool dropped = false;
	// The file looked at before the drop is accepted: one that does not fit is never accepted.
	const ImGuiPayload *dragged = ImGui::GetDragDropPayload();
	if (dragged && dragged->IsDataType(kFileDragPayload) && dragged->Data) {
		const char *data = static_cast<const char *>(dragged->Data);
		const std::string path(data, strnlen(data, size_t(dragged->DataSize)));
		const AssetEntry *entry = nullptr;
		for (const AssetEntry &candidate : view.project.scan->entries)
			if (candidate.relative_path == path) entry = &candidate;
		if (entry && file_serves_reference(entry->kind, field.reference, field.loader_arg) &&
		    ImGui::AcceptDragDropPayload(kFileDragPayload)) {
			picked = entry->logical_name;
			dropped = true;
		}
	}
	ImGui::EndDragDropTarget();
	return dropped;
}

} // namespace opennova::editor
