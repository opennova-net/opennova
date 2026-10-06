#include <editor/ui/files_window.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>

#include <base/gameprofile/required_resources.h>
#include <editor/assets/asset_kinds.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/import_run.h>
#include <editor/import/terrain_import.h>
#include <editor/model/field_text.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/project/project_files.h>
#include <editor/session/file_card.h>
#include <editor/session/problem_query.h>
#include <editor/session/request_factories.h>
#include <editor/session/texture_budget_list.h>
#include <editor/session/view/session_view.h>
#include <editor/session/workspace_parts.h>
#include <editor/ui/editor_requests.h>
#include <editor/ui/inspector_layout.h>
#include <editor/ui/reference_picker.h>
#include <editor/ui/texture_preview.h>
#include <editor/ui/ui_kit.h>
#include <editor/ui/welcome_view.h>

#include <imgui.h>

namespace opennova::editor {

namespace fs = std::filesystem;

namespace {

// An open document's name in Files.
const ImVec4 kOpenColor(0.55f, 0.78f, 1.0f, 1.0f);
const ImVec4 kRefusalColor(0.95f, 0.55f, 0.45f, 1.0f);

using ui_kit::size_text;

// What a blank factory makes, as its menu entry's tooltip.
std::string makes(const BlankFactory &factory) { return std::string("Makes ") + factory.summary + "."; }

const AssetEntry *entry_at(const SessionView &view, const std::string &path) {
	for (const AssetEntry &entry : view.project.scan->entries)
		if (entry.relative_path == path) return &entry;
	return nullptr;
}

const DocumentBase *open_document(const SessionView &view, const std::string &path) {
	for (const auto &document : view.documents.open)
		if (document->path() == path) return document.get();
	return nullptr;
}

// What the tree and each file's counts read of the view: the files and the findings (and which
// documents are open).
RevisionKey tree_key(const SessionView &view) {
	return revision_key(view.revisions, {ViewConcern::Files, ViewConcern::Findings, ViewConcern::DocumentSet});
}

} // namespace

void NewFilePrompt::ask(Workspace &workspace, AssetKind kind) {
	window_requests::set_workspace(workspace, "new_file", "kind", io::JsonValue::make_string(asset_kind_token(kind)));
}

namespace {

// The prompt's values as the session takes them: each given value by its param's token.
io::JsonValue values_json(const BlankParam *params, size_t count, const std::vector<std::string> &values) {
	io::JsonValue out = io::JsonValue::make_object();
	for (size_t i = 0; i < count && i < values.size(); ++i)
		if (!values[i].empty()) out.set(params[i].token, io::JsonValue::make_string(values[i]));
	return out;
}

} // namespace

void NewFilePrompt::draw(Workspace &workspace) {
	const SessionView &v = workspace.view();
	const WorkspaceView::NewFile &held = v.workspace.new_file;
	const AssetKind kind = held.kind;
	// A terrain is made from images (S20, the new_terrain request): its name the terrain's, its values the
	// images and the importer's numbers; any other kind is its blank's.
	const bool terrain = kind == AssetKind::Terrain;
	size_t params = 0;
	const BlankParam *taken = kind == AssetKind::kCount ? nullptr : new_file_params(kind, params);
	// The fields take the session's values when they moved (the prompt opened on another kind, a value set
	// over the wire).
	name_.follow(held.name);
	if (kind != kind_seen_ || held.values != values_seen_ || values_.size() != params) {
		kind_seen_ = kind;
		values_seen_ = held.values;
		values_.assign(params, std::string());
		for (size_t i = 0; i < params; ++i)
			for (const auto &[token, value] : held.values)
				if (token == taken[i].token) values_[i] = value;
	}
	// Held open, it shows when no dialog before it in the session's order is held (shown_modal).
	const bool open = kind != AssetKind::kCount && v.project.open && modal_may_show(v, HeldModal::NewFile);
	if (!popup_.begin("New file", open, true, ImGuiWindowFlags_AlwaysAutoResize, false, v.workspace.opened)) {
		if (popup_.dismissed()) window_requests::set_workspace(workspace, "new_file", "kind", io::JsonValue::make_string(""));
		return;
	}
	if (terrain) ImGui::TextUnformatted("New terrain from images");
	else ImGui::Text("New file: %s", asset_kind_label(kind));
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 21.0f);
	// The name a new file of the kind is offered: its row's (AssetKindRow::new_name); a terrain's, its stem.
	const bool enter = ImGui::InputTextWithHint("Name", terrain ? "island" : asset_kind_row(kind).new_name, name_.text,
	                                            sizeof(name_.text), ImGuiInputTextFlags_EnterReturnsTrue);
	if (name_.sent() != held.name && ImGui::IsItemEdited())
		window_requests::set_workspace(workspace, "new_file", "name", io::JsonValue::make_string(name_.sent()));
	const char *name = name_.text;
	// The project's name rules, as the session checks them again when it creates the file; a
	// texture is the texture factory's placeholder, made only for a name it takes.
	FileNameProblem problem = FileNameProblem::None;
	std::string message;
	const bool named = name[0] != '\0';
	bool fits = named && (terrain ? terrain_stem_fits(name, message) : check_file_name(name, kind, problem, message));
	if (fits && kind == AssetKind::Texture) fits = can_make_blank_texture(name, message);
	// A terrain's name is taken where the project has its settings already (the request checks every file
	// it makes).
	const std::string file = terrain ? std::string(name) + ".trn" : std::string(name);
	const bool in_use = named && v.project.scan->find(file) != nullptr;
	if (named && !fits) ImGui::TextColored(kRefusalColor, "%s", message.c_str());
	else if (in_use) ImGui::TextColored(kRefusalColor, "The project has a file named %s already.", file.c_str());
	if (terrain)
		ImGui::TextWrapped("Each image is a file on disk (copied into art/terrain/) or a file of the project; the "
		                   "terrain is imported from them, and imported again when one changes.");
	// What the kind's blank takes beside its name (a mission's title, terrain and environment): a
	// text, or one of the project's files its reference loads. A project is its own files, so a
	// kind the project has no file of says to import one.
	bool given = true, changed = false;
	for (size_t i = 0; i < params; ++i) {
		const BlankParam &param = taken[i];
		ImGui::PushID(static_cast<int>(i));
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 21.0f);
		if (param.reference == ReferenceKind::None) {
			char text[kWorkspaceFileName];
			std::snprintf(text, sizeof(text), "%s", values_[i].c_str());
			if (ImGui::InputText(param.label, text, sizeof(text))) {
				values_[i] = text;
				changed = true;
			}
		} else {
			const AssetKind wanted = reference_row(param.reference).file;
			size_t offered = 0;
			if (ImGui::BeginCombo(param.label, values_[i].empty() ? "Choose..." : values_[i].c_str())) {
				for (const AssetEntry &entry : v.project.scan->entries) {
					if (entry.kind != wanted) continue;
					++offered;
					if (ImGui::Selectable(entry.logical_name.c_str(), entry.logical_name == values_[i])) {
						values_[i] = entry.logical_name;
						changed = true;
					}
				}
				ImGui::EndCombo();
			} else {
				for (const AssetEntry &entry : v.project.scan->entries) offered += entry.kind == wanted ? 1 : 0;
			}
			if (offered == 0)
				ImGui::TextColored(kRefusalColor, "The project has no %s: import one first (Files > Import).", param.token);
		}
		ImGui::PopID();
		given = given && (!param.required || !values_[i].empty());
	}
	if (changed) window_requests::set_workspace(workspace, "new_file", "values", values_json(taken, params, values_));
	const bool ready =
	        fits && !in_use && given && v.allows(terrain ? EditorRequestKind::NewTerrain : EditorRequestKind::CreateFile);
	ImGui::BeginDisabled(!ready);
	const bool create = ImGui::Button("Create");
	ImGui::EndDisabled();
	if ((create || enter) && ready) {
		std::vector<std::pair<std::string, std::string>> values;
		for (size_t i = 0; i < params; ++i)
			if (!values_[i].empty()) values.emplace_back(taken[i].token, values_[i]);
		// The session closes the prompt as it takes the file it names (create_file alone, over the wire); the
		// prompt's Create closes it too, as Cancel does.
		if (terrain) workspace.request(request::new_terrain(name, std::move(values)));
		else workspace.request(request::create_file(name, asset_kind_token(kind), std::move(values)));
		window_requests::set_workspace(workspace, "new_file", "kind", io::JsonValue::make_string(""));
		popup_.close();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) {
		window_requests::set_workspace(workspace, "new_file", "kind", io::JsonValue::make_string(""));
		popup_.close();
	}
	ImGui::EndPopup();
}

