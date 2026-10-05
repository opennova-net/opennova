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
#include <editor/model/field_text.h>
#include <editor/preview/viewport_kinds.h>
#include <editor/project/project_files.h>
#include <editor/session/file_card.h>
#include <editor/session/problem_query.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
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

void NewFilePrompt::ask(AssetKind kind) {
	ask_ = true;
	kind_ = kind;
	name_[0] = '\0';
	values_.clear();
}

void NewFilePrompt::draw(Workspace &workspace) {
	if (ask_) {
		ask_ = false;
		ImGui::OpenPopup("New file");
	}
	if (!ImGui::BeginPopupModal("New file", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
	const SessionView &v = workspace.view();
	if (!v.project.open) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	ImGui::Text("New file: %s", asset_kind_label(kind_));
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 21.0f);
	// The name a new file of the kind is offered: its row's (AssetKindRow::new_name).
	const bool enter = ImGui::InputTextWithHint("Name", asset_kind_row(kind_).new_name, name_, sizeof(name_),
	                                            ImGuiInputTextFlags_EnterReturnsTrue);
	// The project's name rules, as the session checks them again when it creates the file; a
	// texture is the texture factory's placeholder, made only for a name it takes.
	FileNameProblem problem = FileNameProblem::None;
	std::string message;
	const bool named = name_[0] != '\0';
	bool fits = named && check_file_name(name_, kind_, problem, message);
	if (fits && kind_ == AssetKind::Texture) fits = can_make_blank_texture(name_, message);
	const bool taken = named && v.project.scan->find(name_) != nullptr;
	if (named && !fits) ImGui::TextColored(kRefusalColor, "%s", message.c_str());
	else if (taken) ImGui::TextColored(kRefusalColor, "The project has a file named %s already.", name_);
	// What the kind's blank takes beside its name (a mission's title, terrain and environment): a
	// text, or one of the project's files its reference loads. A project is its own files, so a
	// kind the project has no file of says to import one.
	const BlankFactory *factory = find_blank_factory_for_kind(kind_);
	const size_t params = factory ? factory->param_count : 0;
	if (values_.size() != params) values_.assign(params, std::string());
	bool given = true;
	for (size_t i = 0; i < params; ++i) {
		const BlankParam &param = factory->params[i];
		ImGui::PushID(static_cast<int>(i));
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 21.0f);
		if (param.reference == ReferenceKind::None) {
			char text[64];
			std::snprintf(text, sizeof(text), "%s", values_[i].c_str());
			if (ImGui::InputText(param.label, text, sizeof(text))) values_[i] = text;
		} else {
			const AssetKind wanted = reference_row(param.reference).file;
			size_t offered = 0;
			if (ImGui::BeginCombo(param.label, values_[i].empty() ? "Choose..." : values_[i].c_str())) {
				for (const AssetEntry &entry : v.project.scan->entries) {
					if (entry.kind != wanted) continue;
					++offered;
					if (ImGui::Selectable(entry.logical_name.c_str(), entry.logical_name == values_[i]))
						values_[i] = entry.logical_name;
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
	const bool ready = fits && !taken && given && v.allows(EditorRequestKind::CreateFile);
	ImGui::BeginDisabled(!ready);
	const bool create = ImGui::Button("Create");
	ImGui::EndDisabled();
	if ((create || enter) && ready) {
		std::vector<std::pair<std::string, std::string>> values;
		for (size_t i = 0; i < params; ++i)
			if (!values_[i].empty()) values.emplace_back(factory->params[i].token, values_[i]);
		workspace.request(request::create_file(name_, asset_kind_token(kind_), std::move(values)));
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
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

const std::vector<size_t> &FilesWindow::matching(const SessionView &view) {
	if (filter_[0] == '\0' && kind_shown_ == AssetKind::kCount) {
		matches_.clear();
		matches_made_ = false;
		return matches_;
	}
	const uint64_t generation = view.findings.graph ? view.findings.graph->generation() : 0;
	const std::string asked = std::string(filter_) + '\n' + std::to_string(static_cast<int>(kind_shown_));
	if (matches_made_ && matched_ == asked && matched_generation_ == generation) return matches_;
	matches_made_ = true;
	matched_ = asked;
	matched_generation_ = generation;
	via_.clear();
	// The paths holding the text, then the files of a kind it names ("texture" lists the textures), each of
	// the kind chosen (match_files, the files query's own rule).
	matches_ = match_files(*view.project.scan, filter_, kind_shown_);
	std::vector<bool> listed(view.project.scan->entries.size(), false);
	for (const size_t i : matches_) listed[i] = true;
	// Then the files a record naming them is found by, after them: a model by the item whose graphic it
	// is (lack finds Dblkhwk1.3di through Flyable Blackhawk), from three letters on.
	// Each once: a file its folder's name already lists is not listed again (the graph's search finds by
	// record from three letters on).
	if (view.findings.graph && filter_[0] != '\0') {
		std::unordered_map<std::string, size_t> at;
		for (const GraphSearchHit &hit : view.findings.graph->search(filter_)) {
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
	return matches_;
}

bool FilesWindow::stands_aside() const { return aside_for_welcome(workspace_.view(), welcome_asked_); }

void FilesWindow::receive(const ViewEvent &event) {
	events_.post(event);
	request_focus();
	// An AboutFile's card opens now, Files drawn or not.
	if (event.kind == ViewEventKind::RevealFile && event.tag == 1) open_card(event.path);
}

// The file an ask names, selected; a filter that hides it cleared; the folders on its way
// (an imported file's are its source's) to open and the row to scroll to as they draw.
void FilesWindow::show_revealed(const SessionView &view, const ViewEvent &event) {
	const AssetEntry *entry = entry_at(view, event.path);
	if (!entry) return;
	selected_ = entry->relative_path;
	// A filter or a kind that hides it cleared.
	const std::vector<size_t> &shown = matching(view);
	const bool hidden = (filter_[0] != '\0' || kind_shown_ != AssetKind::kCount) &&
	                    std::none_of(shown.begin(), shown.end(),
	                                 [&](size_t i) { return view.project.scan->entries[i].relative_path == entry->relative_path; });
	if (hidden) {
		filter_[0] = '\0';
		kind_shown_ = AssetKind::kCount;
	}
	scroll_to_ = entry->relative_path;
	open_to_ = entry->imported_from.empty() ? entry->relative_path : entry->imported_from;
	if (event.flag) start_rename(*entry);
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
	// Another project: its own filter, kind and selection (none yet).
	if (v.project.root != shown_root_) {
		shown_root_ = v.project.root;
		filter_[0] = '\0';
		kind_shown_ = AssetKind::kCount;
		selected_.clear();
	}
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
	const bool narrowed = filter_[0] != '\0' || kind_shown_ != AssetKind::kCount;
	const size_t count = narrowed ? matches.size() : v.project.scan->entries.size();
	const std::string files = (narrowed ? grouped(count) + " of " + grouped(v.project.scan->entries.size()) : grouped(count)) +
	                          (v.project.scan->entries.size() == 1 ? " file" : " files");
	{
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		const float kind = ImGui::GetFontSize() * 8.0f;
		const float field = std::max(ImGui::GetFontSize() * 8.0f,
		                             ImGui::GetContentRegionAvail().x - kind - ui_kit::text_width(files.c_str()) - spacing * 2.0f);
		ui_kit::WrapRow row;
		row.next(field);
		ui_kit::filter_box("##filter", filter_, sizeof(filter_), "Filter files", field,
		                   "A name, a folder, or a kind (texture, wave, model...): the files of the kind follow those "
		                   "whose names hold the text.");
		row.next(kind);
		draw_kind_filter(v, kind);
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
		ui_kit::empty_state(kind_shown_ != AssetKind::kCount && filter_[0] == '\0' ? "The project has no file of this kind."
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
		ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ui_kit::text_width("999.9 KB"));
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
	// The card is its project's: closed with it, and when another opens.
	if (!v.project.open || (!card_path_.empty() && v.project.root != card_root_)) {
		close_card();
		return;
	}
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
		if (ImGui::Selectable("Every kind", kind_shown_ == AssetKind::kCount)) kind_shown_ = AssetKind::kCount;
		for (size_t i = 0; i < kAssetKindCount; ++i) {
			if (!counts[i]) continue;
			const AssetKind kind = static_cast<AssetKind>(i);
			const std::string label = std::string(asset_kind_label(kind)) + " (" + grouped(counts[i]) + ")###" + asset_kind_token(kind);
			if (ImGui::Selectable(label.c_str(), kind_shown_ == kind)) kind_shown_ = kind;
		}
		ImGui::EndCombo();
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
				new_file_.ask(factory.kind);
			ImGui::EndDisabled();
			ui_kit::tooltip(makes(factory));
			ImGui::PopID();
		}
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
			if (!is_editable_kind(entry.kind)) open_card(entry.relative_path);
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
		texture_preview::tooltip(workspace_, entry.relative_path, TextureLoadTransform::None, tip());
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
	// The size, right-aligned.
	ImGui::TableNextColumn();
	const std::string size = ui_kit::fit(size_text(entry.size_bytes), ImGui::GetContentRegionAvail().x);
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ui_kit::text_width(size.c_str()));
	ImGui::TextUnformatted(size.c_str());
	ImGui::PopID();
}

// A file's menu (a right click on its row).
void FilesWindow::draw_file_menu(const SessionView &view, const AssetEntry &entry) {
	if (!ImGui::BeginPopupContextItem("file_menu")) return;
	selected_ = entry.relative_path;
	const bool opens = is_editable_kind(entry.kind) && view.allows(EditorRequestKind::OpenDocument);
	if (ImGui::MenuItem("Open", nullptr, false, opens) && opens)
		workspace_.request(request::open_document(entry.relative_path));
	const bool renames = view.allows(EditorRequestKind::RenameAsset);
	if (ImGui::MenuItem("Rename...", "F2", false, renames) && renames) start_rename(entry);
	if (ImGui::MenuItem("About this file...")) open_card(entry.relative_path);
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
	renaming_ = entry.relative_path;
	previewed_.clear();
	const size_t n = std::min(entry.logical_name.size(), sizeof(rename_) - 1);
	std::memcpy(rename_, entry.logical_name.data(), n);
	rename_[n] = '\0';
	open_rename_ = true;
}

// Rename...: the new name; every file naming the old one is rewritten, or the rename is
// refused with the reasons in Problems.
void FilesWindow::draw_rename(const SessionView &view) {
	if (open_rename_) {
		open_rename_ = false;
		ImGui::OpenPopup("Rename");
	}
	if (!ImGui::BeginPopup("Rename")) return;
	const AssetEntry *entry = entry_at(view, renaming_);
	if (!entry) {
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		return;
	}
	ImGui::Text("Rename %s to", entry->logical_name.c_str());
	if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 17.0f);
	const bool enter = ImGui::InputText("##name", rename_, sizeof(rename_), ImGuiInputTextFlags_EnterReturnsTrue);
	const bool changed = rename_[0] != '\0' && entry->logical_name != rename_;
	// What it would rewrite, or why it would be refused, planned as the name is typed (while the
	// busy gate takes the plan; a rename, which writes the files, waits for a build).
	if (changed && previewed_ != rename_ && view.allows(EditorRequestKind::PreviewRename)) {
		previewed_ = rename_;
		workspace_.request(request::preview_file_rename(entry->relative_path, previewed_));
	}
	const DialogsView::RenamePreview &plan = view.dialogs.rename_preview;
	if (changed && !plan.symbol && plan.path == entry->relative_path && plan.requested == rename_) {
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
		workspace_.request(request::rename_asset(entry->relative_path, rename_));
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
}

void FilesWindow::open_card(const std::string &path) {
	if (card_path_ != path) close_card();
	card_path_ = path;
	card_root_ = workspace_.view().project.root;
	card_.reset();
	card_focus_ = true;
}

// The card closed: a sound its Play started stops with it.
void FilesWindow::close_card() {
	if (card_played_ && workspace_.view().allows(EditorRequestKind::StopSound)) workspace_.request(request::stop_sound());
	card_played_ = false;
	card_path_.clear();
	card_.reset();
}

// The card (session/file_card.h), made again when the files, the graph or the project move, and when the
// project's references start or end being read: a window of its own kept in the editor's, as the build
// result is, until it is closed.
void FilesWindow::draw_card(const SessionView &view) {
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
	if (!card.found) {
		close_card();
		return;
	}
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
			if (ImGui::Button("Play##card")) {
				workspace_.request(request::play_sound(card.path));
				card_played_ = true;
			}
			ui_kit::tooltip("Play it as the game decodes it.");
			ImGui::SameLine();
			if (ImGui::Button("Stop##card")) workspace_.request(request::stop_sound());
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
						if (ImGui::SmallButton("Play")) {
							workspace_.request(request::play_sound(named.file));
							card_played_ = true;
						}
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