// The tree of the scan's folders and each file's findings, made again when what they read moves.
void FilesWindow::refresh(const SessionView &view) {
	const RevisionKey key = tree_key(view);
	if (view_ == &view && key_ == key && !folders_.empty()) return;
	view_ = &view;
	key_ = key;
	++rebuilds_;
	folders_.assign(1, Folder());
	matches_made_ = false; // made again by matching(), once a filter is set
	std::map<std::string, size_t> index;
	for (size_t i = 0; i < view.project.scan->entries.size(); ++i) {
		const AssetEntry &entry = view.project.scan->entries[i];
		// An imported file sits in the cache: it shows beside its source.
		const std::string &place = entry.imported_from.empty() ? entry.relative_path : entry.imported_from;
		size_t at = 0;
		std::string path;
		for (const fs::path &part : path_of(place).parent_path()) {
			path += (path.empty() ? "" : "/") + utf8_of(part);
			auto found = index.find(path);
			if (found == index.end()) {
				folders_.push_back(Folder{utf8_of(part), path, {}, {}});
				found = index.emplace(path, folders_.size() - 1).first;
				folders_[at].folders.push_back(found->second);
			}
			at = found->second;
		}
		folders_[at].files.push_back(i);
	}
	for (Folder &folder : folders_)
		std::sort(folder.folders.begin(), folder.folders.end(), [this](size_t a, size_t b) {
			return normalized_logical_name(folders_[a].name) < normalized_logical_name(folders_[b].name);
		});
	counts_.clear();
	FindingMarks scratch;
	const FindingMarks &marks = finding_marks(view, scratch);
	for (size_t i = 0; i < view.findings.diagnostics.size(); ++i) {
		const Diagnostic &d = view.findings.diagnostics[i];
		if (d.asset.empty()) continue;
		Counts &counts = counts_[d.asset];
		// The game's own data's apart, as Problems counts them (S15, per finding).
		const bool original = marks.original[i] != 0;
		if (d.severity == DiagnosticSeverity::Error) ++(original ? counts.original_errors : counts.errors);
		else if (d.severity == DiagnosticSeverity::Warning) ++(original ? counts.original_warnings : counts.warnings);
	}
	if (!entry_at(view, selected_)) selected_.clear();
}

void FilesWindow::follow_filter(const SessionView &view) {
	filter_.follow(view.workspace.files.filter);
	if (kind_held_.follow(view.workspace.files.kind)) kind_shown_ = view.workspace.files.kind;
	if (by_cost_held_.follow(view.workspace.files.by_cost)) by_cost_ = view.workspace.files.by_cost;
}

void FilesWindow::send_filter(bool filter, bool kind, bool by_cost) {
	io::JsonValue members = io::JsonValue::make_object();
	if (by_cost) members.set("by_cost", io::JsonValue::make_bool(by_cost_));
	if (filter) members.set("filter", io::JsonValue::make_string(filter_.sent()));
	if (kind) members.set("kind", io::JsonValue::make_string(kind_shown_ == AssetKind::kCount ? "" : asset_kind_token(kind_shown_)));
	window_requests::set_workspace(workspace_, "files", std::move(members));
}

const std::vector<size_t> &FilesWindow::matching(const SessionView &view) {
	const char *filter = filter_.text;
	if (filter[0] == '\0' && kind_shown_ == AssetKind::kCount) {
		matches_.clear();
		matches_made_ = false;
		return matches_;
	}
	const uint64_t generation = view.findings.graph ? view.findings.graph->generation() : 0;
	const std::string asked =
			std::string(filter) + '\n' + std::to_string(static_cast<int>(kind_shown_)) + (by_cost_ ? "\ncost" : "");
	if (matches_made_ && matched_ == asked && matched_generation_ == generation) return matches_;
	matches_made_ = true;
	matched_ = asked;
	matched_generation_ = generation;
	via_.clear();
	// The paths holding the text, then the files of a kind it names ("texture" lists the textures), each of
	// the kind chosen (match_files, the files query's own rule).
	matches_ = match_files(*view.project.scan, filter, kind_shown_);
	std::vector<bool> listed(view.project.scan->entries.size(), false);
	for (const size_t i : matches_) listed[i] = true;
	// Then the files a record naming them is found by, after them: a model by the item whose graphic it
	// is (lack finds Dblkhwk1.3di through Flyable Blackhawk), from three letters on.
	// Each once: a file its folder's name already lists is not listed again (the graph's search finds by
	// record from three letters on).
	if (view.findings.graph && filter[0] != '\0') {
		std::unordered_map<std::string, size_t> at;
		for (const GraphSearchHit &hit : view.findings.graph->search(filter)) {
			if (hit.symbol || hit.via.empty()) continue;
			if (at.empty())
				for (size_t i = 0; i < view.project.scan->entries.size(); ++i) at.emplace(view.project.scan->entries[i].relative_path, i);
			const auto found = at.find(hit.file);
			if (found == at.end() || found->second >= listed.size() || listed[found->second]) continue;
			if (kind_shown_ != AssetKind::kCount && view.project.scan->entries[found->second].kind != kind_shown_) continue;
			listed[found->second] = true;
			matches_.push_back(found->second);
			via_[hit.file] = hit.via;
		}
	}
	// By cost: the costliest first, what the game makes no model texture of after them in their order.
	costs_.clear();
	matched_cost_ = 0;
	if (by_cost_) {
		for (const TextureBudgetRow &row : texture_budget_list(view).rows) costs_[row.file] += row.budget.full().bytes;
		const auto cost = [&](size_t i) {
			const auto found = costs_.find(view.project.scan->entries[i].relative_path);
			return found == costs_.end() ? uint64_t(0) : found->second;
		};
		std::stable_sort(matches_.begin(), matches_.end(), [&](size_t a, size_t b) { return cost(a) > cost(b); });
		for (const size_t i : matches_) matched_cost_ += cost(i);
	}
	return matches_;
}

bool FilesWindow::stands_aside() const { return aside_for_welcome(workspace_.view(), welcome_asked_); }

void FilesWindow::receive(const ViewEvent &event) {
	events_.post(event);
	request_focus();
}

// The file an ask names, selected; a filter that hides it cleared; the folders on its way
// (an imported file's are its source's) to open and the row to scroll to as they draw.
void FilesWindow::show_revealed(const SessionView &view, const ViewEvent &event) {
	const AssetEntry *entry = entry_at(view, event.path);
	if (!entry) return;
	selected_ = entry->relative_path;
	// A filter or a kind that hides it cleared (the workspace's: the session's filter cleared too).
	const std::vector<size_t> &shown = matching(view);
	const bool hidden = (filter_.text[0] != '\0' || kind_shown_ != AssetKind::kCount) &&
	                    std::none_of(shown.begin(), shown.end(),
	                                 [&](size_t i) { return view.project.scan->entries[i].relative_path == entry->relative_path; });
	if (hidden) {
		filter_.text[0] = '\0';
		kind_shown_ = AssetKind::kCount;
		send_filter(true, true);
	}
	scroll_to_ = entry->relative_path;
	open_to_ = entry->imported_from.empty() ? entry->relative_path : entry->imported_from;
}

void FilesWindow::draw(devtools::ImGuiPass &, uint64_t) {
	const SessionView &v = workspace_.view();
	// The RevealFile events held since Files last drew, taken now: the newest is shown (its file
	// selected, Rename... opened on it when it asks), as the view's one reveal was, each ask
	// overwriting the one before; an older ask is passed over, its Rename... too. With no project
	// open none shows.
	const std::vector<ViewEvent> reveals = events_.take();
	if (!v.project.open) {
		ui_kit::empty_state("No project open.", "Make one or open one in the Document window.");
		return;
	}
	// Another project: its own selection (none yet); the session starts its filter and kind afresh.
	if (v.project.root != shown_root_) {
		shown_root_ = v.project.root;
		selected_.clear();
	}
	follow_filter(v);
	refresh(v);
	const auto newest = std::find_if(reveals.rbegin(), reveals.rend(),
			[](const ViewEvent &event) { return event.kind == ViewEventKind::RevealFile; });
	if (newest != reveals.rend()) show_revealed(v, *newest);
	// A file the session selected since Files last drew (a select_file over the wire, or this
	// window's own click coming back) is the row selected, its folders opened and scrolled to.
	if (v.documents.file_selected.path != followed_) {
		followed_ = v.documents.file_selected.path;
		if (const AssetEntry *entry = followed_.empty() ? nullptr : entry_at(v, followed_)) {
			selected_ = entry->relative_path;
			scroll_to_ = entry->relative_path;
			open_to_ = entry->imported_from.empty() ? entry->relative_path : entry->imported_from;
		}
	}
	draw_toolbar(v);
	// The filter and the kind beside it, then how many files the list shows (under them in a narrow dock).
	const std::vector<size_t> &matches = matching(v);
	const bool narrowed = filter_.text[0] != '\0' || kind_shown_ != AssetKind::kCount;
	const size_t count = narrowed ? matches.size() : v.project.scan->entries.size();
	const std::string files = (narrowed ? grouped(count) + " of " + grouped(v.project.scan->entries.size()) : grouped(count)) +
	                          (v.project.scan->entries.size() == 1 ? " file" : " files") +
	                          (narrowed && by_cost_ ? ", " + texture_bytes_words(matched_cost_) + " in the game" : "");
	// By cost, offered while the list shows textures (and while it is on, to turn it off).
	const bool offers_cost = kind_shown_ == AssetKind::Texture || by_cost_;
	{
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const float kind = ImGui::GetFontSize() * 8.0f;
		const float cost = offers_cost ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ui_kit::text_width("By cost") : 0.0f;
		const float field = std::max(ImGui::GetFontSize() * 8.0f, ImGui::GetContentRegionAvail().x - kind - cost -
		                                                                   ui_kit::text_width(files.c_str()) -
		                                                                   spacing * (offers_cost ? 3.0f : 2.0f));
		ui_kit::WrapRow row;
		row.next(field);
		if (ui_kit::filter_box("##filter", filter_.text, sizeof(filter_.text), "Filter files", field,
		                       "A name, a folder, or a kind (texture, wave, model...): the files of the kind follow those "
		                       "whose names hold the text."))
			send_filter(true, false);
		row.next(kind);
		draw_kind_filter(v, kind);
		if (offers_cost) {
			row.next(cost);
			if (ImGui::Checkbox("By cost", &by_cost_)) send_filter(false, false, true);
			ui_kit::tooltip("The files by what the game's textures of them cost at full detail, the costliest first: "
			                "what the game keeps in its memory for each, every level of its chain.");
		}
		row.next(ui_kit::text_width(files.c_str()));
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", files.c_str());
	}
	// F2 renames the selected file, as its menu's Rename... does (while the busy gate takes a
	// rename).
	if (!selected_.empty() && v.allows(EditorRequestKind::RenameAsset) && ImGui::Shortcut(ImGuiKey_F2))
		if (const AssetEntry *entry = entry_at(v, selected_)) start_rename(*entry);
	// A filter or a kind lists the files it matches flat.
	if (v.project.scan->entries.empty()) {
		ui_kit::empty_state("The project has no files yet.", "Import files, or make one with New.");
	} else if (narrowed && matches.empty()) {
		ui_kit::empty_state(kind_shown_ != AssetKind::kCount && filter_.text[0] == '\0' ? "The project has no file of this kind."
		                                                                              : "No file matches the filter.");
	} else if (const bool kind_fits = ImGui::GetContentRegionAvail().x >= ImGui::GetFontSize() * kKindRoomEm;
	           // "project_files": a table id of its own since the Kind column shows by default, so a layout an
	           // earlier editor saved with it hidden (its default then) does not hide it (the UX round's project lane).
	           ImGui::BeginTable("project_files", 3,
	                             ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable | ImGuiTableFlags_RowBg |
	                                     ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
		// The name takes what the others leave; the kind as wide as "Animation map" (a longer label cut,
		// whole in its tooltip), hidden from the header's menu (a right click), and giving way to the name
		// in a narrow dock (as the dock crosses the width, the author's choice standing in between); the
		// size as wide as "999.9 KB".
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide);
		ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, ui_kit::text_width("Animation map"));
		// By cost, the size the game's textures of the file take ("In game"), in the column's place.
		ImGui::TableSetupColumn(narrowed && by_cost_ ? "In game###size" : "Size###size", ImGuiTableColumnFlags_WidthFixed,
		                        ui_kit::text_width("999.9 KB"));
		if (kind_fits != kind_fitted_) {
			kind_fitted_ = kind_fits;
			ImGui::TableSetColumnEnabled(1, kind_fits);
		}
		ImGui::TableHeadersRow();
		if (narrowed) {
			// Many rows (every texture of a game): only those that show are drawn.
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(matches.size()));
			if (!scroll_to_.empty())
				for (size_t i = 0; i < matches.size(); ++i)
					if (v.project.scan->entries[matches[i]].relative_path == scroll_to_) clipper.IncludeItemByIndex(int(i));
			while (clipper.Step())
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
					draw_file(v, v.project.scan->entries[matches[size_t(i)]], false);
		} else {
			draw_folder(v, folders_.front());
		}
		ImGui::EndTable();
	}
	draw_rename(v);
}

void FilesWindow::draw_card_window() {
	const SessionView &v = workspace_.view();
	// The card is the workspace's (the session closes it with its project).
	if (!v.project.open) return;
	draw_card(v);
}

void FilesWindow::draw_kind_filter(const SessionView &view, float width) {
	// The kinds the project has files of, each with how many, in the rows' order.
	std::vector<size_t> counts(kAssetKindCount, 0);
	for (const AssetEntry &entry : view.project.scan->entries)
		if (static_cast<size_t>(entry.kind) < kAssetKindCount) ++counts[static_cast<size_t>(entry.kind)];
	const std::string shown = kind_shown_ == AssetKind::kCount ? std::string("Every kind")
	                          : std::string(asset_kind_label(kind_shown_));
	ImGui::SetNextItemWidth(width);
	if (ImGui::BeginCombo("##kind", shown.c_str(), ImGuiComboFlags_HeightLarge)) {
		const AssetKind before = kind_shown_;
		if (ImGui::Selectable("Every kind", kind_shown_ == AssetKind::kCount)) kind_shown_ = AssetKind::kCount;
		for (size_t i = 0; i < kAssetKindCount; ++i) {
			if (!counts[i]) continue;
			const AssetKind kind = static_cast<AssetKind>(i);
			const std::string label = std::string(asset_kind_label(kind)) + " (" + grouped(counts[i]) + ")###" + asset_kind_token(kind);
			if (ImGui::Selectable(label.c_str(), kind_shown_ == kind)) kind_shown_ = kind;
		}
		ImGui::EndCombo();
		if (kind_shown_ != before) send_filter(false, true);
	}
	ui_kit::tooltip(kind_shown_ == AssetKind::kCount ? "Only the files of one kind." : "Only the " + shown + " files: Every kind shows them all.");
}

// Import, New and Refresh, wrapping in a narrow dock; each item enabled while the busy gate takes
// its request too (SessionView::allows: a build packing the files refuses a new file, a Refresh
// and a reimport).
void FilesWindow::draw_toolbar(const SessionView &view) {
	ui_kit::WrapRow row;
	const float arrow = ImGui::GetFrameHeight();
	const float import_width = ui_kit::button_width("Import") + arrow;
	row.next(import_width);
	ImGui::SetNextItemWidth(import_width);
	const bool importing = ImGui::BeginCombo("##import", "Import", ImGuiComboFlags_HeightLargest);
	if (!importing) ui_kit::tooltip("Copy files into the project: from the disk, from the game data, or every source again.");
	if (importing) {
		const bool picks = view.allows(EditorRequestKind::PreviewImport);
		ImGui::BeginDisabled(!picks);
		if (ImGui::Selectable("Files...") && picks)
			workspace_.request(request::pick_file(PickPurpose::ImportFiles));
		ImGui::EndDisabled();
		ui_kit::tooltip("Files from the disk, or what a PFF archive holds.");
		const bool game_data =
		        !view.project.retail_directory.empty() && view.allows(EditorRequestKind::PreviewInstallImport);
		// A project that holds missions is offered the whole game install first (ADR 0046 S14).
		const bool missions = view.project.document && view.project.document->features.mission;
		const auto whole_install = [&] {
			ImGui::BeginDisabled(!game_data);
			if (ImGui::Selectable("The whole game install...") && game_data)
				workspace_.request(request::import_whole_install());
			ImGui::EndDisabled();
			ui_kit::tooltip(!view.project.retail_directory.empty()
			                        ? "Every file of the game install, copied into the project: what a mission project needs to "
			                          "play, build and resolve every name."
			                        : "Choose the game install folder in File > Project settings... first.");
		};
		if (missions) whole_install();
		ImGui::BeginDisabled(!game_data);
		if (ImGui::Selectable("From the game data...") && game_data)
			workspace_.request(
					request::preview_install_import({}, view.project.import_dependencies));
		ImGui::EndDisabled();
		ui_kit::tooltip(!view.project.retail_directory.empty() ? "Files of the game install, copied into the project."
		                                               : "Choose the game install folder in File > Project settings... first.");
		if (!missions) whole_install();
		const bool reimports =
				!view.project.imports->empty() && view.allows(EditorRequestKind::Reimport);
		ImGui::BeginDisabled(!reimports);
		if (ImGui::Selectable("Reimport all") && reimports)
			workspace_.request(request::reimport(std::string(), true));
		ImGui::EndDisabled();
		ui_kit::tooltip(view.project.imports->empty()
						? "No file of the project is imported from a source."
						: "Run every importer again (" +
								std::to_string(view.project.imports->size()) +
								(view.project.imports->size() == 1 ? " source)." : " sources)."));
		ImGui::EndCombo();
	}
	const float new_width = ui_kit::button_width("New") + arrow;
	row.next(new_width);
	ImGui::SetNextItemWidth(new_width);
	const bool making = ImGui::BeginCombo("##new", "New", ImGuiComboFlags_HeightLargest);
	if (!making)
		ui_kit::tooltip("Make a new file: a string table, a menu, a font, a placeholder texture, or a file the game reads "
		                "by name.");
	if (making) {
		const bool creates = view.allows(EditorRequestKind::CreateFile);
		// A new file of a kind with a free-form factory: its name is asked first.
		for (size_t i = 0; i < blank_factory_count(); ++i) {
			const BlankFactory &factory = *blank_factory_at(i);
			if (!factory.free_form || factory.role[0] != '\0') continue;
			ImGui::PushID(static_cast<int>(i));
			ImGui::BeginDisabled(!creates);
			if (ImGui::Selectable((std::string(asset_kind_label(factory.kind)) + "...").c_str()) && creates)
				NewFilePrompt::ask(workspace_, factory.kind);
			ImGui::EndDisabled();
			ui_kit::tooltip(makes(factory));
			ImGui::PopID();
		}
		// A terrain made from images (S20): its name, its images and numbers asked by the same prompt.
		ImGui::BeginDisabled(!view.allows(EditorRequestKind::NewTerrain));
		if (ImGui::Selectable("Terrain from images...") && view.allows(EditorRequestKind::NewTerrain))
			NewFilePrompt::ask(workspace_, AssetKind::Terrain);
		ImGui::EndDisabled();
		ui_kit::tooltip("A terrain made from a heightmap and a colour map (and a detail, a tile set and a surface map): "
		                "imported into the files the game reads for a terrain, and imported again when an image changes.");
		// The files the game reads by name, each made at once from its role's factory.
		ImGui::SeparatorText("Files the game reads");
		for (size_t i = 0; i < blank_factory_count(); ++i) {
			const BlankFactory &factory = *blank_factory_at(i);
			const gameprofile::RequiredResource *resource = gameprofile::gameprofile_required_resource_by_role(factory.role);
			if (!resource) continue;
			const bool present = view.project.scan->find(resource->name) != nullptr;
			ImGui::PushID(static_cast<int>(i));
			ImGui::BeginDisabled(present || !creates);
			if (ImGui::Selectable(resource->name) && !present && creates)
				workspace_.request(request::create_file(resource->name, asset_kind_token(factory.kind)));
			ImGui::EndDisabled();
			ui_kit::tooltip(present ? std::string("The project has it.") : makes(factory));
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	if (ui_kit::tool(row, "Refresh", view.allows(EditorRequestKind::Rescan), "Read the project's folder again."))
		workspace_.request(request::rescan());
}

// A folder's folders, each a node open until folded, then its files.
void FilesWindow::draw_folder(const SessionView &view, const Folder &folder) {
	for (const size_t index : folder.folders) {
		const Folder &inner = folders_[index];
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		const std::string label = ui_kit::fit(inner.name, ImGui::GetContentRegionAvail().x - ImGui::GetTreeNodeToLabelSpacing());
		if (!open_to_.empty() && open_to_.rfind(inner.path + "/", 0) == 0) ImGui::SetNextItemOpen(true);
		const bool open = ImGui::TreeNodeEx(inner.path.c_str(), ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_DefaultOpen,
		                                    "%s", label.c_str());
		ui_kit::tooltip(inner.path);
		if (!open) continue;
		draw_folder(view, inner);
		ImGui::TreePop();
	}
	for (const size_t index : folder.files)
		draw_file(view, view.project.scan->entries[index], true);
}

// A file's row: a selectable under the whole row, its name (cut to what its dot and counts
// leave), its kind and its size drawn over it.
void FilesWindow::draw_file(const SessionView &view, const AssetEntry &entry, bool in_tree) {
	ImGui::PushID(entry.relative_path.c_str());
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	const DocumentBase *open = open_document(view, entry.relative_path);
	const bool dirty = open && open->dirty();
	const auto found = counts_.find(entry.relative_path);
	const Counts counts = found != counts_.end() ? found->second : Counts();
	if (ImGui::Selectable("##row", entry.relative_path == selected_,
	                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
	                              ImGuiSelectableFlags_AllowOverlap)) {
		selected_ = entry.relative_path;
		// The selection is the session's too (S18): a texture so selected shows in the Preview window
		// (again, once another document was made active since).
		const bool previews = file_preview_kind(asset_kind_row(entry.kind).document) != ViewportKind::kCount;
		if (view.allows(EditorRequestKind::SelectFile) &&
		    (view.documents.file_selected.path != entry.relative_path || (previews && !view.documents.files_lead)))
			workspace_.request(request::select_file(entry.relative_path));
		// A double click opens what the editor opens, and says what any other file is (its card: the UX
		// round's project lane).
		if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
			if (!is_editable_kind(entry.kind)) workspace_.request(request::about_file(entry.relative_path));
			else if (view.allows(EditorRequestKind::OpenDocument)) workspace_.request(request::open_document(entry.relative_path));
		}
	}
	// Dragged onto a reference field whose kind loads it, the file becomes its value
	// (ReferencePicker::accept_file).
	if (ImGui::BeginDragDropSource()) {
		ImGui::SetDragDropPayload(kFileDragPayload, entry.relative_path.c_str(), entry.relative_path.size() + 1);
		ImGui::TextUnformatted(entry.logical_name.c_str());
		ImGui::EndDragDropSource();
	}
	if (entry.relative_path == scroll_to_) {
		ImGui::SetScrollHereY(0.5f);
		scroll_to_.clear();
		open_to_.clear();
	}
	draw_file_menu(view, entry);
	// Its path, kind and size, made only while its tooltip shows; a texture's picture above them (S18).
	const auto tip = [&] {
		std::string tip = entry.relative_path + "\n" + asset_kind_label(entry.kind) + ", " +
		                  size_text(entry.size_bytes);
		if (!entry.imported_from.empty()) tip += "\nImported from " + entry.imported_from;
		if (dirty) tip += "\nUnsaved changes";
		const auto said = [](size_t errors, size_t warnings) {
			return std::to_string(errors) + (errors == 1 ? " error, " : " errors, ") + std::to_string(warnings) +
			       (warnings == 1 ? " warning" : " warnings");
		};
		if (counts.errors || counts.warnings) tip += "\n" + said(counts.errors, counts.warnings) + ": see Problems";
		if (counts.original_errors || counts.original_warnings)
			tip += "\n" + said(counts.original_errors, counts.original_warnings) +
			       " in the game's own data (the game as it ships has them too)";
		return tip;
	};
	if (entry.kind != AssetKind::Texture) ui_kit::tooltip_lazy(tip);
	else if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
		texture_preview::tooltip(workspace_, entry.relative_path, TextureLoadTransform::None, [&] {
			// What the game's texture of it costs (S18, the texture budget).
			const std::string cost = texture_file_budget_words(view, entry.relative_path);
			return cost.empty() ? tip() : tip() + "\n" + cost;
		}());
	ImGui::SameLine(0.0f, 0.0f);
	if (in_tree) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetTreeNodeToLabelSpacing());
	const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
	float marks = 0.0f;
	if (dirty) marks += spacing + ui_kit::unsaved_dot_width();
	if (counts.errors) marks += spacing + ui_kit::severity_count_width(counts.errors);
	if (counts.warnings) marks += spacing + ui_kit::severity_count_width(counts.warnings);
	// A file the filter found by a record naming it: that record after its name, muted ("Dblkhwk1.3di
	// Flyable Blackhawk").
	const auto via = in_tree ? via_.end() : via_.find(entry.relative_path);
	const std::string name = ui_kit::fit(entry.logical_name, ImGui::GetContentRegionAvail().x - marks);
	if (open) ImGui::TextColored(kOpenColor, "%s", name.c_str());
	else ImGui::TextUnformatted(name.c_str());
	if (via != via_.end()) {
		ImGui::SameLine(0.0f, spacing * 2.0f);
		const std::string by = ui_kit::fit(via->second, ImGui::GetContentRegionAvail().x - marks);
		ImGui::TextDisabled("%s", by.c_str());
		ui_kit::tooltip(entry.logical_name + " is named by " + via->second + ", whose name the filter holds.");
	}
	if (dirty) {
		ImGui::SameLine(0.0f, spacing);
		ui_kit::unsaved_dot();
	}
	if (counts.errors) {
		ImGui::SameLine(0.0f, spacing);
		ui_kit::severity_count(DiagnosticSeverity::Error, counts.errors);
	}
	if (counts.warnings) {
		ImGui::SameLine(0.0f, spacing);
		ui_kit::severity_count(DiagnosticSeverity::Warning, counts.warnings);
	}
	if (ImGui::TableNextColumn()) ui_kit::clipped_text(asset_kind_label(entry.kind));
	// The size, right-aligned; listed by cost, what the game's textures of it take (none: blank).
	ImGui::TableNextColumn();
	std::string shown_size = size_text(entry.size_bytes);
	if (by_cost_ && !in_tree) {
		const auto cost = costs_.find(entry.relative_path);
		shown_size = cost == costs_.end() ? std::string() : texture_bytes_words(cost->second);
	}
	const std::string size = ui_kit::fit(shown_size, ImGui::GetContentRegionAvail().x);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ui_kit::text_width(size.c_str()));
	ImGui::TextUnformatted(size.c_str());
	ImGui::PopID();
}

// A file's menu (a right click on its row).
void FilesWindow::draw_file_menu(const SessionView &view, const AssetEntry &entry) {
	if (!ImGui::BeginPopupContextItem("file_menu")) return;
	// The file the menu is of is the one selected, the session's too (a right click selects it, as a click
	// does).
	if (ImGui::IsWindowAppearing()) {
		selected_ = entry.relative_path;
		if (view.documents.file_selected.path != entry.relative_path && view.allows(EditorRequestKind::SelectFile))
			workspace_.request(request::select_file(entry.relative_path));
	}
	const bool opens = is_editable_kind(entry.kind) && view.allows(EditorRequestKind::OpenDocument);
	if (ImGui::MenuItem("Open", nullptr, false, opens) && opens)
		workspace_.request(request::open_document(entry.relative_path));
	const bool renames = view.allows(EditorRequestKind::RenameAsset);
	if (ImGui::MenuItem("Rename...", "F2", false, renames) && renames) start_rename(entry);
	if (ImGui::MenuItem("About this file...")) workspace_.request(request::about_file(entry.relative_path));
	ui_kit::tooltip("What it is, where a build puts it, what it names and who names it.");
	const bool reveals = view.allows(EditorRequestKind::RevealPath);
	if (ImGui::MenuItem("Show in folder", nullptr, false, reveals) && reveals)
		workspace_.request(request::reveal_path(join_path(view.project.root, entry.relative_path)));
	const bool reimports = view.allows(EditorRequestKind::Reimport);
	const bool source = entry.kind == AssetKind::ImportSource;
	if (source && ImGui::MenuItem("Import again", nullptr, false, reimports) && reimports)
		workspace_.request(request::reimport(entry.relative_path, true));
	ImGui::EndPopup();
}

void FilesWindow::start_rename(const AssetEntry &entry) {
	previewed_.clear();
	window_requests::set_workspace(workspace_, "file_rename", "path", io::JsonValue::make_string(entry.relative_path));
}

// Rename... (the workspace's file_rename): the new name; every file naming the old one is rewritten, or the
// rename is refused with the reasons in Problems.
void FilesWindow::draw_rename(const SessionView &view) {
	const WorkspaceView::FileRename &held = view.workspace.file_rename;
	rename_.follow(held.name);
	const AssetEntry *entry = held.path.empty() ? nullptr : entry_at(view, held.path);
	const auto close = [this] { window_requests::set_workspace(workspace_, "file_rename", "path", io::JsonValue::make_string("")); };
	// A popup at the top level, as the modals are: it shows when none before it in the session's order is held.
	if (!rename_popup_.begin("Rename", entry != nullptr && modal_may_show(view, HeldModal::FileRename), false, 0, false,
	                         view.workspace.opened)) {
		if (rename_popup_.dismissed()) close();
		return;
	}
	ImGui::Text("Rename %s to", entry->logical_name.c_str());
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 17.0f);
	const bool enter = ImGui::InputText("##name", rename_.text, sizeof(rename_.text), ImGuiInputTextFlags_EnterReturnsTrue);
	if (ImGui::IsItemEdited() && rename_.sent() != held.name)
		window_requests::set_workspace(workspace_, "file_rename", "name", io::JsonValue::make_string(rename_.sent()));
	const char *name = rename_.text;
	const bool changed = name[0] != '\0' && entry->logical_name != name;
	// What it would rewrite, or why it would be refused, planned as the name is typed (while the
	// busy gate takes the plan; a rename, which writes the files, waits for a build).
	const std::string asked = entry->relative_path + '\n' + name;
	if (changed && previewed_ != asked && view.allows(EditorRequestKind::PreviewRename)) {
		previewed_ = asked;
		workspace_.request(request::preview_file_rename(entry->relative_path, name));
	}
	const DialogsView::RenamePreview &plan = view.dialogs.rename_preview;
	if (changed && !plan.symbol && plan.path == entry->relative_path && plan.requested == name) {
		for (const Diagnostic &refusal : plan.refusals) {
			ui_kit::severity_marker(refusal.severity);
			ImGui::SameLine();
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 24.0f);
			ImGui::TextWrapped("%s", refusal.message.c_str());
			ImGui::PopTextWrapPos();
		}
		const std::vector<RenameSite> &sites = *plan.sites;
		if (plan.refusals.empty())
			ImGui::TextDisabled("%s", sites.empty() ? "Nothing names it." :
			                          ("Rewrites " + std::to_string(sites.size()) + " reference" +
			                           (sites.size() == 1 ? ":" : "s:")).c_str());
		for (size_t i = 0; i < sites.size() && i < 12; ++i) {
			// The record and the field in words (the plain-words lane, the audit's 8.3).
			const RenameSite &site = sites[i];
			const std::string place = rename_site_place(site);
			ImGui::BulletText("%s", (site.file + ": " + (place.empty() ? std::string() : place + ": ") + site.before + " -> " +
			                         site.after).c_str());
		}
		if (sites.size() > 12) ImGui::TextDisabled("and %zu more", sites.size() - 12);
	}
	const bool allowed = view.allows(EditorRequestKind::RenameAsset);
	ImGui::BeginDisabled(!changed || !allowed);
	const bool rename = ImGui::Button("Rename");
	ImGui::EndDisabled();
	ui_kit::tooltip(allowed ? "Every file naming it is rewritten, or the rename is refused (Problems says why)."
	                        : "A rename rewrites the project's files: it waits for the running operation.");
	if ((rename || enter) && changed && allowed) {
		// The session closes Rename... as it takes the rename (rename_asset alone, over the wire); its button
		// closes it too.
		workspace_.request(request::rename_asset(entry->relative_path, name));
		close();
		rename_popup_.close();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) {
		close();
		rename_popup_.close();
	}
	ImGui::EndPopup();
}

// The card closed (its X): the workspace's card closed, which stops the sound it played.
void FilesWindow::close_card() {
	if (card_closing_ == workspace_.view().workspace.card.path) return; // asked already
	card_closing_ = workspace_.view().workspace.card.path;
	window_requests::set_workspace(workspace_, "card", "path", io::JsonValue::make_string(""));
}

// The card (session/file_card.h) of the file the workspace's card names, made again when that file, the
// files, the graph or the project move, and when the project's references start or end being read: a
// window of its own kept in the editor's, as the build result is, until it is closed.
void FilesWindow::draw_card(const SessionView &view) {
	const std::string &path = view.workspace.card.path;
	if (path != card_path_) {
		card_path_ = path;
		card_.reset();
		card_closing_.clear();
		card_focus_ = !path.empty(); // a card opened comes forward
	}
	if (card_path_.empty()) return;
	const RevisionKey key = revision_key(view.revisions, {ViewConcern::Files, ViewConcern::Graph, ViewConcern::Project});
	const bool reading = !view.activity.validation.read || view.activity.validation.files_unread; // file_card's
	if (!card_ || card_key_ != key || card_->reading != reading) {
		card_key_ = key;
		// A wave's sound as it was read, while its file stands (file_card reads it again when it moved).
		const FileCard::Sound *known = card_ && card_->wave ? &card_->sound : nullptr;
		card_ = std::make_shared<const FileCard>(file_card(view, card_path_, known));
	}
	const FileCard &card = *card_;
	// Its file gone: the session closes the card as the files lose it (workspace_tidies); nothing drawn meanwhile.
	if (!card.found) return;
	const ImGuiViewport *viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowViewport(viewport->ID);
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 36.0f, std::max(viewport->WorkSize.y * 0.6f, ImGui::GetFontSize() * 20.0f)),
	                         ImGuiCond_Appearing);
	ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 20.0f, 0.0f),
	                                    ImVec2(FLT_MAX, std::max(viewport->WorkSize.y * 0.8f, ImGui::GetFontSize() * 20.0f)));
	if (card_focus_) {
		ImGui::SetNextWindowFocus();
		card_focus_ = false;
	}
	bool open = true;
	const std::string title = "About " + card.name + "###file_card";
	if (!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
	                                             ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		if (!open) close_card();
		return;
	}
	ImGui::PushTextWrapPos(0.0f);
	ImGui::Text("%s, %s", card.kind_label.c_str(), size_text(card.size).c_str());
	ImGui::TextDisabled("%s", card.path.c_str());
	ImGui::Spacing();
	ImGui::TextWrapped("%s", card.about.c_str());
	ImGui::TextWrapped("%s", card.build.c_str());
	if (!card.imported_from.empty()) ImGui::TextWrapped("Made from %s by its import.", card.imported_from.c_str());
	ImGui::PopTextWrapPos();
	// A wave's sound, as the game decodes it, played by the editor.
	if (card.wave) {
		ImGui::Spacing();
		if (card.sound.decoded) {
			char words[96];
			std::snprintf(words, sizeof(words), "%s, %s Hz, %.1f s", card.sound.channels == 1 ? "Mono" : "Stereo",
			              grouped(card.sound.rate).c_str(), card.sound.seconds);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(words);
			ImGui::SameLine();
			if (ImGui::Button("Play##card")) workspace_.request(request::play_sound(card.path));
			ui_kit::tooltip("Play it as the game decodes it.");
			ImGui::SameLine();
			if (ImGui::Button("Stop##card")) workspace_.request(request::stop_sound());
			// How the sound goes, as the Shell reports it (the workspace's sound).
			const WorkspaceView::Sound &sound = view.workspace.sound;
			using State = WorkspaceView::SoundState;
			if (sound.path == card.path && sound.state != State::Idle) {
				const std::string said = sound.state == State::Starting  ? std::string("Starting...")
				                         : sound.state == State::Playing ? std::string("Playing")
				                         : sound.state == State::Ended   ? std::string("Played")
				                         : sound.state == State::Stopped ? std::string("Stopped")
				                                                         : "Does not play: " + sound.error;
				ImGui::SameLine();
				ImGui::TextDisabled("%s", said.c_str());
			}
		} else {
			ImGui::PushStyleColor(ImGuiCol_Text, kRefusalColor);
			ImGui::TextWrapped("The game cannot play it: %s", card.sound.error.c_str());
			ImGui::PopStyleColor();
		}
	}
	ImGui::Spacing();
	if (card.opens) {
		ImGui::BeginDisabled(!view.allows(EditorRequestKind::OpenDocument));
		if (ImGui::Button("Open##card")) workspace_.request(request::open_document(card.path));
		ImGui::EndDisabled();
		ImGui::SameLine();
	}
	ImGui::BeginDisabled(!view.allows(EditorRequestKind::RevealPath));
	if (ImGui::Button("Show in folder##card")) workspace_.request(request::reveal_path(join_path(view.project.root, card.path)));
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!view.allows(EditorRequestKind::RenameAsset));
	if (ImGui::Button("Rename...##card"))
		if (const AssetEntry *entry = entry_at(view, card.path)) start_rename(*entry);
	ImGui::EndDisabled();
	// What it names: a click goes to the file it resolves to; a wave it names plays. While the project's
	// references are being read, the counts say so (the lists are the graph's as far as it has read).
	const auto count = [&card](size_t n) { return card.reading ? std::string("being read") : grouped(n); };
	const std::string names = "It names (" + count(card.names.size()) + ")###names";
	// A kind whose files name none (a wave, a texture) says nothing of it.
	const bool names_any = !card.names.empty() || asset_kind_row(card.kind).names_files;
	if (names_any && ImGui::CollapsingHeader(names.c_str(), card.names.size() <= 200 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
		if (card.names.empty())
			ui_kit::empty_state(card.reading ? "The project's references are still being read." : "No other file or record.");
		else if (ImGui::BeginTable("names", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("What", ImGuiTableColumnFlags_WidthStretch, 2.0f);
			ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
			ImGui::TableSetupColumn("##state", ImGuiTableColumnFlags_WidthStretch, 1.0f);
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(card.names.size()));
			while (clipper.Step())
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
					const FileCard::Named &named = card.names[size_t(i)];
					ImGui::PushID(i);
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					const std::string what = named.record.empty() ? named.field : named.record + " - " + named.field;
					ui_kit::clipped_text(what);
					ImGui::TableNextColumn();
					const bool goes = !named.target.file.empty();
					if (goes) {
						if (ImGui::Selectable((ui_kit::fit(named.value, ImGui::GetContentRegionAvail().x) + "###go").c_str()))
							window_requests::go_to(workspace_, named.target);
						ui_kit::tooltip(named.target.editable ? "Open " + named.file + " at it." : "Show " + named.file + " in Files.");
					} else {
						ui_kit::clipped_text(named.value);
					}
					ImGui::TableNextColumn();
					if (named.wave) {
						if (ImGui::SmallButton("Play")) workspace_.request(request::play_sound(named.file));
						ui_kit::tooltip("Play " + named.file + ".");
					} else if (named.status != ReferenceStatus::NotAReference) {
						ImGui::TextColored(ui_kit::reference_color(named.status), "%s", reference_status_words(named.status));
					}
					ImGui::PopID();
				}
			ImGui::EndTable();
		}
	}
	// Who names it, or what it defines: a click goes to the use.
	const std::string users = "Named by (" + count(card.named_by.size()) + ")###users";
	if (ImGui::CollapsingHeader(users.c_str(), card.named_by.size() <= 200 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
		if (card.named_by.empty())
			ui_kit::empty_state(card.reading ? "The project's references are still being read."
			                                 : "No file of the project names it or what it defines.");
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(card.named_by.size()));
		while (clipper.Step())
			for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
				const FileCard::User &user = card.named_by[size_t(i)];
				ImGui::PushID(i);
				const std::string line = user.file + ": " + (user.record.empty() ? "" : user.record + " - ") + user.field;
				if (ImGui::Selectable((ui_kit::fit(line, ImGui::GetContentRegionAvail().x) + "###use").c_str()))
					window_requests::go_to(workspace_, user.target);
				ui_kit::tooltip(line + "\n" + (user.target.editable ? "Open " + user.target.file + " at it." : "Show " + user.target.file + " in Files."));
				ImGui::PopID();
			}
	}
	ImGui::End();
	if (!open) close_card();
}

} // namespace opennova::editor
